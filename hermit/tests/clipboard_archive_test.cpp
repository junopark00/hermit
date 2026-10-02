// Standalone test for app/streaming/clipboardarchive.{h,cpp}.
#include "streaming/clipboardarchive.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>

#include <cstdio>

using namespace ClipboardArchive;

static int g_Failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_Failures++; } } while (0)

// Reference encoder (the old in-memory encodeArchive).
static void appendU32(QByteArray& out, quint32 v) { char b[4]; qToLittleEndian(v, b); out.append(b, 4); }
static void appendU64(QByteArray& out, quint64 v) { char b[8]; qToLittleEndian(v, b); out.append(b, 8); }
struct RefEntry { bool dir; QByteArray path; QByteArray data; };
static QByteArray encodeRef(const QVector<RefEntry>& entries, quint32 countOverride = 0xFFFFFFFF)
{
    QByteArray out("APCF");
    appendU32(out, 1);
    appendU32(out, countOverride != 0xFFFFFFFF ? countOverride : (quint32)entries.size());
    for (const RefEntry& e : entries) {
        out.append(char(e.dir ? 1 : 0));
        appendU32(out, (quint32)e.path.size());
        out.append(e.path);
        appendU64(out, e.dir ? 0 : (quint64)e.data.size());
        if (!e.dir) out.append(e.data);
    }
    return out;
}

static QByteArray randomBytes(qsizetype n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (qsizetype i = 0; i < n; i++) b[i] = char(QRandomGenerator::global()->bounded(256));
    return b;
}

static void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

// Reads a throttled device to the end like Qt does (0 = wait for readyRead, -1 = end).
static QByteArray drainDevice(QIODevice& dev, qint64 chunk, bool& error, qint64* elapsedMs = nullptr)
{
    QByteArray out;
    QByteArray buf(chunk, '\0');
    QElapsedTimer t; t.start();
    error = false;
    for (;;) {
        qint64 n = dev.read(buf.data(), chunk);
        if (n > 0) { out.append(buf.constData(), n); continue; }
        if (n < 0) { error = !dev.atEnd() || static_cast<ThrottledUploadDevice&>(dev).failed(); break; }
        QEventLoop loop;
        QObject::connect(&dev, &QIODevice::readyRead, &loop, &QEventLoop::quit);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (elapsedMs) *elapsedMs = t.elapsed();
    return out;
}

static bool expectReject(const QByteArray& archive, const char* what)
{
    QBuffer buf;
    buf.setData(archive);
    buf.open(QIODevice::ReadOnly);
    QVector<Entry> entries;
    QString error;
    const bool ok = validateArchive(buf, entries, error);
    std::printf("  reject %-28s -> %s\n", what, ok ? "ACCEPTED (bad)" : qPrintable(error));
    return !ok;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString src = tmp.path() + "/src";

    // ---- 1. Plan + upload device produce exactly the reference archive ----
    const QByteArray big = randomBytes(3 * 1024 * 1024 + 17);
    const QByteArray small = randomBytes(1000);
    writeFile(src + "/a.bin", big);
    writeFile(src + "/Folder/b.txt", small);
    writeFile(src + "/Folder/Sub/empty.dat", QByteArray());
    QDir().mkpath(src + "/Folder/EmptyDir");
    writeFile(src + "/한글 파일.txt", QByteArray("hello"));

    QVector<Entry> plan;
    qint64 size = 0;
    QString error;
    const QStringList roots {src + "/a.bin", src + "/Folder", src + "/한글 파일.txt"};
    CHECK(planUpload(roots, plan, size, error));
    std::printf("plan: %d entries, %lld bytes (%s)\n", (int)plan.size(), (long long)size, qPrintable(error));
    QVector<RefEntry> ref;
    for (const Entry& e : plan) {
        QByteArray data;
        if (!e.directory) { QFile f(e.sourcePath); f.open(QIODevice::ReadOnly); data = f.readAll(); }
        ref.append({e.directory, e.path.toUtf8(), data});
        std::printf("  %s %s %llu\n", e.directory ? "D" : "F", qPrintable(e.path), (unsigned long long)e.size);
    }
    const QByteArray expected = encodeRef(ref);
    CHECK(expected.size() == size);

    {
        ArchiveUploadDevice dev(plan, size, 0);
        dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        bool err = false;
        const QByteArray got = drainDevice(dev, 16 * 1024, err);
        CHECK(!err);
        CHECK(got == expected);
        std::printf("unlimited upload device: %lld bytes, identical=%d\n", (long long)got.size(), got == expected);
    }
    {
        // Odd read sizes
        ArchiveUploadDevice dev(plan, size, 0);
        dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        bool err = false;
        const QByteArray got = drainDevice(dev, 7, err);
        CHECK(!err && got == expected);
    }

    // ---- 2. Throttling: 3 MB at 10 Mbps ~ 2.5 s ----
    {
        ArchiveUploadDevice dev(plan, size, 10 * 1000000 / 8);
        dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        bool err = false;
        qint64 ms = 0;
        const QByteArray got = drainDevice(dev, 16 * 1024, err, &ms);
        const double expectMs = (size - (10 * 1000000 / 8) / 20) * 8.0 / 10e6 * 1000.0;
        std::printf("throttled 10 Mbps: %lld bytes in %lld ms (expected ~%.0f ms), %.2f Mbps\n",
                    (long long)got.size(), (long long)ms, expectMs, got.size() * 8.0 / (ms / 1000.0) / 1e6);
        CHECK(!err && got == expected);
        CHECK(ms > expectMs * 0.9 && ms < expectMs * 1.25);
    }
    {
        BufferUploadDevice dev(big, 30 * 1000000 / 8);
        dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        bool err = false;
        qint64 ms = 0;
        const QByteArray got = drainDevice(dev, 64 * 1024, err, &ms);
        std::printf("buffer device 30 Mbps: %lld bytes in %lld ms, %.2f Mbps\n", (long long)got.size(), (long long)ms,
                    got.size() * 8.0 / (ms / 1000.0) / 1e6);
        CHECK(!err && got == big);
    }

    // ---- 3. File changes size between plan and read ----
    {
        writeFile(src + "/Folder/b.txt", small + "more");
        ArchiveUploadDevice dev(plan, size, 0);
        dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        bool err = false;
        drainDevice(dev, 16 * 1024, err);
        std::printf("grown file: failed=%d (%s)\n", dev.failed(), qPrintable(dev.errorString()));
        CHECK(dev.failed());
        writeFile(src + "/Folder/b.txt", small);
    }
    {
        writeFile(src + "/Folder/b.txt", small.left(10));
        ArchiveUploadDevice dev(plan, size, 0);
        dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        bool err = false;
        drainDevice(dev, 16 * 1024, err);
        std::printf("shrunk file: failed=%d (%s)\n", dev.failed(), qPrintable(dev.errorString()));
        CHECK(dev.failed());
        writeFile(src + "/Folder/b.txt", small);
    }

    // ---- 4. Plan rejects: nothing, unsafe names ----
    {
        QVector<Entry> p; qint64 s; QString e;
        CHECK(!planUpload({src + "/does-not-exist"}, p, s, e));
        std::printf("plan missing -> %s\n", qPrintable(e));
        writeFile(tmp.path() + "/bad/trailing.", QByteArray("x"));
    }

    // ---- 5. Validate + extract round trip ----
    const QString stage = tmp.path() + "/stage";
    {
        QFile archive(tmp.path() + "/dl.apcf");
        archive.open(QIODevice::ReadWrite | QIODevice::Truncate);
        archive.write(expected);
        archive.flush();
        QVector<Entry> entries;
        CHECK(validateArchive(archive, entries, error));
        QStringList top;
        CHECK(extractArchive(archive, entries, stage, top, error));
        std::printf("extracted top level: %s (%s)\n", qPrintable(top.join(" | ")), qPrintable(error));
        CHECK(top.size() == 3);
        const QString folder = QFileInfo(top.value(0)).absolutePath();
        for (const RefEntry& r : ref) {
            const QString path = folder + "/" + QString::fromUtf8(r.path);
            if (r.dir) { CHECK(QFileInfo(path).isDir()); continue; }
            QFile f(path);
            CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == r.data);
        }
        // Cancel during extraction removes the folder
        int calls = 0;
        QStringList top2;
        CHECK(!extractArchive(archive, entries, stage, top2, error, [&]() { return ++calls > 3; }));
        std::printf("cancelled extraction -> %s, folders now %d\n", qPrintable(error),
                    (int)QDir(stage).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
        CHECK(QDir(stage).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1);
    }

    // ---- 6. Validation rejects ----
    {
        QVector<RefEntry> ok {{false, "x.txt", "abc"}};
        CHECK(expectReject(QByteArray("APCX") + encodeRef(ok).mid(4), "bad magic"));
        QByteArray v2 = encodeRef(ok); v2[4] = 2;
        CHECK(expectReject(v2, "version 2"));
        CHECK(expectReject(encodeRef({}, 0), "count 0"));
        CHECK(expectReject(encodeRef(ok, 1001), "count 1001"));
        CHECK(expectReject(encodeRef(ok, 2), "count larger than entries"));
        CHECK(expectReject(encodeRef(ok) + "x", "trailing data"));
        CHECK(expectReject(encodeRef(ok).chopped(1), "truncated data"));
        CHECK(expectReject(encodeRef({{false, "../evil", "a"}}), "dot-dot"));
        CHECK(expectReject(encodeRef({{false, "/abs", "a"}}), "absolute"));
        CHECK(expectReject(encodeRef({{false, "a\\b", "a"}}), "backslash"));
        CHECK(expectReject(encodeRef({{false, "C:x", "a"}}), "drive colon"));
        CHECK(expectReject(encodeRef({{false, "con.txt", "a"}}), "reserved name"));
        CHECK(expectReject(encodeRef({{false, "a.", "a"}}), "trailing dot"));
        CHECK(expectReject(encodeRef({{false, "A.txt", "a"}, {false, "a.TXT", "b"}}), "case duplicate"));
        CHECK(expectReject(encodeRef({{false, "f", "a"}, {false, "f/g", "b"}}), "file used as dir"));
        QByteArray badUtf8 = encodeRef({{false, QByteArray("\xff\xfe", 2), "a"}});
        CHECK(expectReject(badUtf8, "invalid utf-8"));
        QByteArray dirWithSize = encodeRef({{true, "d", QByteArray()}});
        qToLittleEndian<quint64>(5, dirWithSize.data() + 12 + 1 + 4 + 1);
        CHECK(expectReject(dirWithSize + "abcde", "directory with size"));
        QByteArray huge = encodeRef({{false, "h", "a"}});
        qToLittleEndian<quint64>(k_MaxFilesBytes + 1, huge.data() + 12 + 1 + 4 + 1);
        CHECK(expectReject(huge, "size over limit"));
        QByteArray hugePath = encodeRef(ok);
        qToLittleEndian<quint32>(5000, hugePath.data() + 12 + 1);
        CHECK(expectReject(hugePath, "path length 5000"));
        // Accepts a good one
        QBuffer b; b.setData(encodeRef({{true, "d", {}}, {false, "d/f", "abc"}})); b.open(QIODevice::ReadOnly);
        QVector<Entry> e; QString er;
        CHECK(validateArchive(b, e, er) && e.size() == 2 && e[1].dataOffset == 12 + 1 + 4 + 1 + 8 + 1 + 4 + 3 + 8);
    }

    // ---- 7. Qt integration: streamed POST and throttled GET against a local server ----
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost, 0);
        const qint64 downloadSize = 6 * 1024 * 1024;
        QByteArray received;
        qint64 postFirstByteMs = -1, postLastByteMs = -1;
        qint64 expectedPost = -1;
        QElapsedTimer clock;
        QTcpSocket* sock = nullptr;
        QObject::connect(&server, &QTcpServer::newConnection, [&]() {
            sock = server.nextPendingConnection();
            auto* buf = new QByteArray;
            auto* headerDone = new bool(false);
            QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, buf, headerDone]() {
                buf->append(sock->readAll());
                if (!*headerDone) {
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray head = buf->left(end);
                    buf->remove(0, end + 4);
                    *headerDone = true;
                    if (head.startsWith("GET")) {
                        QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(downloadSize) + "\r\nConnection: close\r\n\r\n";
                        sock->write(resp);
                        sock->write(QByteArray(downloadSize, 'z'));
                        return;
                    }
                    const int cl = head.toLower().indexOf("content-length:");
                    expectedPost = head.mid(cl + 15, head.indexOf("\r\n", cl) - cl - 15).trimmed().toLongLong();
                }
                if (!buf->isEmpty() && postFirstByteMs < 0) postFirstByteMs = clock.elapsed();
                received.append(*buf);
                buf->clear();
                if (received.size() >= expectedPost && expectedPost >= 0) {
                    postLastByteMs = clock.elapsed();
                    sock->write("HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nseq=42");
                }
            });
        });

        QNetworkAccessManager nam;
        const QUrl url(QString("http://127.0.0.1:%1/actions/clipboard?type=files").arg(server.serverPort()));
        {
            const qint64 rate = 20 * 1000000 / 8;
            ArchiveUploadDevice dev(plan, size, rate);
            dev.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
            QNetworkRequest req(url);
            req.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain; charset=utf-8");
            req.setHeader(QNetworkRequest::ContentLengthHeader, size);
            req.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
            req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
            clock.start();
            QNetworkReply* reply = nam.post(req, &dev);
            qint64 lastSent = 0, progressEvents = 0;
            QObject::connect(reply, &QNetworkReply::uploadProgress, [&](qint64 s, qint64) { lastSent = s; progressEvents++; });
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(30000, &loop, &QEventLoop::quit);
            loop.exec();
            const qint64 ms = clock.elapsed();
            std::printf("POST streamed: finished=%d error=%d body=%s; server got %lld/%lld bytes, first byte at %lld ms, last at %lld ms (%.2f Mbps), uploadProgress events=%lld\n",
                        reply->isFinished(), reply->error(), reply->readAll().constData(), (long long)received.size(), (long long)size,
                        (long long)postFirstByteMs, (long long)postLastByteMs,
                        received.size() * 8.0 / ((postLastByteMs - postFirstByteMs) / 1000.0) / 1e6, (long long)progressEvents);
            CHECK(reply->error() == QNetworkReply::NoError);
            CHECK(received.size() > 12 && received.mid(0, received.size()) == expected);
            CHECK(postFirstByteMs < 500);  // streamed, not buffered first
            CHECK(ms > 1000);
            delete reply;
        }
        {
            // Throttled download: read buffer limits what Qt holds, read at 20 Mbps.
            const qint64 rate = 20 * 1000000 / 8;
            RateLimiter limiter(rate);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
            clock.restart();
            QNetworkReply* reply = nam.get(req);
            const qint64 bufSize = qMax<qint64>(64 * 1024, limiter.burst() * 2);
            reply->setReadBufferSize(bufSize);
            qint64 got = 0, maxAvail = 0;
            QByteArray chunk(64 * 1024, '\0');
            QEventLoop loop;
            QTimer tick;
            tick.setTimerType(Qt::PreciseTimer);
            tick.setInterval(10);
            QObject::connect(&tick, &QTimer::timeout, [&]() {
                maxAvail = qMax(maxAvail, reply->bytesAvailable());
                while (reply->bytesAvailable() > 0) {
                    qint64 want = qMin<qint64>(chunk.size(), qMin(reply->bytesAvailable(), limiter.available()));
                    if (want <= 0) break;
                    qint64 n = reply->read(chunk.data(), want);
                    if (n <= 0) break;
                    limiter.consume(n);
                    got += n;
                }
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(30000, &loop, &QEventLoop::quit);
            tick.start();
            loop.exec();
            const qint64 finishedMs = clock.elapsed();
            const qint64 left = reply->bytesAvailable();
            got += reply->readAll().size();
            std::printf("GET throttled: finished after %lld ms with %lld bytes left in Qt's buffer (limit %lld), max buffered %lld, total %lld/%lld, %.2f Mbps\n",
                        (long long)finishedMs, (long long)left, (long long)bufSize, (long long)maxAvail, (long long)got, (long long)downloadSize,
                        got * 8.0 / (finishedMs / 1000.0) / 1e6);
            CHECK(got == downloadSize);
            CHECK(maxAvail <= bufSize + 64 * 1024);
            CHECK(finishedMs > 1500);
            delete reply;
        }
    }

    // ---- Host file list (type=filelist) ----
    {
        const QByteArray good =
            "seq=42\nsnapshot=0123456789abcdef\nentries=5\nbytes=5000000006\n"
            "f\t300000\t1790922480699\ta.bin\n"
            "d\t0\t1790922480700\tFolder\n"
            "f\t4999700005\t0\tFolder/b.txt\n"
            "d\t0\t1790922480700\tFolder/Sub\n"
            "f\t1\t1790922480701\t\xED\x95\x9C\xEA\xB8\x80.txt\n";
        RemoteFileList list;
        QString err;
        // 5 GB is over the 4 GB limit
        CHECK(!parseFileList(good, list, err) && err == "files too large");
        QByteArray small = good;
        small.replace("bytes=5000000006", "bytes=300002").replace("4999700005", "1");
        err.clear();
        CHECK(parseFileList(small, list, err));
        std::printf("file list: %s, %d entries, %llu bytes, %d top-level\n", qPrintable(err), (int)list.entries.size(),
                    (unsigned long long)list.totalBytes, list.topLevelCount());
        CHECK(list.seq == 42 && list.snapshot == "0123456789abcdef" && list.entries.size() == 5);
        CHECK(list.totalBytes == 300002 && list.topLevelCount() == 3);
        CHECK(list.entries[1].directory && list.entries[1].path == "Folder" && list.entries[2].path == "Folder/b.txt");
        CHECK(list.entries[0].modifiedMs == 1790922480699LL && list.entries[4].path == QString::fromUtf8("\xED\x95\x9C\xEA\xB8\x80.txt"));
        // CRLF line ends and unknown fields are accepted
        QByteArray crlf = small;
        crlf.replace("\n", "\r\n").prepend("future=1\r\n");
        CHECK(parseFileList(crlf, list, err) && list.entries.size() == 5);

        auto rejects = [&](const QByteArray& from, const QByteArray& to, const char* why) {
            QByteArray bad = small;
            bad.replace(from, to);
            RemoteFileList l;
            QString e;
            const bool ok = parseFileList(bad, l, e);
            if (ok) {
                std::printf("file list accepted although %s\n", why);
            }
            CHECK(!ok);
        };
        rejects("entries=5", "entries=6", "the count is wrong");
        rejects("bytes=300002", "bytes=300003", "the total is wrong");
        rejects("snapshot=0123456789abcdef", "snapshot=01&x=2", "the snapshot id is unsafe");
        rejects("snapshot=0123456789abcdef\n", "", "the snapshot is missing");
        rejects("\tFolder/Sub\n", "\tFolder/../Sub\n", "a path has ..");
        rejects("\tFolder/Sub\n", "\tC:/Sub\n", "a path has a drive");
        rejects("\tFolder/Sub\n", "\tfolder/b.TXT\n", "a path is duplicated");
        rejects("d\t0\t1790922480700\tFolder\n", "f\t0\t1790922480700\tFolder\n", "a file is used as a folder");
        rejects("d\t0\t1790922480700\tFolder/Sub\n", "d\t0\t1790922480700\tOther/Sub\n", "the parent folder is missing");
        rejects("d\t0\t1790922480700\tFolder\n", "d\t7\t1790922480700\tFolder\n", "a folder has a size");
        rejects("f\t1\t1790922480701", "f\tx\t1790922480701", "a size is not a number");
        rejects("\t1790922480701\t", "\t1790922480701\tx\t", "an entry has five fields");
        CHECK(!parseFileList(QByteArray(), list, err));
        QByteArray many = "seq=1\nsnapshot=ab\nentries=1001\nbytes=0\n";
        for (int i = 0; i < 1001; i++) {
            many += "f\t0\t0\tfile" + QByteArray::number(i) + "\n";
        }
        CHECK(!parseFileList(many, list, err) && err == "too many files");
    }

    std::printf(g_Failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_Failures);
    return g_Failures ? 1 : 0;
}
