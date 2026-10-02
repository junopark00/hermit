#include "clipboardsync.h"
#include "clipboardarchive.h"
#include "clipboardvirtualfiles.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "session.h"
#include "settings/streamingpreferences.h"
#include "SDL_compat.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QNetworkReply>
#include <QPair>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <cstring>

#ifdef Q_OS_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <SDL_syswm.h>
#endif

// Short notice over the stream for images and files. Text copies happen too often to announce.
static void showClipboardNotice(const QString& text, int durationMs = 3500)
{
    Session* session = Session::get();
    if (session != nullptr) {
        session->getOverlayManager().showToast(text, durationMs);
    }
}

constexpr int k_MaxTextBytes = 1024 * 1024;
constexpr int k_MaxImageBytes = 32 * 1024 * 1024;       // encoded PNG, same as the host
constexpr qint64 k_MaxRawImageBytes = 64LL * 1024 * 1024; // local PNG/DIB before conversion
constexpr int k_MaxImageSide = 8192;                     // the host's limit is this many pixels squared
constexpr quint64 k_MaxImagePixels = (quint64)k_MaxImageSide * k_MaxImageSide;

// Why clipboard content did not move (ClipboardTransferControl::pendingNotice)
enum ClipboardNotice {
    NoNotice = 0,
    // Host files
    DownloadNotAllowed, OverLimit, OverStreamLimit, HostFilesUnsupported, HostFilesFailed,
    // Other host content
    HostImageTooLarge, HostImageUnreadable, HostImageFailed, HostTextTooLarge,
    // Local content
    LocalImageTooLarge, LocalImageUnsupported, LocalImageFailed, LocalTextTooLarge,
    // Permissions for a whole direction
    ReadNotAllowed, WriteNotAllowed, ReadWriteNotAllowed,
};

static void showNotice(int notice)
{
    QString text;
    int durationMs = 6000;
    switch (notice) {
    case DownloadNotAllowed:
        text = QCoreApplication::translate("ClipboardSync", "Files on the host not copied: the host does not allow file download for this device");
        break;
    case OverLimit:
    case OverStreamLimit: {
        // Files pasted as virtual files (host with file streaming) have the higher limit
        const quint64 limit = notice == OverStreamLimit ? ClipboardArchive::k_MaxStreamFilesBytes : ClipboardArchive::k_MaxFilesBytes;
        text = QCoreApplication::translate("ClipboardSync", "Files on the host not copied: over %1 MB or %2 items")
                   .arg(limit / (1024 * 1024)).arg(ClipboardArchive::k_MaxFileEntries);
        break;
    }
    case HostFilesUnsupported:
        text = QCoreApplication::translate("ClipboardSync", "Host files can't be copied: unsupported or duplicate names");
        break;
    case HostFilesFailed:
        text = QCoreApplication::translate("ClipboardSync", "Host files could not be copied");
        break;
    case HostImageTooLarge:
        text = QCoreApplication::translate("ClipboardSync", "Image on the host not copied: over %1 MB or %2x%2 pixels")
                   .arg(k_MaxImageBytes / (1024 * 1024)).arg(k_MaxImageSide);
        break;
    case HostImageUnreadable:
        text = QCoreApplication::translate("ClipboardSync", "Image on the host not copied: it could not be read");
        break;
    case HostImageFailed:
        text = QCoreApplication::translate("ClipboardSync", "Image on the host could not be copied");
        break;
    case HostTextTooLarge:
        text = QCoreApplication::translate("ClipboardSync", "Text on the host not copied: over %1 MB").arg(k_MaxTextBytes / (1024 * 1024));
        break;
    case LocalImageTooLarge:
        text = QCoreApplication::translate("ClipboardSync", "Image not sent to the host: over %1 MB or %2x%2 pixels")
                   .arg(k_MaxImageBytes / (1024 * 1024)).arg(k_MaxImageSide);
        break;
    case LocalImageUnsupported:
        text = QCoreApplication::translate("ClipboardSync", "Image not sent to the host: too large or in a format that can't be sent");
        break;
    case LocalImageFailed:
        text = QCoreApplication::translate("ClipboardSync", "Image could not be sent to the host");
        break;
    case LocalTextTooLarge:
        text = QCoreApplication::translate("ClipboardSync", "Text not sent to the host: over %1 MB").arg(k_MaxTextBytes / (1024 * 1024));
        break;
    case ReadNotAllowed:
        text = QCoreApplication::translate("ClipboardSync", "Clipboard: the host does not allow reading its clipboard for this device. Allow Clipboard Read in the host's device permissions.");
        durationMs = 8000;
        break;
    case WriteNotAllowed:
        text = QCoreApplication::translate("ClipboardSync", "Clipboard: the host does not allow sending to its clipboard for this device. Allow Clipboard Set in the host's device permissions.");
        durationMs = 8000;
        break;
    case ReadWriteNotAllowed:
        text = QCoreApplication::translate("ClipboardSync", "Clipboard: the host does not allow clipboard sync for this device. Allow Clipboard Read and Clipboard Set in the host's device permissions.");
        durationMs = 8000;
        break;
    default:
        return;
    }
    showClipboardNotice(text, durationMs);
}

static QString formatTransferSize(qint64 bytes)
{
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 KB").arg(qMax<qint64>(1, bytes / 1024));
}

// "12.0 / 26.0 MB": both numbers in the unit of the total.
static QString formatTransferProgress(qint64 done, qint64 total)
{
    if (total >= 1024 * 1024) {
        return QStringLiteral("%1 / %2 MB").arg(done / (1024.0 * 1024.0), 0, 'f', 1).arg(total / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 / %2 KB").arg(done / 1024).arg(qMax<qint64>(1, total / 1024));
}

using ClipboardArchive::k_MaxArchiveBytes;
using ClipboardArchive::k_MaxFileEntries;
using ClipboardArchive::k_MaxFilesBytes;

namespace {

constexpr int k_InfoTimeoutMs = 5000;
constexpr int k_TextTimeoutMs = 5000;
// The host walks the copied folders for a file list (at most 1,000 items, no file data).
constexpr int k_FileListTimeoutMs = 30000;
// Images and files have no fixed deadline (a 256 MB transfer at 10 Mbps takes minutes); they
// fail when no data moves for this long, which also covers the host packing or unpacking.
constexpr qint64 k_TransferInactivityTimeoutMs = 60000;
constexpr int k_TransferTickMs = 10;
constexpr qint64 k_ProgressDelayMs = 1000;
constexpr qint64 k_ProgressIntervalMs = 500;
constexpr int k_ProgressToastMs = 1500;

std::atomic<int> s_NextGeneration {1};

// Main thread only: the sync of the running stream, for the cancel shortcut.
ClipboardSync* s_ActiveSync = nullptr;

// Marks a file or image transfer as cancellable for its lifetime.
class ActiveTransfer
{
public:
    explicit ActiveTransfer(ClipboardTransferControl& control)
        : m_Control(control)
    {
        m_Control.cancel.store(false);
        m_Control.active.store(true);
    }

    ~ActiveTransfer()
    {
        m_Control.active.store(false);
        m_Control.cancel.store(false);
    }

private:
    ClipboardTransferControl& m_Control;
};

QString clipboardStagingRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/HermitClipboard");
}

QByteArray hashOf(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

// Host replies are "key=value" lines.
bool parseField(const QByteArray& body, const char* key, QByteArray& value)
{
    const QByteArray prefix = QByteArray(key) + '=';
    const QList<QByteArray> lines = body.split('\n');
    for (const QByteArray& line : lines) {
        QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith(prefix)) {
            value = trimmed.mid(prefix.size());
            return true;
        }
    }
    return false;
}

bool parseSeq(const QByteArray& body, quint32& seq)
{
    QByteArray value;
    bool ok = false;
    if (!parseField(body, "seq", value)) {
        return false;
    }
    seq = value.toUInt(&ok);
    return ok;
}

// Pixels in an image (PNG, or a packed DIB on Windows) from its header, without decoding it; 0 if
// the header cannot be read.
quint64 imagePixels(const QByteArray& data, bool isDib)
{
#ifdef Q_OS_WIN32
    if (isDib) {
        BITMAPINFOHEADER header;
        if (data.size() < (qsizetype)sizeof(header)) {
            return 0;
        }
        memcpy(&header, data.constData(), sizeof(header));
        if (header.biSize < sizeof(BITMAPINFOHEADER)) {
            return 0;
        }
        return (quint64)qAbs((qint64)header.biWidth) * (quint64)qAbs((qint64)header.biHeight);
    }
#else
    Q_UNUSED(isDib);
#endif
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    return size.isValid() ? (quint64)size.width() * (quint64)size.height() : 0;
}

#ifdef Q_OS_WIN32

// ---- Windows clipboard helpers (SDL only handles text) --------------------------------------

UINT pngFormat()
{
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

UINT dropEffectFormat()
{
    static const UINT format = RegisterClipboardFormatW(L"Preferred DropEffect");
    return format;
}

bool openClipboardWithRetry(HWND owner)
{
    // Another application may hold the clipboard for a moment.
    for (int attempt = 0; attempt < 20; attempt++) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(15);
    }
    return false;
}

bool localHasImage()
{
    return IsClipboardFormatAvailable(pngFormat()) || IsClipboardFormatAvailable(CF_DIB) ||
           IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_BITMAP);
}

QByteArray readGlobal(HANDLE handle)
{
    if (handle == nullptr) {
        return QByteArray();
    }
    const SIZE_T size = GlobalSize(handle);
    const char* data = static_cast<const char*>(GlobalLock(handle));
    if (data == nullptr) {
        return QByteArray();
    }
    QByteArray out(data, (qsizetype)size);
    GlobalUnlock(handle);
    return out;
}

// Reads the clipboard image as PNG if an app provided one, otherwise as a packed DIB.
// Returns false only if the clipboard could not be opened.
bool readLocalImage(HWND owner, QByteArray& data, bool& isDib)
{
    data.clear();
    isDib = false;
    if (!openClipboardWithRetry(owner)) {
        return false;
    }
    if (IsClipboardFormatAvailable(pngFormat())) {
        HANDLE handle = GetClipboardData(pngFormat());
        if (handle != nullptr && GlobalSize(handle) <= (SIZE_T)k_MaxRawImageBytes) {
            data = readGlobal(handle);
        }
    }
    if (data.isEmpty()) {
        // CF_DIB rather than CF_DIBV5: many apps write an unreliable alpha channel in V5 data.
        HANDLE handle = GetClipboardData(CF_DIB);
        if (handle != nullptr && GlobalSize(handle) <= (SIZE_T)k_MaxRawImageBytes) {
            data = readGlobal(handle);
            isDib = !data.isEmpty();
        }
    }
    CloseClipboard();
    return true;
}

QStringList readLocalFiles(HWND owner)
{
    QStringList paths;
    if (!openClipboardWithRetry(owner)) {
        return paths;
    }
    HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; i++) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, &path[0], length + 1);
            path.resize(length);
            if (!path.empty()) {
                paths.append(QString::fromStdWString(path));
            }
        }
    }
    CloseClipboard();
    return paths;
}

HGLOBAL makeGlobal(const QByteArray& bytes)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)bytes.size());
    if (global == nullptr) {
        return nullptr;
    }
    void* data = GlobalLock(global);
    if (data == nullptr) {
        GlobalFree(global);
        return nullptr;
    }
    memcpy(data, bytes.constData(), (size_t)bytes.size());
    GlobalUnlock(global);
    return global;
}

// Replaces the clipboard with the given formats; all or nothing.
bool writeLocalClipboard(HWND owner, const QVector<QPair<UINT, QByteArray>>& formats)
{
    QVector<HGLOBAL> handles;
    for (const auto& format : formats) {
        HGLOBAL handle = makeGlobal(format.second);
        if (handle == nullptr) {
            for (HGLOBAL h : handles) {
                GlobalFree(h);
            }
            return false;
        }
        handles.append(handle);
    }
    if (!openClipboardWithRetry(owner)) {
        for (HGLOBAL h : handles) {
            GlobalFree(h);
        }
        return false;
    }
    // After a successful SetClipboardData the system owns that handle.
    QVector<bool> owned(handles.size(), false);
    bool ok = EmptyClipboard() != 0;
    for (int i = 0; ok && i < handles.size(); i++) {
        owned[i] = SetClipboardData(formats[i].first, handles[i]) != nullptr;
        ok = owned[i];
    }
    CloseClipboard();
    for (int i = 0; i < handles.size(); i++) {
        if (!owned[i]) {
            GlobalFree(handles[i]);
        }
    }
    return ok;
}

QByteArray makeDropFiles(const QStringList& paths)
{
    QByteArray out((qsizetype)sizeof(DROPFILES), '\0');
    DROPFILES header = {};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    memcpy(out.data(), &header, sizeof(header));
    for (const QString& path : paths) {
        const QString native = QDir::toNativeSeparators(path);
        // utf16() is NUL-terminated, so this includes each path's terminator.
        out.append(reinterpret_cast<const char*>(native.utf16()), (native.size() + 1) * (qsizetype)sizeof(char16_t));
    }
    out.append(2, '\0');  // the list ends with an empty string
    return out;
}

QByteArray dibToPng(const QByteArray& dib)
{
    if (dib.size() < (qsizetype)sizeof(BITMAPINFOHEADER)) {
        return QByteArray();
    }
    BITMAPINFOHEADER header;
    memcpy(&header, dib.constData(), sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > (DWORD)dib.size()) {
        return QByteArray();
    }
    // Pixel offset: header, masks (only after a plain BITMAPINFOHEADER), then the color table.
    qint64 offset = header.biSize;
    if (header.biSize == sizeof(BITMAPINFOHEADER) && (header.biCompression == BI_BITFIELDS || header.biCompression == 6 /* BI_ALPHABITFIELDS */)) {
        offset += header.biCompression == BI_BITFIELDS ? 12 : 16;
    }
    qint64 colors = header.biClrUsed;
    if (colors == 0 && header.biBitCount <= 8) {
        colors = 1LL << header.biBitCount;
    }
    offset += colors * 4;
    if (offset >= dib.size()) {
        return QByteArray();
    }

    BITMAPFILEHEADER file = {};
    file.bfType = 0x4D42;  // "BM"
    file.bfSize = (DWORD)(sizeof(file) + dib.size());
    file.bfOffBits = (DWORD)(sizeof(file) + offset);
    QByteArray bmp(reinterpret_cast<const char*>(&file), (qsizetype)sizeof(file));
    bmp.append(dib);

    QImage image;
    if (!image.loadFromData(bmp, "BMP")) {
        return QByteArray();
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        return QByteArray();
    }
    return png;
}

// Bottom-up 32-bit BI_BITFIELDS CF_DIBV5, the layout most applications accept.
QByteArray pngToDibV5(const QByteArray& png)
{
    QImage image;
    if (!image.loadFromData(png, "PNG") || image.isNull() ||
        (quint64)image.width() * (quint64)image.height() > k_MaxImagePixels) {
        return QByteArray();
    }
    image = image.convertToFormat(QImage::Format_ARGB32);  // B, G, R, A bytes on little-endian

    const qsizetype rowBytes = (qsizetype)image.width() * 4;
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = image.width();
    header.bV5Height = image.height();  // positive height: bottom-up
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = (DWORD)(rowBytes * image.height());
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = 0x73524742;  // LCS_sRGB
    header.bV5Intent = LCS_GM_IMAGES;

    QByteArray dib(reinterpret_cast<const char*>(&header), (qsizetype)sizeof(header));
    dib.reserve(dib.size() + rowBytes * image.height());
    for (int y = image.height() - 1; y >= 0; y--) {
        dib.append(reinterpret_cast<const char*>(image.constScanLine(y)), rowBytes);
    }
    return dib;
}

#endif // Q_OS_WIN32

}  // namespace

// ---- Worker (runs on the clipboard sync thread) ---------------------------------------------

ClipboardSyncWorker::ClipboardSyncWorker(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid,
                                         int sdlEventCode, int generation, bool localChangeTracking, int rateMbps,
                                         std::shared_ptr<std::atomic<bool>> stopped,
                                         std::shared_ptr<ClipboardTransferControl> control)
    : m_Address(address),
      m_HttpsPort(httpsPort),
      m_ServerCert(serverCert),
      m_UseTrueUid(useTrueUid),
      m_SdlEventCode(sdlEventCode),
      m_Generation(generation),
      m_LocalChangeTracking(localChangeTracking),
      m_RateBytesPerSecond(rateMbps > 0 ? (qint64)rateMbps * 1000000 / 8 : 0),
      m_Stopped(std::move(stopped)),
      m_Control(std::move(control)),
      m_Http(nullptr),
      m_Busy(false),
      m_Mode(Mode::Unknown),
      m_HostSeqValid(false),
      m_HostStreamsFiles(false),
      m_HostSeq(0),
      m_HostTextHashValid(false),
      m_WarnedTextOnly(false),
      m_PullDenied(false),
      m_PushDenied(false),
      m_PushAccepted(false),
      m_LastHttpStatus(0)
{
}

void ClipboardSyncWorker::run(const std::function<void()>& job)
{
    if (m_Busy) {
        m_Pending.append(job);
        return;
    }
    m_Busy = true;
    job();
    while (!m_Pending.isEmpty() && !stopped()) {
        const std::function<void()> next = m_Pending.takeFirst();
        next();
    }
    m_Pending.clear();
    m_Busy = false;
}

NvHTTP* ClipboardSyncWorker::http()
{
    // Created lazily so the NvHTTP and its QNetworkAccessManager live on this thread.
    if (m_Http == nullptr) {
        m_Http = new NvHTTP(m_Address, m_HttpsPort, m_ServerCert, m_UseTrueUid);
        m_Http->setParent(this);
    }
    return m_Http;
}

bool ClipboardSyncWorker::request(const QString& type, const QByteArray* postBody, int timeoutMs, QByteArray& body, int& qtError)
{
    m_LastHttpStatus = 0;
    try {
        body = http()->clipboardRequest(type, postBody, timeoutMs);
        qtError = QNetworkReply::NoError;
        return true;
    }
    catch (const QtNetworkReplyException& e) {
        qtError = e.getError();
        m_LastHttpStatus = e.getHttpStatus();
        m_LastError = e.toQString();
    }
    catch (const HostHttpResponseException& e) {
        qtError = -1;
        m_LastError = e.toQString();
    }
    body.clear();
    return false;
}

ClipboardSyncWorker::TransferResult ClipboardSyncWorker::transfer(const QString& type, ClipboardArchive::ThrottledUploadDevice* upload,
                                                                  QIODevice* sink, qint64 sinkLimit, QByteArray* response, int& qtError)
{
    enum class State { Running, Cancelled, TimedOut, TooLarge, WriteFailed };

    const bool isUpload = upload != nullptr;
    qtError = QNetworkReply::NoError;
    m_LastError.clear();
    m_LastHttpStatus = 0;

    // Uploads are throttled by the body device itself.
    ClipboardArchive::RateLimiter limiter(isUpload ? 0 : m_RateBytesPerSecond);
    std::unique_ptr<QNetworkReply> reply(http()->startClipboardRequest(type, upload, isUpload ? upload->totalSize() : 0));
    if (!limiter.unlimited()) {
        // Qt stops reading the socket while this buffer is full, so TCP flow control slows the
        // host down to the rate at which we take the data out.
        reply->setReadBufferSize(qMax<qint64>(64 * 1024, limiter.burst() * 2));
    }
    if (m_ReadBuffer.isEmpty()) {
        m_ReadBuffer.resize(64 * 1024);
    }

    State state = State::Running;
    qint64 done = 0;  // bytes sent, or bytes written to the sink
    qint64 total = isUpload ? upload->totalSize() : -1;
    QElapsedTimer clock;
    clock.start();
    qint64 lastActivityMs = 0;
    qint64 lastProgressMs = -1;
    QEventLoop loop;

    auto finish = [&](State newState) {
        if (state == State::Running) {
            state = newState;
        }
        loop.quit();
    };
    auto drain = [&](bool throttled) {
        while (state == State::Running && reply->bytesAvailable() > 0) {
            qint64 want = qMin<qint64>(m_ReadBuffer.size(), reply->bytesAvailable());
            if (throttled) {
                want = qMin(want, limiter.available());
                if (want <= 0) {
                    return;
                }
            }
            const qint64 n = reply->read(m_ReadBuffer.data(), want);
            if (n <= 0) {
                return;
            }
            limiter.consume(n);
            if (done + n > sinkLimit) {
                finish(State::TooLarge);
                return;
            }
            if (sink->write(m_ReadBuffer.constData(), n) != n) {
                finish(State::WriteFailed);
                return;
            }
            done += n;
        }
    };

    connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (isUpload) {
        connect(reply.get(), &QNetworkReply::uploadProgress, &loop, [&](qint64 sent, qint64) {
            if (sent > done) {
                done = sent;
                lastActivityMs = clock.elapsed();
            }
        });
    }
    else {
        qint64 received = 0;
        connect(reply.get(), &QNetworkReply::downloadProgress, &loop, [&, received](qint64 bytes, qint64 bytesTotal) mutable {
            if (bytes > received) {
                received = bytes;
                lastActivityMs = clock.elapsed();
            }
            if (bytesTotal > 0) {
                total = bytesTotal;
                if (bytesTotal > sinkLimit) {
                    finish(State::TooLarge);
                }
            }
        });
        if (limiter.unlimited()) {
            connect(reply.get(), &QNetworkReply::readyRead, &loop, [&]() { drain(false); });
        }
    }

    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(k_TransferTickMs);
    connect(&tick, &QTimer::timeout, &loop, [&]() {
        if (stopped()) {
            loop.quit();
            return;
        }
        if (m_Control->cancel.load()) {
            finish(State::Cancelled);
            return;
        }
        if (isUpload && upload->failed()) {
            loop.quit();
            return;
        }
        if (!isUpload) {
            drain(!limiter.unlimited());
        }
        const qint64 now = clock.elapsed();
        if (now - lastActivityMs > k_TransferInactivityTimeoutMs) {
            finish(State::TimedOut);
            return;
        }
        if (now >= k_ProgressDelayMs && (lastProgressMs < 0 || now - lastProgressMs >= k_ProgressIntervalMs)) {
            lastProgressMs = now;
            showTransferProgress(isUpload, done, total);
        }
    });
    tick.start();
    if (state == State::Running && !reply->isFinished()) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    tick.stop();

    const bool finished = reply->isFinished();
    // No more callbacks into this frame's locals.
    QObject::disconnect(reply.get(), nullptr, &loop, nullptr);
    if (!finished) {
        reply->abort();
    }

    if (stopped()) {
        return TransferResult::Stopped;
    }
    switch (state) {
    case State::Cancelled:
        return TransferResult::Cancelled;
    case State::TimedOut:
        qtError = QNetworkReply::TimeoutError;
        m_LastError = QStringLiteral("no data moved for %1 s").arg(k_TransferInactivityTimeoutMs / 1000);
        return TransferResult::Failed;
    case State::TooLarge:
        qtError = QNetworkReply::UnknownContentError;  // what the host answers with 413
        m_LastHttpStatus = 413;
        m_LastError = QStringLiteral("over the size limit");
        return TransferResult::Failed;
    case State::WriteFailed:
        qtError = -1;
        m_LastError = QStringLiteral("cannot write the temporary file");
        return TransferResult::Failed;
    case State::Running:
        break;
    }
    if (isUpload && upload->failed()) {
        qtError = -1;
        m_LastError = upload->errorString();
        return TransferResult::Failed;
    }
    if (!finished) {
        // The event loop was ended from outside (thread or application shutting down).
        return TransferResult::Stopped;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            qtError = -1;
            m_LastError = QStringLiteral("Server certificate mismatch");
        }
        else {
            qtError = reply->error();
            m_LastHttpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_LastError = reply->errorString();
            // The host says why in the body (a name it cannot copy, for example)
            const QByteArray reason = reply->readAll().left(200).trimmed();
            if (!reason.isEmpty()) {
                m_LastError += QStringLiteral(": ") + QString::fromUtf8(reason);
            }
        }
        return TransferResult::Failed;
    }

    if (isUpload) {
        if (response != nullptr) {
            *response = reply->readAll();
        }
        return TransferResult::Done;
    }
    // At most one read buffer is left; take it without waiting for the rate limit.
    drain(false);
    switch (state) {
    case State::TooLarge:
        qtError = QNetworkReply::UnknownContentError;
        m_LastHttpStatus = 413;
        m_LastError = QStringLiteral("over the size limit");
        return TransferResult::Failed;
    case State::WriteFailed:
        qtError = -1;
        m_LastError = QStringLiteral("cannot write the temporary file");
        return TransferResult::Failed;
    default:
        break;
    }
    if (total >= 0 && done != total) {
        qtError = -1;
        m_LastError = QStringLiteral("incomplete download");
        return TransferResult::Failed;
    }
    return TransferResult::Done;
}

void ClipboardSyncWorker::showTransferProgress(bool upload, qint64 done, qint64 total)
{
    if (stopped()) {
        return;
    }
    QString text;
    if (total > 0) {
        const int percent = (int)qMin<qint64>(100, qMax<qint64>(0, done * 100 / total));
        text = upload
                ? QCoreApplication::translate("ClipboardSync", "Sending to the host: %1% (%2)").arg(percent).arg(formatTransferProgress(done, total))
                : QCoreApplication::translate("ClipboardSync", "Receiving from the host: %1% (%2)").arg(percent).arg(formatTransferProgress(done, total));
    }
    else {
        text = QCoreApplication::translate("ClipboardSync", "Receiving from the host (%1)").arg(formatTransferSize(done));
    }
    text += QStringLiteral(" · ") + QCoreApplication::translate("ClipboardSync", "Ctrl+Alt+Shift+T to cancel");
    // Outlives the update interval, so the notice stays up until the next update or the end.
    showClipboardNotice(text, k_ProgressToastMs);
}

void ClipboardSyncWorker::showTransferEnd(TransferResult result, int qtError)
{
    if (stopped()) {
        return;
    }
    if (result == TransferResult::Cancelled) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "File transfer cancelled"));
    }
    else if (result == TransferResult::Failed && qtError != QNetworkReply::ContentAccessDenied) {
        // 403 is left out: the host no longer sees our stream, which is ending anyway.
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "File transfer failed"));
    }
}

void ClipboardSyncWorker::notify(int notice, bool repeat)
{
    if (stopped()) {
        return;
    }
    if (repeat) {
        // Both directions refused, one after the other: one notice says so.
        auto isPermission = [](int n) { return n == ReadNotAllowed || n == WriteNotAllowed || n == ReadWriteNotAllowed; };
        int pending = m_Control->pendingNotice.load();
        int merged;
        do {
            merged = notice;
            if (isPermission(notice) && isPermission(pending) && pending != notice) {
                merged = ReadWriteNotAllowed;
            }
        } while (!m_Control->pendingNotice.compare_exchange_weak(pending, merged));
    }
    showNotice(notice);
}

void ClipboardSyncWorker::denyDirection(Direction direction)
{
    if (direction == Direction::Pull) {
        if (m_PullDenied) {
            return;
        }
        m_PullDenied = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard from the host turned off: this device lacks the clipboard read permission on the host");
    }
    else {
        if (m_PushDenied) {
            return;
        }
        m_PushDenied = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard to the host turned off: this device lacks the clipboard set permission on the host");
    }
    if (m_PullDenied && m_PushDenied) {
        notify(ReadWriteNotAllowed, true);
    }
    else {
        notify(direction == Direction::Pull ? ReadNotAllowed : WriteNotAllowed, true);
    }
}

void ClipboardSyncWorker::handleFailure(const char* operation, int qtError, Direction direction)
{
    switch (qtError) {
    case QNetworkReply::ContentNotFoundError:
        // 404: the host has no clipboard endpoint (no clipboard extension).
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync disabled: host does not support it");
        m_Mode = Mode::Disabled;
        break;
    case QNetworkReply::AuthenticationRequiredError:
        // 401: this device lacks the permission for this direction (read or set) on the host;
        // the other direction keeps working.
        denyDirection(direction);
        break;
    case QNetworkReply::ContentAccessDenied:
        // 403: the host does not see an active stream for us yet (or any more). Transient.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard %s skipped: host reports no active stream", operation);
        break;
    default:
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard %s failed: %s", operation, qPrintable(m_LastError));
        break;
    }
}

void ClipboardSyncWorker::recordHostSequence(const QByteArray& responseBody)
{
    quint32 seq = 0;
    if (parseSeq(responseBody, seq)) {
        m_HostSeq = seq;
        m_HostSeqValid = true;
    }
}

void ClipboardSyncWorker::deliver(ClipboardHostContent* content)
{
    content->generation = m_Generation;
    if (stopped()) {
        delete content;
        return;
    }
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = m_SdlEventCode;
    event.user.data1 = content;
    if (SDL_PushEvent(&event) != 1) {
        delete content;
    }
}

void ClipboardSyncWorker::init()
{
    if (stopped() || m_Mode != Mode::Unknown) {
        return;
    }

    QByteArray body;
    int error = QNetworkReply::NoError;
    quint32 seq = 0;
    if (request(QStringLiteral("info"), nullptr, k_InfoTimeoutMs, body, error) && parseSeq(body, seq)) {
        // Shell host: remember its current state, but do not copy it yet.
        m_Mode = Mode::Extended;
        m_HostSeq = seq;
        m_HostSeqValid = true;
        QByteArray files;
        m_HostStreamsFiles = parseField(body, "files", files) && files == "stream";
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: host supports text, images and files%s",
                    m_HostStreamsFiles ? " (files pasted as they download)" : "");
        return;
    }
    if (error == QNetworkReply::AuthenticationRequiredError) {
        // This device may not read the host clipboard, so its reply cannot tell which protocol
        // the host speaks; sending to it may still be allowed.
        denyDirection(Direction::Pull);
        m_Mode = hostIsShell() ? Mode::Extended : Mode::Legacy;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: sending only, %s",
                    m_Mode == Mode::Extended ? "text, images and files" : "text");
        return;
    }
    if (error == QNetworkReply::NoError || error == QNetworkReply::ProtocolInvalidOperationError) {
        // 400 (or a reply without a sequence number): a host that only understands type=text.
        m_Mode = Mode::Legacy;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: host supports text only");
        if (request(QStringLiteral("text"), nullptr, k_TextTimeoutMs, body, error)) {
            m_HostTextHash = hashOf(body);
            m_HostTextHashValid = true;
        }
        else if (error == QNetworkReply::AuthenticationRequiredError) {
            denyDirection(Direction::Pull);
        }
        return;
    }
    handleFailure("setup", error, Direction::Pull);
}

bool ClipboardSyncWorker::hostIsShell()
{
    // The power query answers every paired device (with whether it may use it); other hosts have
    // no such endpoint.
    try {
        const QByteArray body = http()->powerRequest(QStringLiteral("query"), false, k_InfoTimeoutMs);
        QByteArray value;
        return parseField(body, "supported", value);
    }
    catch (const std::exception&) {
        return false;
    }
}

bool ClipboardSyncWorker::ensureReady()
{
    if (stopped()) {
        return false;
    }
    if (m_Mode == Mode::Unknown) {
        init();
    }
    return m_Mode == Mode::Extended || m_Mode == Mode::Legacy;
}

void ClipboardSyncWorker::pushText(const QByteArray& utf8)
{
    if (utf8.isEmpty() || !ensureReady() || m_PushDenied) {
        return;
    }
    if (utf8.size() > k_MaxTextBytes) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Local clipboard text not sent: %d bytes exceeds the %d byte limit",
                    (int)utf8.size(), k_MaxTextBytes);
        notify(LocalTextTooLarge, false);
        return;
    }

    // Without per-change tracking on this side (or a host that reports changes), identical text
    // is our own echo and must not be sent back.
    const QByteArray hash = hashOf(utf8);
    const bool trustLocalTracking = m_Mode == Mode::Extended && m_LocalChangeTracking;
    if (!trustLocalTracking && m_HostTextHashValid && hash == m_HostTextHash) {
        return;
    }

    QByteArray body;
    int error = QNetworkReply::NoError;
    if (!request(QStringLiteral("text"), &utf8, k_TextTimeoutMs, body, error)) {
        handleFailure("send", error, Direction::Push);
        return;
    }
    m_PushAccepted = true;
    m_HostTextHash = hash;
    m_HostTextHashValid = true;
    recordHostSequence(body);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard text sent to host (%d bytes)", (int)utf8.size());
}

void ClipboardSyncWorker::pushImage(const QByteArray& data, bool isDib)
{
    if (!ensureReady() || m_PushDenied) {
        return;
    }
    if (m_Mode != Mode::Extended) {
        if (!m_WarnedTextOnly) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard images and files need a Shell host; only text is synced");
            m_WarnedTextOnly = true;
        }
        return;
    }

    // The host takes at most 8192x8192 pixels; checked from the header before converting.
    const quint64 pixels = imagePixels(data, isDib);
    if (pixels > k_MaxImagePixels) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Local clipboard image not sent: %llu pixels exceeds the %dx%d limit",
                    (unsigned long long)pixels, k_MaxImageSide, k_MaxImageSide);
        notify(LocalImageTooLarge, false);
        return;
    }
#ifdef Q_OS_WIN32
    const QByteArray png = isDib ? dibToPng(data) : data;
#else
    const QByteArray png = data;
#endif
    if (png.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Local clipboard image could not be converted");
        notify(LocalImageUnsupported, false);
        return;
    }
    if (png.size() > k_MaxImageBytes) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Local clipboard image not sent: %d KB exceeds the %d MB limit",
                    (int)(png.size() / 1024), k_MaxImageBytes / (1024 * 1024));
        notify(LocalImageTooLarge, false);
        return;
    }

    QByteArray body;
    int error = QNetworkReply::NoError;
    ActiveTransfer active(*m_Control);
    ClipboardArchive::BufferUploadDevice device(png, m_RateBytesPerSecond);
    device.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    const TransferResult result = transfer(QStringLiteral("image"), &device, nullptr, 0, &body, error);
    if (result == TransferResult::Cancelled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Sending the clipboard image to the host was cancelled");
        showTransferEnd(result, error);
        return;
    }
    if (result == TransferResult::Failed) {
        if (m_LastHttpStatus == 413) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard image not sent: the host refused it as too large");
            notify(LocalImageTooLarge, false);
        }
        else {
            handleFailure("image send", error, Direction::Push);
            if (error != QNetworkReply::AuthenticationRequiredError && error != QNetworkReply::ContentAccessDenied &&
                error != QNetworkReply::ContentNotFoundError) {
                notify(LocalImageFailed, false);
            }
        }
        return;
    }
    if (result != TransferResult::Done) {
        return;
    }
    m_PushAccepted = true;
    recordHostSequence(body);
    m_HostTextHashValid = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard image sent to host (%d KB)", (int)(png.size() / 1024));
    if (!stopped()) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "Image sent to the host (%1)")
                                .arg(formatTransferSize(png.size())));
    }
}

void ClipboardSyncWorker::pushFiles(const QStringList& paths, bool dropped)
{
    const bool ready = ensureReady();
    if (ready && m_PushDenied) {
        if (dropped) {
            notify(WriteNotAllowed, false);
        }
        return;
    }
    if ((!ready || m_Mode != Mode::Extended) && dropped && !stopped()) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "Sending files needs a Shell host that allows clipboard and file transfer for this device"), 5000);
    }
    if (!ready) {
        return;
    }
    if (m_Mode != Mode::Extended) {
        if (!m_WarnedTextOnly) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard images and files need a Shell host; only text is synced");
            m_WarnedTextOnly = true;
        }
        return;
    }

    QVector<ClipboardArchive::Entry> entries;
    qint64 archiveSize = 0;
    QString packError;
    if (!ClipboardArchive::planUpload(paths, entries, archiveSize, packError)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Local files not sent to host: %s", qPrintable(packError));
        if (!stopped()) {
            showClipboardNotice(QCoreApplication::translate("ClipboardSync", "Files not sent: over %1 MB or %2 items, or a file cannot be read")
                                    .arg(k_MaxFilesBytes / (1024 * 1024)).arg(k_MaxFileEntries), 5000);
        }
        return;
    }

    // The archive is produced from the files while it is sent, never held in memory.
    QByteArray body;
    int error = QNetworkReply::NoError;
    ActiveTransfer active(*m_Control);
    ClipboardArchive::ArchiveUploadDevice device(entries, archiveSize, m_RateBytesPerSecond);
    device.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    const TransferResult result = transfer(QStringLiteral("files"), &device, nullptr, 0, &body, error);
    if (result == TransferResult::Cancelled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Sending files to the host was cancelled");
    }
    if (result == TransferResult::Failed && error == QNetworkReply::AuthenticationRequiredError) {
        // Files need the clipboard set permission and the file upload one. A 401 after the host
        // already took something from us means the file upload permission is missing.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Files not sent: %s is not granted to this device on the host",
                    m_PushAccepted ? "the file upload permission" : "the clipboard set or file upload permission");
        // Copied files are sent when the user returns to the stream window, so the notice is
        // seen there just like for dropped files
        if (!stopped()) {
            showClipboardNotice(m_PushAccepted
                                    ? QCoreApplication::translate("ClipboardSync", "Files not sent: the host does not allow file upload for this device")
                                    : QCoreApplication::translate("ClipboardSync", "Files not sent: the host does not allow sending to its clipboard or file upload for this device. Check Clipboard Set and File Upload in the host's device permissions."),
                                6000);
        }
        return;
    }
    if (result == TransferResult::Failed && m_LastHttpStatus == 422) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Files not sent: the host refused them: %s", qPrintable(m_LastError));
        if (!stopped()) {
            showClipboardNotice(QCoreApplication::translate("ClipboardSync", "Files not sent: the host can't take these names"), 5000);
        }
        return;
    }
    if (result == TransferResult::Failed) {
        if (error == QNetworkReply::ContentAccessDenied) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Files not sent: host reports no active stream");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Sending files to host failed: %s", qPrintable(m_LastError));
        }
    }
    if (result != TransferResult::Done) {
        showTransferEnd(result, error);
        return;
    }
    m_PushAccepted = true;
    recordHostSequence(body);
    m_HostTextHashValid = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard files sent to host (%d items, %lld KB)",
                (int)paths.size(), (long long)(archiveSize / 1024));
    if (!stopped() && dropped) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "%n item(s) copied to the host clipboard (%1). Paste with Ctrl+V on the host.", nullptr, (int)paths.size())
                                .arg(formatTransferSize(archiveSize)), 6000);
    }
    else if (!stopped()) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "%n item(s) sent to the host (%1)", nullptr, (int)paths.size())
                                .arg(formatTransferSize(archiveSize)));
    }
}

void ClipboardSyncWorker::pull()
{
    if (!ensureReady() || m_PullDenied) {
        return;
    }

    QByteArray body;
    int error = QNetworkReply::NoError;

    if (m_Mode == Mode::Legacy) {
        if (!request(QStringLiteral("text"), nullptr, k_TextTimeoutMs, body, error)) {
            handleFailure("fetch", error, Direction::Pull);
            return;
        }
        const QByteArray hash = hashOf(body);
        const bool changed = !m_HostTextHashValid || hash != m_HostTextHash;
        m_HostTextHash = hash;
        m_HostTextHashValid = true;
        // Empty means the host has no text; leave the local clipboard alone.
        if (changed && body.size() > k_MaxTextBytes) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host clipboard text ignored: %d bytes exceeds the %d byte limit",
                        (int)body.size(), k_MaxTextBytes);
            notify(HostTextTooLarge, true);
        }
        else if (changed && !body.isEmpty()) {
            auto* content = new ClipboardHostContent;
            content->kind = ClipboardHostContent::Text;
            content->text = body;
            deliver(content);
        }
        return;
    }

    if (!request(QStringLiteral("info"), nullptr, k_InfoTimeoutMs, body, error)) {
        handleFailure("check", error, Direction::Pull);
        return;
    }
    quint32 seq = 0;
    QByteArray type;
    if (!parseSeq(body, seq) || !parseField(body, "type", type)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unexpected clipboard info from host");
        return;
    }
    QByteArray files;
    m_HostStreamsFiles = parseField(body, "files", files) && files == "stream";
    if (m_HostSeqValid && seq == m_HostSeq) {
        return;  // unchanged since we last pushed or pulled
    }

    if (type == "text") {
        if (!request(QStringLiteral("text"), nullptr, k_TextTimeoutMs, body, error)) {
            handleFailure("fetch", error, Direction::Pull);
            return;
        }
        m_HostSeq = seq;
        m_HostSeqValid = true;
        m_HostTextHash = hashOf(body);
        m_HostTextHashValid = true;
        if (body.size() > k_MaxTextBytes) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host clipboard text ignored: %d bytes exceeds the %d byte limit",
                        (int)body.size(), k_MaxTextBytes);
            notify(HostTextTooLarge, true);
        }
        else if (!body.isEmpty()) {
            auto* content = new ClipboardHostContent;
            content->kind = ClipboardHostContent::Text;
            content->text = body;
            deliver(content);
        }
        return;
    }

    // For images and files, remember the sequence even on failure so the same content is not
    // retried on every focus change.
    m_HostSeq = seq;
    m_HostSeqValid = true;
    m_HostTextHashValid = false;

#ifdef Q_OS_WIN32
    if (type == "image") {
        body.clear();
        QBuffer buffer(&body);
        buffer.open(QIODevice::WriteOnly);
        ActiveTransfer active(*m_Control);
        const TransferResult result = transfer(QStringLiteral("image"), nullptr, &buffer, k_MaxImageBytes, nullptr, error);
        buffer.close();
        if (result == TransferResult::Cancelled) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Fetching the host clipboard image was cancelled");
            showTransferEnd(result, error);
            return;
        }
        if (result == TransferResult::Failed) {
            if (m_LastHttpStatus == 413) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Host clipboard image ignored: over the %d MB limit", k_MaxImageBytes / (1024 * 1024));
                notify(HostImageTooLarge, true);
            }
            else {
                handleFailure("image fetch", error, Direction::Pull);
                if (error != QNetworkReply::AuthenticationRequiredError && error != QNetworkReply::ContentAccessDenied &&
                    error != QNetworkReply::ContentNotFoundError) {
                    notify(HostImageFailed, true);
                }
            }
            return;
        }
        if (result != TransferResult::Done || body.isEmpty()) {
            return;  // empty: no image any more, or over the host's own limit
        }
        const quint64 pixels = imagePixels(body, false);
        if (pixels > k_MaxImagePixels) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host clipboard image ignored: %llu pixels exceeds the %dx%d limit",
                        (unsigned long long)pixels, k_MaxImageSide, k_MaxImageSide);
            notify(HostImageTooLarge, true);
            return;
        }
        QByteArray dib = pngToDibV5(body);
        if (dib.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host clipboard image could not be decoded");
            notify(HostImageUnreadable, true);
            return;
        }
        auto* content = new ClipboardHostContent;
        content->kind = ClipboardHostContent::Image;
        content->png = body;
        content->dib = std::move(dib);
        deliver(content);
    }
    else if (type == "files") {
        if (m_HostStreamsFiles) {
            pullFileList();
        }
        else {
            pullArchive();
        }
    }
#endif
}

#ifdef Q_OS_WIN32

void ClipboardSyncWorker::pullFileList()
{
    // Only the list now (names, sizes, times). File Explorer shows its own progress when the user
    // pastes, and each file is downloaded while it is pasted.
    QByteArray body;
    int error = QNetworkReply::NoError;
    if (!request(QStringLiteral("filelist"), nullptr, k_FileListTimeoutMs, body, error)) {
        if (error == QNetworkReply::AuthenticationRequiredError) {
            // The clipboard info just came through, so reading the clipboard is allowed and this
            // is the file download permission.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: file download permission is not granted to this device");
            notify(DownloadNotAllowed, true);
        }
        else if (m_LastHttpStatus == 413) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: over the %d MB / %d item limit",
                        (int)(ClipboardArchive::k_MaxStreamFilesBytes / (1024 * 1024)), k_MaxFileEntries);
            notify(OverStreamLimit, true);
        }
        else if (m_LastHttpStatus == 422) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: the host cannot list them (unsupported or duplicate names)");
            notify(HostFilesUnsupported, true);
        }
        else if (error == QNetworkReply::ContentAccessDenied) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: host reports no active stream");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Fetching the host file list failed: %s", qPrintable(m_LastError));
            notify(HostFilesFailed, true);
        }
        return;
    }
    if (body.isEmpty()) {
        return;  // the host clipboard no longer holds files
    }
    auto* content = new ClipboardHostContent;
    content->kind = ClipboardHostContent::RemoteFiles;
    QString listError;
    if (!ClipboardArchive::parseFileList(body, content->remoteFiles, listError)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host file list rejected: %s", qPrintable(listError));
        delete content;
        notify(listError == QLatin1String("unsafe path") || listError == QLatin1String("duplicate path")
                   ? HostFilesUnsupported : HostFilesFailed, true);
        return;
    }
    if (!ClipboardVirtualFiles::fitsFileDescriptors(content->remoteFiles)) {
        // Explorer's file descriptors hold at most 259 characters per path; such deep folders come
        // over as one archive instead (within its smaller limit).
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host files have paths over 259 characters; copying them as an archive");
        delete content;
        pullArchive();
        return;
    }
    m_HostSeq = content->remoteFiles.seq;
    m_HostSeqValid = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Host file list received (%d items, %llu KB); files download when pasted",
                (int)content->remoteFiles.entries.size(), (unsigned long long)(content->remoteFiles.totalBytes / 1024));
    deliver(content);
}

void ClipboardSyncWorker::pullArchive()
{
    int error = QNetworkReply::NoError;
    const QString stagingRoot = clipboardStagingRoot();
    if (!QDir().mkpath(stagingRoot)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host files not copied: cannot create the staging folder");
        notify(HostFilesFailed, true);
        return;
    }
    // Left over if Hermit was closed during a download (removal fails harmlessly while in use).
    QDir staging(stagingRoot);
    const QStringList stale = staging.entryList({QStringLiteral("download-*.apcf")}, QDir::Files);
    for (const QString& name : stale) {
        QFile::remove(staging.filePath(name));
    }
    // The archive goes to a temporary file (removed when this scope ends), not to memory.
    QTemporaryFile archiveFile(stagingRoot + QStringLiteral("/download-XXXXXX.apcf"));
    if (!archiveFile.open()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host files not copied: cannot create a temporary file");
        notify(HostFilesFailed, true);
        return;
    }
    ActiveTransfer active(*m_Control);
    const TransferResult result = transfer(QStringLiteral("files"), nullptr, &archiveFile, k_MaxArchiveBytes, nullptr, error);
    if (result == TransferResult::Cancelled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Fetching files from the host was cancelled");
        showTransferEnd(result, error);
        return;
    }
    if (result == TransferResult::Failed) {
        if (error == QNetworkReply::AuthenticationRequiredError) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: file download permission is not granted to this device");
            notify(DownloadNotAllowed, true);
        }
        else if (m_LastHttpStatus == 413) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: over the %d MB / %d item limit",
                        (int)(k_MaxFilesBytes / (1024 * 1024)), k_MaxFileEntries);
            notify(OverLimit, true);
        }
        else if (m_LastHttpStatus == 422) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: %s", qPrintable(m_LastError));
            notify(HostFilesUnsupported, true);
        }
        else if (error == QNetworkReply::ContentAccessDenied) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files not copied: host reports no active stream");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Fetching files from host failed: %s", qPrintable(m_LastError));
            notify(HostFilesFailed, true);
        }
        return;
    }
    if (result != TransferResult::Done) {
        return;
    }
    if (!archiveFile.flush() || archiveFile.size() == 0) {
        return;
    }
    // Every path, size and duplicate is checked before the first file is written.
    QVector<ClipboardArchive::Entry> entries;
    QString archiveError;
    if (!ClipboardArchive::validateArchive(archiveFile, entries, archiveError)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host files rejected: %s", qPrintable(archiveError));
        notify(archiveError == QLatin1String("unsafe path") || archiveError == QLatin1String("duplicate path") ||
                       archiveError == QLatin1String("file used as directory")
                   ? HostFilesUnsupported : HostFilesFailed, true);
        return;
    }
    QStringList topLevel;
    auto cancelled = [this]() { return stopped() || m_Control->cancel.load(); };
    if (!ClipboardArchive::extractArchive(archiveFile, entries, stagingRoot, topLevel, archiveError, cancelled)) {
        if (cancelled()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Fetching files from the host was cancelled");
            showTransferEnd(TransferResult::Cancelled, QNetworkReply::NoError);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files could not be saved: %s", qPrintable(archiveError));
            notify(HostFilesFailed, true);
        }
        return;
    }
    auto* content = new ClipboardHostContent;
    content->kind = ClipboardHostContent::Files;
    content->files = topLevel;
    deliver(content);
}

#endif // Q_OS_WIN32

// ---- Main-thread side ---------------------------------------------------------------------

ClipboardSync::ClipboardSync(NvComputer* computer, int sdlEventCode, SDL_Window* window)
    : m_Thread(new QThread()),
      m_Worker(nullptr),
      m_Stopped(std::make_shared<std::atomic<bool>>(false)),
      m_Control(std::make_shared<ClipboardTransferControl>()),
      m_Generation(s_NextGeneration.fetch_add(1)),
      m_WindowHandle(nullptr),
      m_SdlEventCode(sdlEventCode),
      m_HttpsPort(0),
      m_UseTrueUid(true),
      m_RateMbps(StreamingPreferences::get()->clipboardRateMbps),
      m_VirtualFiles(nullptr),
      m_LocalSeqValid(false),
      m_LocalSeq(0)
{
#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window != nullptr && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        m_WindowHandle = info.info.win.window;
    }
    const bool localChangeTracking = true;
#else
    Q_UNUSED(window);
    const bool localChangeTracking = false;
#endif

    NvAddress address;
    uint16_t httpsPort;
    QSslCertificate serverCert;
    bool useTrueUid;
    {
        // Copy what the worker needs so it never touches the shared NvComputer.
        QReadLocker lock(&computer->lock);
        address = computer->activeAddress;
        httpsPort = computer->activeHttpsPort;
        serverCert = computer->serverCert;
        useTrueUid = !computer->isNvidiaServerSoftware;
    }

    m_Address = address;
    m_HttpsPort = httpsPort;
    m_ServerCert = serverCert;
    m_UseTrueUid = useTrueUid;
    m_Worker = new ClipboardSyncWorker(address, httpsPort, serverCert, useTrueUid,
                                       sdlEventCode, m_Generation, localChangeTracking,
                                       m_RateMbps, m_Stopped, m_Control);
    m_Worker->moveToThread(m_Thread);
    // Both delete themselves once the thread stops, so a long transfer never blocks shutdown.
    QObject::connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    QObject::connect(m_Thread, &QThread::finished, m_Thread, &QObject::deleteLater);
    m_Thread->setObjectName("Clipboard sync");
    m_Thread->start();
    s_ActiveSync = this;
}

ClipboardSync::~ClipboardSync()
{
    if (s_ActiveSync == this) {
        s_ActiveSync = nullptr;
    }
#ifdef Q_OS_WIN32
    // Host files still on the clipboard can no longer be downloaded: removes them from the
    // clipboard and fails pastes in progress.
    delete m_VirtualFiles;
    m_VirtualFiles = nullptr;
#endif
    // The worker stops starting new work and aborts an image or file transfer; a text request
    // already in flight finishes or times out on its own, and the thread and worker are then
    // deleted.
    m_Stopped->store(true);
    m_Thread->quit();
    if (!m_Thread->wait(300)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard sync: a transfer is still finishing in the background");
    }
}

void ClipboardSync::start()
{
    ClipboardSyncWorker* worker = m_Worker;
    post([worker]() { worker->init(); });
    pushLocalToHost(Trigger::Start);
}

void ClipboardSync::post(const std::function<void()>& job)
{
    ClipboardSyncWorker* worker = m_Worker;
    QMetaObject::invokeMethod(m_Worker, [worker, job]() { worker->run(job); }, Qt::QueuedConnection);
}

void ClipboardSync::pushDroppedFiles(const QStringList& paths)
{
    if (m_Stopped->load() || paths.isEmpty()) {
        return;
    }
    // Files copied locally while the stream was in the background would otherwise be sent when
    // the user clicks back into the stream, replacing the dropped ones on the host clipboard.
    markLocalHandled();
    ClipboardSyncWorker* worker = m_Worker;
    post([worker, paths]() { worker->pushFiles(paths, true); });
}

bool ClipboardSync::cancelActiveTransfer()
{
    // Called from the keyboard shortcut on the main thread, like every other ClipboardSync call.
    if (s_ActiveSync == nullptr ||
        (!s_ActiveSync->m_Control->active.load() && s_ActiveSync->m_Control->streams.load() == 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "No clipboard file transfer to cancel");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Cancelling the clipboard file transfer");
    s_ActiveSync->m_Control->cancel.store(true);
    // Also stops host files being pasted; File Explorer then reports the copy as cancelled.
    s_ActiveSync->m_Control->streamCancel.fetch_add(1);
    return true;
}

void ClipboardSync::markLocalHandled()
{
#ifdef Q_OS_WIN32
    m_LocalSeq = GetClipboardSequenceNumber();
    m_LocalSeqValid = true;
#endif
}

void ClipboardSync::pushLocalToHost(Trigger trigger)
{
    if (m_Stopped->load()) {
        return;
    }
    ClipboardSyncWorker* worker = m_Worker;

    if (trigger == Trigger::FocusGained) {
        showNotice(m_Control->pendingNotice.exchange(NoNotice));
    }

#ifdef Q_OS_WIN32
    // Each clipboard write bumps the sequence number, so this skips content we already sent
    // and content we just wrote ourselves from the host.
    if (m_LocalSeqValid && GetClipboardSequenceNumber() == m_LocalSeq) {
        return;
    }
    if (IsClipboardFormatAvailable(ClipboardVirtualFiles::markerFormat())) {
        // Our own list of host files (virtual files): never sent back.
        markLocalHandled();
        return;
    }
    HWND owner = static_cast<HWND>(m_WindowHandle);

    // Same priority as the host: text, then image, then files.
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        char* text = SDL_GetClipboardText();
        QByteArray utf8(text != nullptr ? text : "");
        SDL_free(text);
        markLocalHandled();
        if (!utf8.isEmpty()) {
            post([worker, utf8]() { worker->pushText(utf8); });
        }
        return;
    }
    if (localHasImage()) {
        QByteArray data;
        bool isDib = false;
        if (!readLocalImage(owner, data, isDib)) {
            return;  // clipboard busy: try again on the next trigger
        }
        markLocalHandled();
        if (data.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Local clipboard image not sent: unsupported format or over the size limit");
            showNotice(LocalImageUnsupported);
            return;
        }
        post([worker, data, isDib]() { worker->pushImage(data, isDib); });
        return;
    }
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        // Files can be large, so they move when the stream starts or the user returns to the
        // stream window, not on every copy. Files copied before the stream started are sent
        // too: the host otherwise keeps pointing at an older snapshot of the same files.
        if (trigger == Trigger::ClipboardChanged) {
            return;
        }
        const QStringList paths = readLocalFiles(owner);
        if (paths.isEmpty()) {
            return;  // clipboard busy or empty list: leave unhandled so the next trigger retries
        }
        markLocalHandled();
        post([worker, paths]() { worker->pushFiles(paths); });
        return;
    }
    markLocalHandled();
#else
    Q_UNUSED(trigger);
    if (!SDL_HasClipboardText()) {
        return;
    }
    char* text = SDL_GetClipboardText();
    QByteArray utf8(text != nullptr ? text : "");
    SDL_free(text);
    if (!utf8.isEmpty()) {
        post([worker, utf8]() { worker->pushText(utf8); });
    }
#endif
}

void ClipboardSync::pullHostToLocal()
{
    if (m_Stopped->load()) {
        return;
    }
    ClipboardSyncWorker* worker = m_Worker;
    post([worker]() { worker->pull(); });
}

void ClipboardSync::onHostContent(ClipboardHostContent* content)
{
    std::unique_ptr<ClipboardHostContent> owned(content);
    if (!owned || owned->generation != m_Generation || m_Stopped->load()) {
        return;  // left over from an earlier stream
    }

#ifdef Q_OS_WIN32
    if (owned->kind == ClipboardHostContent::RemoteFiles) {
        if (m_VirtualFiles == nullptr) {
            ClipboardVirtualFiles::Connection connection;
            connection.address = m_Address;
            connection.httpsPort = m_HttpsPort;
            connection.serverCert = m_ServerCert;
            connection.useTrueUid = m_UseTrueUid;
            connection.rateBytesPerSecond = m_RateMbps > 0 ? (qint64)m_RateMbps * 1000000 / 8 : 0;
            m_VirtualFiles = new ClipboardVirtualFiles::VirtualFileClipboard(connection, m_Control, m_SdlEventCode, m_Generation);
        }
        // Set on the clipboard by the owner thread, which then sends RemoteFilesReady.
        m_VirtualFiles->publish(owned->remoteFiles);
        return;
    }
    if (owned->kind == ClipboardHostContent::RemoteFilesReady) {
        if (m_VirtualFiles == nullptr || !m_VirtualFiles->isLatest(owned->listGeneration)) {
            // Newer host content (or a newer list) came after this list was offered.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard files from host superseded before they were offered");
            return;
        }
        // The sequence number right after our data object went on the clipboard: not sent back
        m_LocalSeq = owned->localSeq;
        m_LocalSeqValid = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard files from host ready to paste (%d items)", owned->itemCount);
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "%n item(s) from the host are ready to paste", nullptr,
                                                        owned->itemCount));
        return;
    }
#endif

    // Before writing: a host file list still waiting to go on the clipboard must not cover this
    // newer content.
    releaseHostFileList();

    bool ok = false;
    const char* what = "text";
    switch (owned->kind) {
    case ClipboardHostContent::Text:
        ok = SDL_SetClipboardText(owned->text.constData()) == 0;
        break;
#ifdef Q_OS_WIN32
    case ClipboardHostContent::Image:
        what = "image";
        ok = writeLocalClipboard(static_cast<HWND>(m_WindowHandle),
                                 {{pngFormat(), owned->png}, {CF_DIBV5, owned->dib}});
        break;
    case ClipboardHostContent::Files: {
        what = "files";
        DWORD effect = 1;  // DROPEFFECT_COPY: pasting copies instead of moving out of the staging folder
        const QByteArray effectBytes(reinterpret_cast<const char*>(&effect), (qsizetype)sizeof(effect));
        ok = writeLocalClipboard(static_cast<HWND>(m_WindowHandle),
                                 {{CF_HDROP, makeDropFiles(owned->files)}, {dropEffectFormat(), effectBytes}});
        break;
    }
#endif
    default:
        break;
    }

    if (!ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to update the local clipboard with host %s", what);
        return;
    }
    markLocalHandled();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard %s received from host", what);
    if (owned->kind == ClipboardHostContent::Image) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "Image from the host is ready to paste"));
    }
    else if (owned->kind == ClipboardHostContent::Files) {
        showClipboardNotice(QCoreApplication::translate("ClipboardSync", "%n item(s) from the host are ready to paste", nullptr,
                                                        (int)owned->files.size()));
    }
}

void ClipboardSync::releaseHostFileList()
{
#ifdef Q_OS_WIN32
    // Newer host content replaces our host file list on the clipboard. A paste that is already
    // running keeps its downloads.
    if (m_VirtualFiles != nullptr) {
        m_VirtualFiles->release();
    }
#endif
}

void ClipboardSync::discardHostContent(void* content)
{
    delete static_cast<ClipboardHostContent*>(content);
}
