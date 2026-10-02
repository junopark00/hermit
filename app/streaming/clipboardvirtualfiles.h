#pragma once

// Host files offered on the Windows clipboard as virtual files (CFSTR_FILEDESCRIPTORW and
// CFSTR_FILECONTENTS), like Remote Desktop does. When the stream window loses focus only the list
// of copied files is fetched; File Explorer shows its own copy progress when the user pastes, and
// each file is downloaded from the host (GET type=filedata) while Explorer reads it. Nothing is
// downloaded unless the user pastes.
//
// Threads:
// - the owner thread is a COM single-threaded apartment that owns the data object, calls
//   OleSetClipboard and runs a message loop. Explorer's calls (GetData, IStream::Read) arrive there.
//   A Read that waits for data keeps dispatching COM calls and sent messages, so another program
//   taking over the clipboard meanwhile does not hang;
// - a Qt thread runs the HTTPS requests and fills a bounded buffer per file. When the buffer is
//   full it stops reading, and TCP flow control holds the host back;
// - the SDL main thread only posts commands to both and never waits for them, except for up to a
//   second when the stream ends so the clipboard is cleared before Hermit may exit.
//
// Included by clipboardsync.cpp only.

#include <QtGlobal>

#ifdef Q_OS_WIN32

#include "backend/nvhttp.h"
#include "clipboardarchive.h"
#include "clipboardsync.h"
#include "SDL_compat.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QNetworkReply>
#include <QThread>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shldisp.h>

#include <atomic>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ClipboardVirtualFiles {

constexpr qint64 k_DownloadBufferBytes = 4 * 1024 * 1024;  // per file being pasted
constexpr qint64 k_ReplyBufferBytes = 256 * 1024;          // what Qt may hold per request
// While waiting for the network, per attempt. The host may first have to download a cloud
// placeholder (OneDrive Files On-Demand) before the first byte, which can take minutes; File
// Explorer's dialog shows the copy waiting meanwhile and can cancel it.
constexpr qint64 k_InactivityTimeoutMs = 5 * 60000;
constexpr int k_TickMs = 10;
constexpr int k_MaxAttemptsWithoutProgress = 2;
constexpr qint64 k_PasteIdleMs = 2000;  // a paste is over (for the log) after this long without files
constexpr DWORD k_ReadWaitSliceMs = 1000;
constexpr DWORD k_QuitWaitMs = 1000;

// What the download thread needs to reach the host, copied from the NvComputer.
struct Connection
{
    NvAddress address;
    uint16_t httpsPort = 0;
    QSslCertificate serverCert;
    bool useTrueUid = true;
    qint64 rateBytesPerSecond = 0;  // 0: unlimited
};

// Starts GET /actions/clipboard?type=<type> on the download thread; parent owns what it creates.
using RequestFactory = std::function<QNetworkReply*(QObject* parent, const QString& type)>;

inline RequestFactory hostRequests(const Connection& connection)
{
    auto http = std::make_shared<NvHTTP*>(nullptr);
    return [connection, http](QObject* parent, const QString& type) {
        // Created on the download thread so the NvHTTP and its QNetworkAccessManager live there.
        if (*http == nullptr) {
            *http = new NvHTTP(connection.address, connection.httpsPort, connection.serverCert, connection.useTrueUid);
            (*http)->setParent(parent);
        }
        return (*http)->startClipboardRequest(type, nullptr, 0);
    };
}

inline CLIPFORMAT descriptorFormat()
{
    static const CLIPFORMAT format = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    return format;
}

inline CLIPFORMAT contentsFormat()
{
    static const CLIPFORMAT format = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    return format;
}

inline CLIPFORMAT preferredEffectFormat()
{
    static const CLIPFORMAT format = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return format;
}

// Marks clipboard content that is our own host file list, so it is never sent back to the host.
inline CLIPFORMAT markerFormat()
{
    static const CLIPFORMAT format = (CLIPFORMAT)RegisterClipboardFormatW(L"Hermit Host Files");
    return format;
}

// FILEDESCRIPTORW::cFileName holds MAX_PATH characters including the terminator.
inline bool fitsFileDescriptors(const ClipboardArchive::RemoteFileList& list)
{
    for (const ClipboardArchive::RemoteFile& entry : list.entries) {
        if (entry.path.size() >= MAX_PATH) {
            return false;
        }
    }
    return true;
}

inline FILETIME fileTimeFromUnixMs(qint64 ms)
{
    ULARGE_INTEGER value;
    value.QuadPart = (ULONGLONG)ms * 10000ULL + 116444736000000000ULL;  // 1970-01-01 as a FILETIME
    FILETIME time;
    time.dwLowDateTime = value.LowPart;
    time.dwHighDateTime = value.HighPart;
    return time;
}

inline HGLOBAL globalFromBytes(const void* data, SIZE_T size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size > 0 ? size : 1);
    if (global == nullptr) {
        return nullptr;
    }
    void* target = GlobalLock(global);
    if (target == nullptr) {
        GlobalFree(global);
        return nullptr;
    }
    if (size > 0) {
        memcpy(target, data, size);
    }
    GlobalUnlock(global);
    return global;
}

// The HRESULT a paste reports for a refused file request. File Explorer turns it into its own
// error message and offers to skip the file or try again.
inline HRESULT resultForHttpStatus(int status)
{
    switch (status) {
    case 401:
    case 403:
        return E_ACCESSDENIED;               // permission removed, or the stream ended
    case 404:
    case 410:
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);  // list replaced on the host
    case 409:
        return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);    // file changed or deleted on the host
    default:
        return HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR);
    }
}

// One file's bytes on their way from the host to an IStream: filled on the download thread, read
// on the owner thread.
class Download
{
public:
    Download(const QByteArray& snapshotId, int fileIndex, quint64 fileSize, quint64 startOffset)
        : snapshot(snapshotId),
          index(fileIndex),
          size(fileSize),
          offset(startOffset),
          m_Event(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          m_ReadPos(0),
          m_Received(0),
          m_Finished(false),
          m_Failure(S_OK),
          m_Abandoned(false)
    {
    }

    ~Download()
    {
        if (m_Event != nullptr) {
            CloseHandle(m_Event);
        }
    }

    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;

    const QByteArray snapshot;
    const int index;
    const quint64 size;    // whole file
    const quint64 offset;  // first byte to download

    quint64 expected() const { return size - offset; }

    // Signalled whenever data arrives or the download ends.
    HANDLE event() const { return m_Event; }

    // ---- Download thread

    qint64 space()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return k_DownloadBufferBytes - (m_Buffer.size() - m_ReadPos);
    }

    quint64 received()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Received;
    }

    // No longer wanted: the stream was released or moved, or the download failed (the stream
    // ended, for example).
    bool closed()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Abandoned || (m_Finished && FAILED(m_Failure));
    }

    void append(const char* data, qint64 bytes)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_ReadPos > 0 && m_ReadPos >= m_Buffer.size() / 2) {
                m_Buffer.remove(0, m_ReadPos);
                m_ReadPos = 0;
            }
            m_Buffer.append(data, (qsizetype)bytes);
            m_Received += (quint64)bytes;
            if (m_Received >= expected()) {
                m_Finished = true;
            }
        }
        SetEvent(m_Event);
    }

    void fail(HRESULT result)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_Finished) {
                m_Finished = true;
                m_Failure = result;
            }
        }
        SetEvent(m_Event);
    }

    // ---- Owner thread

    // Copies up to maxBytes of what has arrived. ended: nothing more will come (failure says why,
    // S_OK when the whole file arrived).
    qint64 take(char* out, qint64 maxBytes, bool& ended, HRESULT& failure)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const qint64 n = qMin<qint64>(maxBytes, m_Buffer.size() - m_ReadPos);
        if (n > 0) {
            memcpy(out, m_Buffer.constData() + m_ReadPos, (size_t)n);
            m_ReadPos += n;
        }
        ended = m_Finished && m_ReadPos == m_Buffer.size();
        failure = m_Failure;
        return n;
    }

    // The stream was released or moved elsewhere; the download thread drops the request.
    void abandon()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Abandoned = true;
    }

private:
    HANDLE m_Event;
    std::mutex m_Mutex;
    QByteArray m_Buffer;
    qsizetype m_ReadPos;
    quint64 m_Received;
    bool m_Finished;
    HRESULT m_Failure;
    bool m_Abandoned;
};

// Runs the file requests on the download thread: one request per file being read, all within the
// clipboard speed limit, cancelled by Ctrl+Alt+Shift+T. The host ends a response after 30 minutes
// and a network can drop, so a request that ends early continues from the bytes received.
class Fetcher : public QObject
{
public:
    Fetcher(RequestFactory requests, qint64 rateBytesPerSecond, std::shared_ptr<ClipboardTransferControl> control)
        : m_Requests(std::move(requests)),
          m_Control(std::move(control)),
          m_Tick(nullptr),
          m_Limiter(rateBytesPerSecond),
          m_Chunk(64 * 1024, Qt::Uninitialized),
          m_InService(false),
          m_PasteStartMs(-1),
          m_IdleSinceMs(-1),
          m_DoneFiles(0),
          m_DoneBytes(0)
    {
        m_Clock.start();
    }

    ~Fetcher() override
    {
        // The thread is ending (end of the stream): the hub already failed every download.
        for (Job& job : m_Jobs) {
            if (job.reply != nullptr) {
                QObject::disconnect(job.reply, nullptr, this, nullptr);
                job.reply->abort();
                delete job.reply;
            }
            job.download->fail(HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR));
            m_Control->streams.fetch_sub(1);
        }
    }

    // Download thread
    void start(const std::shared_ptr<Download>& download)
    {
        // Explorer opens the files one after another; they count as one paste for the log.
        if (m_PasteStartMs < 0) {
            m_PasteStartMs = m_Clock.elapsed();
            m_DoneFiles = 0;
            m_DoneBytes = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Pasting host files: downloading while File Explorer copies them");
        }
        m_IdleSinceMs = -1;
        Job job;
        job.download = download;
        job.cancelEpoch = m_Control->streamCancel.load();
        m_Control->streams.fetch_add(1);
        m_Jobs.push_back(job);
        sendRequest(m_Jobs.back());
        if (m_Tick == nullptr) {
            m_Tick = new QTimer(this);
            m_Tick->setTimerType(Qt::PreciseTimer);
            m_Tick->setInterval(k_TickMs);
            connect(m_Tick, &QTimer::timeout, this, [this]() { service(); });
        }
        if (!m_Tick->isActive()) {
            m_Tick->start();
        }
    }

private:
    struct Job
    {
        std::shared_ptr<Download> download;
        QNetworkReply* reply = nullptr;
        quint64 attemptStart = 0;  // bytes received when the current request started
        int attemptsWithoutProgress = 0;
        qint64 lastActivityMs = 0;
        int cancelEpoch = 0;
        bool checkedLength = false;
    };

    void sendRequest(Job& job)
    {
        const Download& download = *job.download;
        job.attemptStart = job.download->received();
        job.checkedLength = false;
        job.lastActivityMs = m_Clock.elapsed();
        const QString type = QStringLiteral("filedata&snapshot=%1&index=%2&offset=%3")
                                 .arg(QString::fromLatin1(download.snapshot))
                                 .arg(download.index)
                                 .arg(download.offset + job.attemptStart);
        job.reply = m_Requests(this, type);
        // Qt stops reading the socket while this buffer is full, so the host waits for us.
        job.reply->setReadBufferSize(qMax<qint64>(k_ReplyBufferBytes, m_Limiter.burst() * 2));
        connect(job.reply, &QNetworkReply::metaDataChanged, this, [this]() { service(); });
        connect(job.reply, &QNetworkReply::readyRead, this, [this]() { service(); });
        connect(job.reply, &QNetworkReply::finished, this, [this]() { service(); });
    }

    void dropReply(Job& job)
    {
        if (job.reply == nullptr) {
            return;
        }
        // Disconnected first: abort() emits finished, which would re-enter service().
        QObject::disconnect(job.reply, nullptr, this, nullptr);
        if (!job.reply->isFinished()) {
            job.reply->abort();
        }
        // Possibly called from one of the reply's own signals.
        job.reply->deleteLater();
        job.reply = nullptr;
    }

    void failJob(Job& job, HRESULT result, const QString& why)
    {
        dropReply(job);
        job.download->fail(result);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host file %d not pasted: %s",
                    job.download->index, qPrintable(why));
    }

    // The request ended before the whole file arrived: continue from where it stopped, unless the
    // previous attempts made no progress either.
    bool retryOrFail(Job& job, const QString& why)
    {
        const quint64 received = job.download->received();
        if (received > job.attemptStart) {
            job.attemptsWithoutProgress = 0;
        }
        else {
            job.attemptsWithoutProgress++;
        }
        if (job.attemptsWithoutProgress >= k_MaxAttemptsWithoutProgress) {
            failJob(job, HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR), why);
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host file %d: download stopped at %llu of %llu bytes (%s); continuing",
                    job.download->index, (unsigned long long)received,
                    (unsigned long long)job.download->expected(), qPrintable(why));
        dropReply(job);
        sendRequest(job);
        return true;
    }

    // False when the job is over (complete, failed, cancelled or no longer wanted).
    bool serviceJob(Job& job, qint64 now)
    {
        Download& download = *job.download;
        if (download.closed()) {
            dropReply(job);
            return false;
        }
        if (m_Control->streamCancel.load() != job.cancelEpoch) {
            failJob(job, HRESULT_FROM_WIN32(ERROR_CANCELLED), QStringLiteral("cancelled"));
            return false;
        }
        QNetworkReply* reply = job.reply;
        if (reply == nullptr) {
            return false;
        }

        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (status.isValid()) {
            const int code = status.toInt();
            if (code != 200) {
                failJob(job, resultForHttpStatus(code), QStringLiteral("host answered %1").arg(code));
                return false;
            }
            if (!job.checkedLength) {
                job.checkedLength = true;
                const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
                if (length.isValid() && length.toULongLong() != download.expected() - download.received()) {
                    failJob(job, HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR), QStringLiteral("unexpected length"));
                    return false;
                }
            }
            while (reply->bytesAvailable() > 0) {
                qint64 want = qMin<qint64>(m_Chunk.size(), reply->bytesAvailable());
                want = qMin(want, download.space());
                want = qMin<qint64>(want, (qint64)(download.expected() - download.received()));
                if (!m_Limiter.unlimited()) {
                    want = qMin(want, m_Limiter.available());
                }
                if (want <= 0) {
                    break;
                }
                const qint64 n = reply->read(m_Chunk.data(), want);
                if (n <= 0) {
                    break;
                }
                m_Limiter.consume(n);
                download.append(m_Chunk.constData(), n);
                job.lastActivityMs = now;
            }
            if (download.received() >= download.expected()) {
                dropReply(job);
                m_DoneFiles++;
                m_DoneBytes += download.expected();
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                             "Host file %d pasted (%llu bytes)",
                             download.index, (unsigned long long)download.expected());
                return false;
            }
        }

        if (reply->isFinished() && reply->bytesAvailable() == 0) {
            return retryOrFail(job, reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("response ended early"));
        }
        if (download.space() <= 0) {
            job.lastActivityMs = now;  // Explorer is not reading (paused, or a slow disk): not a stall
        }
        else if (now - job.lastActivityMs > k_InactivityTimeoutMs) {
            return retryOrFail(job, QStringLiteral("no data for %1 s").arg(k_InactivityTimeoutMs / 1000));
        }
        return true;
    }

    void service()
    {
        if (m_InService) {
            return;
        }
        m_InService = true;
        const qint64 now = m_Clock.elapsed();
        for (size_t i = 0; i < m_Jobs.size();) {
            if (serviceJob(m_Jobs[i], now)) {
                i++;
                continue;
            }
            m_Jobs.erase(m_Jobs.begin() + (std::ptrdiff_t)i);
            m_Control->streams.fetch_sub(1);
        }
        if (m_Jobs.empty() && m_PasteStartMs >= 0) {
            if (m_IdleSinceMs < 0) {
                m_IdleSinceMs = now;
            }
            else if (now - m_IdleSinceMs >= k_PasteIdleMs) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Pasting host files: %d file(s) downloaded (%llu KB) in %lld ms",
                            m_DoneFiles, (unsigned long long)(m_DoneBytes / 1024), (long long)(m_IdleSinceMs - m_PasteStartMs));
                m_PasteStartMs = -1;
                m_Tick->stop();
            }
        }
        m_InService = false;
    }

    RequestFactory m_Requests;
    std::shared_ptr<ClipboardTransferControl> m_Control;
    QTimer* m_Tick;
    ClipboardArchive::RateLimiter m_Limiter;  // shared by all files, like one transfer
    QByteArray m_Chunk;
    std::vector<Job> m_Jobs;
    QElapsedTimer m_Clock;
    bool m_InService;
    qint64 m_PasteStartMs;  // -1 when no paste is running
    qint64 m_IdleSinceMs;   // -1 while files are downloading
    int m_DoneFiles;
    quint64 m_DoneBytes;
};

// Shared by the main thread, the owner thread and the download thread.
class Hub
{
public:
    Hub()
        : stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_Stopped(false),
          m_Fetcher(nullptr)
    {
    }

    ~Hub()
    {
        if (stopEvent != nullptr) {
            CloseHandle(stopEvent);
        }
    }

    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // Set when the stream ends, so reads stop waiting at once.
    const HANDLE stopEvent;

    bool stopped() const { return m_Stopped.load(); }

    void setFetcher(Fetcher* fetcher)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Fetcher = fetcher;
    }

    // Owner thread: hands a download to the download thread. False once the stream ended.
    bool startDownload(const std::shared_ptr<Download>& download)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped.load() || m_Fetcher == nullptr) {
            return false;
        }
        for (size_t i = 0; i < m_Live.size();) {
            if (m_Live[i].expired()) {
                m_Live.erase(m_Live.begin() + (std::ptrdiff_t)i);
            }
            else {
                i++;
            }
        }
        m_Live.push_back(download);
        // The fetcher is deleted only after stop() cleared m_Fetcher and its thread ended.
        Fetcher* fetcher = m_Fetcher;
        QMetaObject::invokeMethod(fetcher, [fetcher, download]() { fetcher->start(download); }, Qt::QueuedConnection);
        return true;
    }

    // Main thread, when the stream ends: every download fails and waiting reads return.
    void stop()
    {
        std::vector<std::shared_ptr<Download>> live;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stopped.store(true);
            m_Fetcher = nullptr;
            for (const std::weak_ptr<Download>& weak : m_Live) {
                if (std::shared_ptr<Download> download = weak.lock()) {
                    live.push_back(download);
                }
            }
            m_Live.clear();
        }
        for (const std::shared_ptr<Download>& download : live) {
            download->fail(HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR));
        }
        SetEvent(stopEvent);
    }

private:
    std::atomic<bool> m_Stopped;
    std::mutex m_Mutex;
    Fetcher* m_Fetcher;
    std::vector<std::weak_ptr<Download>> m_Live;
};

// CFSTR_FILECONTENTS for one file. The download starts at the first Read and continues as File
// Explorer reads; Read waits until the requested bytes arrived (fewer only at the end of the file)
// and fails if the download fails, is cancelled or the stream ends, so Explorer reports the copy as
// failed instead of waiting forever.
class RemoteFileStream : public IStream
{
public:
    RemoteFileStream(std::shared_ptr<Hub> hub, const QByteArray& snapshot, int index, const ClipboardArchive::RemoteFile& file)
        : m_Refs(1),
          m_Hub(std::move(hub)),
          m_Snapshot(snapshot),
          m_Index(index),
          m_Size(file.size),
          m_Modified(fileTimeFromUnixMs(file.modifiedMs)),
          m_Position(0),
          m_InRead(false)
    {
        const QString name = file.path.mid(file.path.lastIndexOf(QLatin1Char('/')) + 1);
        m_Name = name.toStdWString();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream)) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_Refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override
    {
        ULONG copied = 0;
        const HRESULT result = readSome(static_cast<char*>(data), size, copied);
        if (read != nullptr) {
            *read = copied;
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPosition) override
    {
        qint64 base = 0;
        switch (origin) {
        case STREAM_SEEK_SET:
            base = 0;
            break;
        case STREAM_SEEK_CUR:
            base = (qint64)m_Position;
            break;
        case STREAM_SEEK_END:
            base = (qint64)m_Size;
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        const qint64 target = base + move.QuadPart;
        if (target < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        if ((quint64)target != m_Position) {
            if (m_InRead) {
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            // The next Read downloads from the new position.
            if (m_Download) {
                m_Download->abandon();
                m_Download.reset();
            }
            m_Position = (quint64)target;
        }
        if (newPosition != nullptr) {
            newPosition->QuadPart = m_Position;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER bytes, ULARGE_INTEGER* readOut, ULARGE_INTEGER* writtenOut) override
    {
        if (target == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::vector<char> buffer(256 * 1024);
        ULONGLONG remaining = bytes.QuadPart;
        ULONGLONG totalRead = 0, totalWritten = 0;
        HRESULT result = S_OK;
        while (remaining > 0) {
            ULONG got = 0;
            result = Read(buffer.data(), (ULONG)qMin<ULONGLONG>(remaining, buffer.size()), &got);
            if (FAILED(result) || got == 0) {
                break;
            }
            totalRead += got;
            ULONG put = 0;
            result = target->Write(buffer.data(), got, &put);
            totalWritten += put;
            if (FAILED(result)) {
                break;
            }
            remaining -= got;
        }
        if (readOut != nullptr) {
            readOut->QuadPart = totalRead;
        }
        if (writtenOut != nullptr) {
            writtenOut->QuadPart = totalWritten;
        }
        return FAILED(result) ? result : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override
    {
        if (stat == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = m_Size;
        stat->mtime = m_Modified;
        stat->grfMode = STGM_READ;
        if (!(flags & STATFLAG_NONAME)) {
            const size_t bytes = (m_Name.size() + 1) * sizeof(wchar_t);
            stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
            if (stat->pwcsName == nullptr) {
                return E_OUTOFMEMORY;
            }
            memcpy(stat->pwcsName, m_Name.c_str(), bytes);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream** stream) override
    {
        if (stream != nullptr) {
            *stream = nullptr;
        }
        return E_NOTIMPL;
    }

private:
    ~RemoteFileStream()
    {
        if (m_Download) {
            m_Download->abandon();
        }
    }

    HRESULT readSome(char* out, ULONG size, ULONG& copied)
    {
        copied = 0;
        if (out == nullptr && size > 0) {
            return STG_E_INVALIDPOINTER;
        }
        if (m_InRead) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);  // re-entered while waiting; Explorer reads in order
        }
        if (size == 0 || m_Position >= m_Size) {
            return S_OK;  // end of the file
        }
        if (!m_Download) {
            m_Download = std::make_shared<Download>(m_Snapshot, m_Index, m_Size, m_Position);
            if (!m_Hub->startDownload(m_Download)) {
                m_Download.reset();
                return HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR);
            }
        }

        m_InRead = true;
        HRESULT result = S_OK;
        for (;;) {
            bool ended = false;
            HRESULT failure = S_OK;
            const qint64 n = m_Download->take(out + copied, (qint64)(size - copied), ended, failure);
            copied += (ULONG)n;
            m_Position += (quint64)n;
            if (copied == size || m_Position >= m_Size) {
                break;
            }
            if (ended) {
                // Bytes copied so far are returned now; the next Read reports the failure.
                if (copied == 0) {
                    result = FAILED(failure) ? failure : HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR);
                }
                break;
            }
            if (m_Hub->stopped()) {
                if (copied == 0) {
                    result = HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR);  // the stream ended
                }
                break;
            }
            // Waits for more data. COM calls and sent messages (another program emptying the
            // clipboard, for example) are still handled meanwhile.
            HANDLE handles[2] = {m_Download->event(), m_Hub->stopEvent};
            DWORD which = 0;
            const HRESULT waited = CoWaitForMultipleHandles(0, k_ReadWaitSliceMs, 2, handles, &which);
            if (FAILED(waited) && waited != RPC_S_CALLPENDING) {
                if (copied == 0) {
                    result = waited;
                }
                break;
            }
        }
        m_InRead = false;
        return result;
    }

    std::atomic<ULONG> m_Refs;
    std::shared_ptr<Hub> m_Hub;
    QByteArray m_Snapshot;
    int m_Index;
    quint64 m_Size;
    FILETIME m_Modified;
    std::wstring m_Name;
    quint64 m_Position;
    bool m_InRead;
    std::shared_ptr<Download> m_Download;
};

// Data objects the owner thread put on the clipboard that are still alive. One the owner no longer
// references may still be on the clipboard (released while it was current, for example), so the
// end of the stream checks all of them, not only the latest.
class PublishedObjects
{
public:
    void add(IDataObject* object)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Objects.push_back(object);
    }

    void remove(IDataObject* object)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Objects.erase(std::remove(m_Objects.begin(), m_Objects.end(), object), m_Objects.end());
    }

    int count()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return (int)m_Objects.size();
    }

    // Owner thread: whether one of them is on the clipboard now.
    bool anyOnClipboard()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (IDataObject* object : m_Objects) {
            if (OleIsCurrentClipboard(object) == S_OK) {
                return true;
            }
        }
        return false;
    }

private:
    std::mutex m_Mutex;
    std::vector<IDataObject*> m_Objects;
};

// Commands from the main thread to the owner thread. Each publish and release gets the next
// generation number. The owner thread runs commands only between messages, so a long Read can hold
// several back; a publish that a later publish or release superseded is then dropped instead of
// covering newer host content with an older file list.
class CommandQueue
{
public:
    enum Kind { Publish, Release, Quit };

    struct Command
    {
        Kind kind = Quit;
        quint64 generation = 0;
        ClipboardArchive::RemoteFileList list;
    };

    CommandQueue()
        : m_Latest(0)
    {
    }

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    // Main thread. Returns the command's generation (0 for Quit, which supersedes nothing).
    quint64 post(Kind kind, const ClipboardArchive::RemoteFileList& list = ClipboardArchive::RemoteFileList())
    {
        Command command;
        command.kind = kind;
        command.list = list;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (kind != Quit) {
            command.generation = ++m_Latest;
        }
        m_Commands.push_back(command);
        return command.generation;
    }

    // Whether nothing was published or released after the command with this generation.
    bool isLatest(quint64 generation) const { return generation == m_Latest.load(); }

    // Owner thread: the queued commands, without the publishes already superseded.
    std::deque<Command> take()
    {
        std::deque<Command> commands;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            commands.swap(m_Commands);
        }
        std::deque<Command> wanted;
        for (Command& command : commands) {
            if (command.kind == Publish && !isLatest(command.generation)) {
                continue;
            }
            wanted.push_back(std::move(command));
        }
        return wanted;
    }

private:
    std::mutex m_Mutex;
    std::deque<Command> m_Commands;
    std::atomic<quint64> m_Latest;
};

// Waits up to ms while still handling this thread's messages, so COM calls into our objects and
// clipboard messages keep being answered (a plain Sleep would hold up other programs while our
// window owns the clipboard).
inline void waitHandlingMessages(DWORD ms)
{
    const ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= end) {
            return;
        }
        const DWORD woken = MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(end - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (woken != WAIT_OBJECT_0) {
            return;  // time is up (or the wait failed)
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// OleSetClipboard, tried again for a moment while another program holds the clipboard. stillWanted,
// if given, is asked before every attempt (the wait between them handles messages, so a newer
// command can arrive meanwhile); E_ABORT once it says no.
inline HRESULT setClipboardWithRetry(IDataObject* object, const std::function<bool()>& stillWanted = {})
{
    HRESULT result = E_FAIL;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (stillWanted && !stillWanted()) {
            return E_ABORT;
        }
        result = OleSetClipboard(object);
        if (result != CLIPBRD_E_CANT_OPEN) {
            break;
        }
        waitHandlingMessages(15);
    }
    return result;
}

// Empties the clipboard only while stillOurs() says it holds our content, asked with the
// clipboard open so no other program (nor the main thread writing newer host content) can change it
// in between. OleSetClipboard(nullptr) would empty it whoever owns it. Tried again for a moment
// while another program holds the clipboard; false if it could not be opened. Emptying sends
// WM_DESTROYCLIPBOARD to OLE's clipboard window on this thread, which releases our data object.
inline bool emptyClipboardIfOurs(const std::function<bool()>& stillOurs)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (OpenClipboard(nullptr)) {
            const bool ours = stillOurs();
            const bool emptied = ours && EmptyClipboard();
            CloseClipboard();
            return !ours || emptied;
        }
        waitHandlingMessages(15);
    }
    return false;
}

// The data object on the clipboard: file descriptors, file contents by index, "copy" as the
// preferred effect, and our marker. Implements IDataObjectAsyncCapability so File Explorer pastes on
// a background thread with its progress dialog.
class HostFilesDataObject : public IDataObject, public IDataObjectAsyncCapability
{
public:
    // published: where the owner thread keeps track of its objects that are still alive
    HostFilesDataObject(std::shared_ptr<Hub> hub, const ClipboardArchive::RemoteFileList& list,
                        std::shared_ptr<PublishedObjects> published = nullptr)
        : m_Refs(1),
          m_Hub(std::move(hub)),
          m_List(list),
          m_Published(std::move(published)),
          m_AsyncMode(TRUE),
          m_InOperation(FALSE)
    {
        if (m_Published) {
            m_Published->add(this);
        }
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *object = static_cast<IDataObject*>(this);
        }
        else if (riid == __uuidof(IDataObjectAsyncCapability)) {
            *object = static_cast<IDataObjectAsyncCapability*>(this);
        }
        else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_Refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr) {
            return E_INVALIDARG;
        }
        memset(medium, 0, sizeof(*medium));
        const HRESULT supported = QueryGetData(format);
        if (FAILED(supported)) {
            return supported;
        }
        const CLIPFORMAT cf = format->cfFormat;
        if (cf == contentsFormat()) {
            if (format->lindex < 0 || format->lindex >= (LONG)m_List.entries.size() || m_List.entries[format->lindex].directory) {
                return DV_E_LINDEX;
            }
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = new RemoteFileStream(m_Hub, m_List.snapshot, (int)format->lindex, m_List.entries[format->lindex]);
            return S_OK;
        }

        HGLOBAL global = nullptr;
        if (cf == descriptorFormat()) {
            global = makeDescriptors();
        }
        else if (cf == preferredEffectFormat()) {
            const DWORD effect = DROPEFFECT_COPY;  // pasting copies; the host files stay where they are
            global = globalFromBytes(&effect, sizeof(effect));
        }
        else if (cf == markerFormat()) {
            const DWORD marker = 1;
            global = globalFromBytes(&marker, sizeof(marker));
        }
        else {
            for (const auto& stored : m_Stored) {
                if (stored.first == cf) {
                    global = globalFromBytes(stored.second.data(), stored.second.size());
                    break;
                }
            }
        }
        if (global == nullptr) {
            return E_OUTOFMEMORY;
        }
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (format == nullptr) {
            return E_INVALIDARG;
        }
        if (format->dwAspect != DVASPECT_CONTENT) {
            return DV_E_DVASPECT;
        }
        const CLIPFORMAT cf = format->cfFormat;
        if (cf == contentsFormat()) {
            return (format->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
        }
        bool known = cf == descriptorFormat() || cf == preferredEffectFormat() || cf == markerFormat();
        for (const auto& stored : m_Stored) {
            known = known || stored.first == cf;
        }
        if (!known) {
            return DV_E_FORMATETC;
        }
        return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override
    {
        if (out == nullptr) {
            return E_INVALIDARG;
        }
        if (in != nullptr) {
            *out = *in;
        }
        out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override
    {
        if (format == nullptr || medium == nullptr) {
            return E_INVALIDARG;
        }
        const CLIPFORMAT cf = format->cfFormat;
        if (cf == descriptorFormat() || cf == contentsFormat() || cf == markerFormat() || cf == preferredEffectFormat()) {
            return E_NOTIMPL;
        }
        if (medium->tymed != TYMED_HGLOBAL || medium->hGlobal == nullptr) {
            return DV_E_TYMED;
        }
        // Paste targets report what they did ("Performed DropEffect", "Paste Succeeded"); kept so
        // they can read it back.
        const SIZE_T size = GlobalSize(medium->hGlobal);
        const void* data = GlobalLock(medium->hGlobal);
        if (data == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::string bytes(static_cast<const char*>(data), size);
        GlobalUnlock(medium->hGlobal);
        bool replaced = false;
        for (auto& stored : m_Stored) {
            if (stored.first == cf) {
                stored.second = bytes;
                replaced = true;
            }
        }
        if (!replaced) {
            m_Stored.emplace_back(cf, std::move(bytes));
        }
        if (release) {
            ReleaseStgMedium(medium);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) override
    {
        if (formats == nullptr) {
            return E_INVALIDARG;
        }
        *formats = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        FORMATETC list[] = {
            {descriptorFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {contentsFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
            {preferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {markerFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        return SHCreateStdEnumFmtEtc((UINT)(sizeof(list) / sizeof(list[0])), list, formats);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

    // IDataObjectAsyncCapability
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL async) override
    {
        m_AsyncMode = async;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* async) override
    {
        if (async == nullptr) {
            return E_INVALIDARG;
        }
        *async = m_AsyncMode;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override
    {
        m_InOperation = TRUE;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "File Explorer started pasting %d host item(s)", m_List.topLevelCount());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InOperation(BOOL* inOperation) override
    {
        if (inOperation == nullptr) {
            return E_INVALIDARG;
        }
        *inOperation = m_InOperation;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD effects) override
    {
        m_InOperation = FALSE;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "File Explorer finished pasting host files (result 0x%08lx, effect %lu)",
                    (unsigned long)result, (unsigned long)effects);
        return S_OK;
    }

private:
    ~HostFilesDataObject()
    {
        if (m_Published) {
            m_Published->remove(this);
        }
    }

    HGLOBAL makeDescriptors() const
    {
        const UINT count = (UINT)m_List.entries.size();
        const SIZE_T bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + (SIZE_T)count * sizeof(FILEDESCRIPTORW);
        HGLOBAL global = GlobalAlloc(GHND, bytes);  // zero-filled, so every name is terminated
        if (global == nullptr) {
            return nullptr;
        }
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(global));
        if (group == nullptr) {
            GlobalFree(global);
            return nullptr;
        }
        group->cItems = count;
        FILEDESCRIPTORW* descriptors = group->fgd;
        for (UINT i = 0; i < count; i++) {
            const ClipboardArchive::RemoteFile& entry = m_List.entries[(int)i];
            FILEDESCRIPTORW& descriptor = descriptors[i];
            descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_PROGRESSUI;
            descriptor.dwFileAttributes = entry.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            descriptor.nFileSizeHigh = (DWORD)(entry.size >> 32);
            descriptor.nFileSizeLow = (DWORD)(entry.size & 0xFFFFFFFFULL);
            if (entry.modifiedMs > 0) {
                descriptor.dwFlags |= FD_WRITESTIME;
                descriptor.ftLastWriteTime = fileTimeFromUnixMs(entry.modifiedMs);
            }
            // Relative path with backslashes; fitsFileDescriptors() checked the length.
            QString name = entry.path;
            name.replace(QLatin1Char('/'), QLatin1Char('\\'));
            memcpy(descriptor.cFileName, name.utf16(), (size_t)qMin<qsizetype>(name.size(), MAX_PATH - 1) * sizeof(wchar_t));
        }
        GlobalUnlock(global);
        return global;
    }

    std::atomic<ULONG> m_Refs;
    std::shared_ptr<Hub> m_Hub;
    ClipboardArchive::RemoteFileList m_List;
    std::shared_ptr<PublishedObjects> m_Published;
    std::vector<std::pair<CLIPFORMAT, std::string>> m_Stored;
    BOOL m_AsyncMode;
    BOOL m_InOperation;
};

// The single-threaded apartment that owns our clipboard content. Commands from the main thread
// run between messages, never inside a Read that is waiting.
class Owner : public std::enable_shared_from_this<Owner>
{
public:
    Owner(std::shared_ptr<Hub> hub, int sdlEventCode, int generation)
        : m_Hub(std::move(hub)),
          m_SdlEventCode(sdlEventCode),
          m_Generation(generation),
          m_Wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          m_Done(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_Published(std::make_shared<PublishedObjects>()),
          m_Current(nullptr)
    {
    }

    ~Owner()
    {
        CloseHandle(m_Wake);
        CloseHandle(m_Done);
    }

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

    void start()
    {
        std::shared_ptr<Owner> self = shared_from_this();
        std::thread([self]() { self->run(); }).detach();
    }

    // Main thread. Both return the command's generation.
    quint64 publish(const ClipboardArchive::RemoteFileList& list) { return post(CommandQueue::Publish, list); }
    quint64 release() { return post(CommandQueue::Release); }

    // Main thread: false once a later publish or release was posted.
    bool isLatest(quint64 generation) const { return m_Commands.isLatest(generation); }

    // Main thread, when the stream ends: clears the clipboard if it still holds our files (their
    // downloads stop with the stream) and ends the thread. Waits at most timeoutMs.
    bool quit(DWORD timeoutMs)
    {
        post(CommandQueue::Quit);
        return WaitForSingleObject(m_Done, timeoutMs) == WAIT_OBJECT_0;
    }

private:
    quint64 post(CommandQueue::Kind kind, const ClipboardArchive::RemoteFileList& list = ClipboardArchive::RemoteFileList())
    {
        const quint64 generation = m_Commands.post(kind, list);
        SetEvent(m_Wake);
        return generation;
    }

    void run()
    {
        // OleSetClipboard needs OLE, which makes this thread a single-threaded apartment.
        const HRESULT init = OleInitialize(nullptr);
        if (FAILED(init)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files cannot be offered for paste: OLE initialization failed (0x%08lx)",
                        (unsigned long)init);
        }
        bool running = true;
        while (running) {
            const DWORD woken = MsgWaitForMultipleObjectsEx(1, &m_Wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (woken == WAIT_OBJECT_0) {
                running = runCommands(SUCCEEDED(init));
            }
            else if (woken == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            else {
                finish(SUCCEEDED(init));
                running = false;
            }
        }
        if (SUCCEEDED(init)) {
            // Disconnects anything File Explorer still holds; its calls then fail.
            OleUninitialize();
        }
        SetEvent(m_Done);
    }

    bool runCommands(bool oleReady)
    {
        for (const CommandQueue::Command& command : m_Commands.take()) {
            switch (command.kind) {
            case CommandQueue::Publish:
                if (oleReady && !m_Hub->stopped()) {
                    setClipboard(command.list, command.generation);
                }
                break;
            case CommandQueue::Release:
                releaseCurrent();
                break;
            case CommandQueue::Quit:
                finish(oleReady);
                return false;
            }
        }
        return true;
    }

    void setClipboard(const ClipboardArchive::RemoteFileList& list, quint64 generation)
    {
        // An earlier command in this batch may have taken a while, and the retry below handles
        // messages while it waits: checked before every attempt, so a list that newer host
        // content superseded is not put on the clipboard.
        auto* object = new HostFilesDataObject(m_Hub, list, m_Published);
        const HRESULT result = setClipboardWithRetry(object, [this, generation]() { return m_Commands.isLatest(generation); });
        if (result == E_ABORT) {
            object->Release();
            return;
        }
        if (FAILED(result)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files could not be put on the clipboard (0x%08lx)", (unsigned long)result);
            object->Release();
            // The main thread forgets the host sequence number, so the next pull offers it again.
            auto* failed = new ClipboardHostContent;
            failed->kind = ClipboardHostContent::RemoteFilesFailed;
            failed->generation = m_Generation;
            failed->listGeneration = generation;
            failed->hostSeq = list.seq;
            pushContent(failed);
            return;
        }
        const DWORD sequence = GetClipboardSequenceNumber();
        releaseCurrent();
        m_Current = object;
        if (!m_Commands.isLatest(generation)) {
            // Superseded between the last check and the set: the main thread may already have
            // written the newer content, which this list would now cover. Handled like the Release
            // that is on its way, right now: emptied while the clipboard still holds this list.
            // Emptied only while this list is still the clipboard content, asked with the
            // clipboard open on every attempt: newer content written meanwhile stays.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Host files superseded as they went on the clipboard; removed again");
            emptyClipboardIfOurs([object]() { return OleIsCurrentClipboard(object) == S_OK; });
            releaseCurrent();
            return;
        }

        // The main thread marks this clipboard content as handled, so it is not sent back, unless
        // newer host content superseded this list meanwhile (listGeneration).
        auto* content = new ClipboardHostContent;
        content->kind = ClipboardHostContent::RemoteFilesReady;
        content->generation = m_Generation;
        content->listGeneration = generation;
        content->localSeq = sequence;
        content->itemCount = list.topLevelCount();
        pushContent(content);
    }

    // Hands content to the main thread (onHostContent), which takes ownership.
    void pushContent(ClipboardHostContent* content)
    {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = m_SdlEventCode;
        event.user.data1 = content;
        if (SDL_PushEvent(&event) != 1) {
            delete content;
        }
    }

    // Drops our reference; the clipboard keeps its own while the list is on it.
    void releaseCurrent()
    {
        if (m_Current != nullptr) {
            m_Current->Release();
            m_Current = nullptr;
        }
    }

    // End of the stream: empties the clipboard if any list we put there is still on it (their
    // downloads stop with the stream), then drops our reference. Emptied rather than flushed:
    // OleFlushClipboard would download every file. Whether one is still there is asked again with
    // the clipboard open, so content another program wrote meanwhile is not emptied.
    void finish(bool oleReady)
    {
        if (oleReady && m_Published->anyOnClipboard()) {
            std::shared_ptr<PublishedObjects> published = m_Published;
            if (!emptyClipboardIfOurs([published]() { return published->anyOnClipboard(); })) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Host files could not be removed from the clipboard (it stayed busy)");
            }
        }
        releaseCurrent();
    }

    std::shared_ptr<Hub> m_Hub;
    int m_SdlEventCode;
    int m_Generation;
    HANDLE m_Wake;
    HANDLE m_Done;
    CommandQueue m_Commands;
    std::shared_ptr<PublishedObjects> m_Published;
    IDataObject* m_Current;  // owner thread only
};

// Main-thread side, created when the first host file list arrives and deleted when the stream (or
// clipboard sync) ends.
class VirtualFileClipboard
{
public:
    VirtualFileClipboard(const Connection& connection, std::shared_ptr<ClipboardTransferControl> control,
                         int sdlEventCode, int generation)
        : m_Hub(std::make_shared<Hub>()),
          m_FetchThread(new QThread())
    {
        auto* fetcher = new Fetcher(hostRequests(connection), connection.rateBytesPerSecond, std::move(control));
        fetcher->moveToThread(m_FetchThread);
        // Both delete themselves once the thread stops, which also aborts the requests.
        QObject::connect(m_FetchThread, &QThread::finished, fetcher, &QObject::deleteLater);
        QObject::connect(m_FetchThread, &QThread::finished, m_FetchThread, &QObject::deleteLater);
        m_FetchThread->setObjectName("Clipboard file paste");
        m_FetchThread->start();
        m_Hub->setFetcher(fetcher);

        m_Owner = std::make_shared<Owner>(m_Hub, sdlEventCode, generation);
        m_Owner->start();
    }

    ~VirtualFileClipboard()
    {
        m_Hub->stop();
        m_FetchThread->quit();
        if (!m_Owner->quit(k_QuitWaitMs)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard: the host file list was not removed from the clipboard in time");
        }
    }

    VirtualFileClipboard(const VirtualFileClipboard&) = delete;
    VirtualFileClipboard& operator=(const VirtualFileClipboard&) = delete;

    // Puts the list on the clipboard (on the owner thread). The main thread then gets a
    // RemoteFilesReady content event with the returned generation.
    quint64 publish(const ClipboardArchive::RemoteFileList& list) { return m_Owner->publish(list); }

    // Other host content replaces ours on the clipboard: drop our reference, and never publish a
    // list still waiting. A paste that is already running keeps its streams.
    void release() { m_Owner->release(); }

    // False for the generation of a list that newer host content superseded.
    bool isLatest(quint64 listGeneration) const { return m_Owner->isLatest(listGeneration); }

private:
    std::shared_ptr<Hub> m_Hub;
    QThread* m_FetchThread;
    std::shared_ptr<Owner> m_Owner;
};

}  // namespace ClipboardVirtualFiles

#endif // Q_OS_WIN32
