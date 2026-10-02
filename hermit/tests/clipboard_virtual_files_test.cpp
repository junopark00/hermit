// Standalone test for app/streaming/clipboardvirtualfiles.h: the data object's formats and the
// file streams, which download from a loopback HTTP server that stands in for the host. The data
// object is called directly on a COM apartment thread; the system clipboard is never touched.
#define SDL_MAIN_HANDLED  // keep this main() (SDL would rename it)
#include "streaming/clipboardvirtualfiles.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

using namespace ClipboardVirtualFiles;
using ClipboardArchive::RemoteFile;
using ClipboardArchive::RemoteFileList;

static std::atomic<int> g_Failures {0};
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_Failures++; } } while (0)

static QByteArray randomBytes(qsizetype n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (qsizetype i = 0; i < n; i++) b[i] = char(QRandomGenerator::global()->bounded(256));
    return b;
}

// Answers GET /actions/clipboard?type=filedata&snapshot=..&index=..&offset=.. like Shell does.
struct FakeHost
{
    QTcpServer server;
    QMap<int, QByteArray> files;   // index -> data
    QByteArray snapshot = "abc123";
    qint64 cutAt = -1;             // the first request for cutIndex ends after this many body bytes
    int cutIndex = -1;
    std::mutex mutex;
    QList<QPair<int, qint64>> requests;  // (index, offset)

    void start()
    {
        server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* sock = server.nextPendingConnection()) {
                auto* buf = new QByteArray;
                QObject::connect(sock, &QTcpSocket::disconnected, sock, [sock, buf]() { delete buf; sock->deleteLater(); });
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf]() {
                    buf->append(sock->readAll());
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray line = buf->left(buf->indexOf("\r\n"));
                    buf->clear();
                    answer(sock, QUrl(QString::fromLatin1(line.split(' ').value(1))));
                });
            }
        });
    }

    void reply(QTcpSocket* sock, int code, const QByteArray& body, qint64 contentLength = -1)
    {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " X\r\nConnection: close\r\nContent-Length: " +
                          QByteArray::number(contentLength >= 0 ? contentLength : body.size()) + "\r\n\r\n";
        sock->write(head);
        sock->write(body);
        sock->disconnectFromHost();
    }

    void answer(QTcpSocket* sock, const QUrl& url)
    {
        const QUrlQuery query(url);
        const int index = query.queryItemValue("index").toInt();
        const qint64 offset = query.queryItemValue("offset").toLongLong();
        bool firstForIndex;
        {
            std::lock_guard<std::mutex> lock(mutex);
            firstForIndex = true;
            for (const auto& r : requests) firstForIndex = firstForIndex && r.first != index;
            requests.append({index, offset});
        }
        if (query.queryItemValue("type") != "filedata" || query.queryItemValue("snapshot").toLatin1() != snapshot) {
            reply(sock, 410, "unknown file list");
            return;
        }
        if (!files.contains(index)) {
            reply(sock, 404, "no such file");
            return;
        }
        const QByteArray data = files.value(index).mid(offset);
        if (index == cutIndex && firstForIndex && cutAt >= 0) {
            // Promises the whole file but stops early, like the host's 300 s response limit.
            reply(sock, 200, data.left(cutAt), data.size());
            return;
        }
        reply(sock, 200, data);
    }

    QList<QPair<int, qint64>> requestsFor(int index)
    {
        std::lock_guard<std::mutex> lock(mutex);
        QList<QPair<int, qint64>> out;
        for (const auto& r : requests) if (r.first == index) out.append(r);
        return out;
    }
};

static RequestFactory loopbackRequests(quint16 port)
{
    auto nam = std::make_shared<QNetworkAccessManager*>(nullptr);
    return [port, nam](QObject* parent, const QString& type) {
        if (*nam == nullptr) {
            *nam = new QNetworkAccessManager(parent);
        }
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/actions/clipboard?type=%2").arg(port).arg(type)));
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        return (*nam)->get(request);
    };
}

static HRESULT readAll(IStream* stream, QByteArray& out, ULONG chunk)
{
    out.clear();
    QByteArray buf(chunk, '\0');
    for (;;) {
        ULONG got = 0;
        const HRESULT hr = stream->Read(buf.data(), chunk, &got);
        if (FAILED(hr)) return hr;
        if (got == 0) return S_OK;
        out.append(buf.constData(), got);
    }
}

static IStream* openContents(IDataObject* object, LONG index, HRESULT* result = nullptr)
{
    FORMATETC format = {contentsFormat(), nullptr, DVASPECT_CONTENT, index, TYMED_ISTREAM | TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    const HRESULT hr = object->GetData(&format, &medium);
    if (result) *result = hr;
    if (FAILED(hr)) return nullptr;
    CHECK(medium.tymed == TYMED_ISTREAM && medium.pUnkForRelease == nullptr);
    return medium.pstm;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    FakeHost host;
    host.start();
    const QByteArray big = randomBytes(3 * 1024 * 1024 + 17);
    const QByteArray smallData = randomBytes(1000);
    host.files[1] = big;
    host.files[2] = QByteArray();
    host.files[3] = smallData;
    host.files[4] = big;
    host.cutIndex = 1;
    host.cutAt = 1024 * 1024;

    RemoteFileList list;
    list.snapshot = "abc123";
    list.seq = 7;
    list.entries = {
        {true, "Folder", 0, 1790922480700LL},
        {false, "Folder/big.bin", (quint64)big.size(), 1790922480699LL},
        {false, "empty.txt", 0, 0},
        {false, "small.txt", (quint64)smallData.size(), 1790922480701LL},
        {false, "Folder/slow.bin", (quint64)big.size(), 1790922480701LL},
    };
    for (const RemoteFile& e : list.entries) list.totalBytes += e.size;
    CHECK(fitsFileDescriptors(list));

    // ---- Owner thread commands: a publish superseded while the owner was busy (a long Read) is
    // dropped, so an older file list never covers newer host content
    {
        CommandQueue queue;
        RemoteFileList older = list, newer = list;
        older.seq = 1;
        newer.seq = 2;
        const quint64 first = queue.post(CommandQueue::Publish, older);
        const quint64 second = queue.post(CommandQueue::Publish, newer);
        CHECK(first != 0 && second > first);
        CHECK(!queue.isLatest(first) && queue.isLatest(second));
        auto batch = queue.take();
        CHECK(batch.size() == 1 && batch[0].kind == CommandQueue::Publish && batch[0].list.seq == 2);
        CHECK(queue.take().empty());

        // Publish, then other host content released it: only the release runs
        const quint64 published = queue.post(CommandQueue::Publish, older);
        const quint64 released = queue.post(CommandQueue::Release);
        batch = queue.take();
        CHECK(batch.size() == 1 && batch[0].kind == CommandQueue::Release && batch[0].generation == released);
        // Its RemoteFilesReady (had it been sent) would be stale on the main thread
        CHECK(!queue.isLatest(published) && queue.isLatest(released));

        // Taken in one batch, superseded after: the owner checks again before setting the clipboard
        const quint64 pending = queue.post(CommandQueue::Publish, newer);
        batch = queue.take();
        CHECK(batch.size() == 1 && batch[0].generation == pending && queue.isLatest(pending));
        queue.post(CommandQueue::Release);
        CHECK(!queue.isLatest(pending));
        CHECK(queue.take().size() == 1);

        // The owner asks before every OleSetClipboard attempt whether the list is still wanted:
        // a superseded one is never set (E_ABORT, the clipboard untouched)
        const quint64 stale = queue.post(CommandQueue::Publish, older);
        queue.post(CommandQueue::Release);
        CHECK(setClipboardWithRetry(nullptr, [&]() { return queue.isLatest(stale); }) == E_ABORT);
        batch = queue.take();
        CHECK(batch.size() == 1 && batch[0].kind == CommandQueue::Release);

        // Quit never supersedes a publish, and is always run
        const quint64 last = queue.post(CommandQueue::Publish, newer);
        CHECK(queue.post(CommandQueue::Quit) == 0);
        batch = queue.take();
        CHECK(batch.size() == 2 && batch[0].generation == last && batch[1].kind == CommandQueue::Quit);
        std::printf("command queue: superseded publishes dropped\n");
    }

    // ---- Data objects that are still alive are tracked (the end of the stream checks them all)
    {
        auto published = std::make_shared<PublishedObjects>();
        auto* a = new HostFilesDataObject(std::make_shared<Hub>(), list, published);
        auto* b = new HostFilesDataObject(std::make_shared<Hub>(), list, published);
        CHECK(published->count() == 2);
        a->Release();
        CHECK(published->count() == 1);
        b->Release();
        CHECK(published->count() == 0);
    }

    // Download thread with two fetchers: unlimited, and 8 Mbps for the cancel and stop cases
    auto control = std::make_shared<ClipboardTransferControl>();
    QThread fetchThread;
    auto* fast = new Fetcher(loopbackRequests(host.server.serverPort()), 0, control);
    auto* slow = new Fetcher(loopbackRequests(host.server.serverPort()), 8 * 1000000 / 8, control);
    fast->moveToThread(&fetchThread);
    slow->moveToThread(&fetchThread);
    QObject::connect(&fetchThread, &QThread::finished, fast, &QObject::deleteLater);
    QObject::connect(&fetchThread, &QThread::finished, slow, &QObject::deleteLater);
    fetchThread.start();
    auto hub = std::make_shared<Hub>();
    hub->setFetcher(fast);
    auto slowHub = std::make_shared<Hub>();
    slowHub->setFetcher(slow);

    std::atomic<bool> done {false};
    std::thread sta([&]() {
        CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
        auto* object = new HostFilesDataObject(hub, list);

        // ---- File descriptors
        {
            FORMATETC format = {descriptorFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium = {};
            CHECK(object->QueryGetData(&format) == S_OK);
            CHECK(object->GetData(&format, &medium) == S_OK && medium.tymed == TYMED_HGLOBAL);
            auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
            CHECK(group != nullptr && group->cItems == 5);
            if (group != nullptr && group->cItems == 5) {
                const FILEDESCRIPTORW* fd = group->fgd;
                CHECK(wcscmp(fd[0].cFileName, L"Folder") == 0 && (fd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
                CHECK(wcscmp(fd[1].cFileName, L"Folder\\big.bin") == 0 && !(fd[1].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
                CHECK(fd[1].nFileSizeLow == (DWORD)big.size() && fd[1].nFileSizeHigh == 0);
                CHECK((fd[1].dwFlags & (FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI)) == (FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI));
                CHECK(!(fd[2].dwFlags & FD_WRITESTIME));  // unknown time
                ULARGE_INTEGER t;
                t.LowPart = fd[1].ftLastWriteTime.dwLowDateTime;
                t.HighPart = fd[1].ftLastWriteTime.dwHighDateTime;
                CHECK(t.QuadPart == 1790922480699ULL * 10000ULL + 116444736000000000ULL);
                std::printf("descriptors: %u items, first '%ls', second '%ls' (%lu bytes)\n", group->cItems, fd[0].cFileName, fd[1].cFileName, fd[1].nFileSizeLow);
            }
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
        }
        // ---- Preferred effect, marker, enumeration, async capability, SetData round trip
        {
            FORMATETC format = {preferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium = {};
            CHECK(object->GetData(&format, &medium) == S_OK);
            CHECK(*static_cast<DWORD*>(GlobalLock(medium.hGlobal)) == DROPEFFECT_COPY);
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
            format.cfFormat = markerFormat();
            CHECK(object->QueryGetData(&format) == S_OK);
            format.cfFormat = CF_HDROP;
            CHECK(object->QueryGetData(&format) == DV_E_FORMATETC);

            IEnumFORMATETC* formats = nullptr;
            CHECK(object->EnumFormatEtc(DATADIR_GET, &formats) == S_OK && formats != nullptr);
            FORMATETC got[8];
            ULONG count = 0;
            formats->Next(8, got, &count);
            CHECK(count == 4);
            formats->Release();

            IDataObjectAsyncCapability* async = nullptr;
            CHECK(object->QueryInterface(__uuidof(IDataObjectAsyncCapability), (void**)&async) == S_OK);
            BOOL mode = FALSE;
            CHECK(async->GetAsyncMode(&mode) == S_OK && mode);
            CHECK(async->StartOperation(nullptr) == S_OK);
            BOOL inOp = FALSE;
            CHECK(async->InOperation(&inOp) == S_OK && inOp);
            CHECK(async->EndOperation(S_OK, nullptr, DROPEFFECT_COPY) == S_OK);
            async->Release();

            const CLIPFORMAT performed = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
            DWORD effect = DROPEFFECT_COPY;
            STGMEDIUM set = {};
            set.tymed = TYMED_HGLOBAL;
            set.hGlobal = globalFromBytes(&effect, sizeof(effect));
            FORMATETC setFormat = {performed, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            CHECK(object->SetData(&setFormat, &set, TRUE) == S_OK);
            STGMEDIUM back = {};
            CHECK(object->GetData(&setFormat, &back) == S_OK);
            ReleaseStgMedium(&back);
        }
        // ---- Directory index and bad index
        {
            HRESULT hr = S_OK;
            CHECK(openContents(object, 0, &hr) == nullptr && hr == DV_E_LINDEX);
            CHECK(openContents(object, 9, &hr) == nullptr && hr == DV_E_LINDEX);
        }
        // ---- Big file, cut by the host after 1 MB and continued from there
        {
            IStream* stream = openContents(object, 1);
            CHECK(stream != nullptr);
            STATSTG stat = {};
            CHECK(stream->Stat(&stat, STATFLAG_DEFAULT) == S_OK);
            CHECK(stat.cbSize.QuadPart == (ULONGLONG)big.size() && stat.pwcsName != nullptr && wcscmp(stat.pwcsName, L"big.bin") == 0);
            CoTaskMemFree(stat.pwcsName);
            CHECK(host.requestsFor(1).isEmpty());  // nothing is downloaded before the first Read
            QByteArray got;
            QElapsedTimer t;
            t.start();
            const HRESULT hr = readAll(stream, got, 1024 * 1024);
            const auto requests = host.requestsFor(1);
            std::printf("big file: hr=0x%08lx, %lld/%lld bytes in %lld ms, %d request(s), offsets %lld, %lld\n",
                        (unsigned long)hr, (long long)got.size(), (long long)big.size(), (long long)t.elapsed(), (int)requests.size(),
                        (long long)requests.value(0).second, (long long)requests.value(1).second);
            CHECK(hr == S_OK && got == big);
            CHECK(requests.size() == 2 && requests.value(0).second == 0 && requests.value(1).second == 1024 * 1024);
            ULARGE_INTEGER pos;
            LARGE_INTEGER zero = {};
            CHECK(stream->Seek(zero, STREAM_SEEK_CUR, &pos) == S_OK && pos.QuadPart == (ULONGLONG)big.size());
            stream->Release();
        }
        // ---- Empty file: no request at all
        {
            IStream* stream = openContents(object, 2);
            QByteArray got;
            CHECK(stream != nullptr && readAll(stream, got, 4096) == S_OK && got.isEmpty());
            CHECK(host.requestsFor(2).isEmpty());
            stream->Release();
        }
        // ---- Seek before the first read starts the download there; odd read sizes
        {
            IStream* stream = openContents(object, 3);
            LARGE_INTEGER to;
            to.QuadPart = 500;
            ULARGE_INTEGER pos;
            CHECK(stream->Seek(to, STREAM_SEEK_SET, &pos) == S_OK && pos.QuadPart == 500);
            QByteArray got;
            CHECK(readAll(stream, got, 7) == S_OK && got == smallData.mid(500));
            const auto requests = host.requestsFor(3);
            CHECK(requests.size() == 1 && requests.value(0).second == 500);
            // Seek back: a new download from 0
            to.QuadPart = 0;
            CHECK(stream->Seek(to, STREAM_SEEK_SET, &pos) == S_OK && pos.QuadPart == 0);
            CHECK(readAll(stream, got, 333) == S_OK && got == smallData);
            stream->Release();
        }
        // ---- A list the host no longer knows (410): the read fails instead of waiting
        {
            RemoteFileList stale = list;
            stale.snapshot = "gone";
            auto* staleObject = new HostFilesDataObject(hub, stale);
            IStream* stream = openContents(staleObject, 3);
            char buf[100];
            ULONG got = 1;
            const HRESULT hr = stream->Read(buf, sizeof(buf), &got);
            std::printf("stale list: hr=0x%08lx, %lu bytes\n", (unsigned long)hr, got);
            CHECK(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && got == 0);
            stream->Release();
            staleObject->Release();
        }
        // ---- Cancel shortcut during a throttled download
        {
            auto* slowObject = new HostFilesDataObject(slowHub, list);
            IStream* stream = openContents(slowObject, 4);
            QByteArray buf(256 * 1024, '\0');
            ULONG got = 0;
            CHECK(stream->Read(buf.data(), (ULONG)buf.size(), &got) == S_OK && got == (ULONG)buf.size());
            CHECK(control->streams.load() >= 1);
            control->streamCancel.fetch_add(1);
            HRESULT hr = S_OK;
            qint64 total = got;
            for (int i = 0; i < 100 && SUCCEEDED(hr) && got > 0; i++) {
                hr = stream->Read(buf.data(), (ULONG)buf.size(), &got);
                total += got;
            }
            std::printf("cancelled: hr=0x%08lx after %lld bytes\n", (unsigned long)hr, (long long)total);
            CHECK(hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && total < big.size());
            stream->Release();

            // ---- End of the stream while a read waits
            IStream* again = openContents(slowObject, 4);
            CHECK(again->Read(buf.data(), 1024, &got) == S_OK && got == 1024);
            std::thread stopper([&]() { Sleep(200); slowHub->stop(); });
            QElapsedTimer t;
            t.start();
            QByteArray all(8 * 1024 * 1024, '\0');
            hr = again->Read(all.data(), (ULONG)all.size(), &got);
            std::printf("stopped: hr=0x%08lx, %lu bytes, returned after %lld ms\n", (unsigned long)hr, got, (long long)t.elapsed());
            CHECK((hr == HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR) && got == 0) || (hr == S_OK && got < (ULONG)all.size()));
            CHECK(t.elapsed() < 2000);
            stopper.join();
            hr = again->Read(all.data(), (ULONG)all.size(), &got);
            CHECK(hr == HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR) && got == 0);
            again->Release();
            IStream* afterStop = openContents(slowObject, 3);
            CHECK(afterStop->Read(buf.data(), 10, &got) == HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR));
            afterStop->Release();
            slowObject->Release();
        }
        object->Release();
        CoUninitialize();
        done = true;
    });

    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&]() { if (done) loop.quit(); });
    poll.start(20);
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!done) {
        std::printf("FAIL: timed out\n");
        std::fflush(stdout);
        _exit(1);
    }
    sta.join();
    // Every job ends once its stream is released; give the download thread a moment.
    for (int i = 0; i < 100 && control->streams.load() != 0; i++) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    CHECK(control->streams.load() == 0);
    fetchThread.quit();
    fetchThread.wait();

    std::printf(g_Failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_Failures.load());
    return g_Failures ? 1 : 0;
}
