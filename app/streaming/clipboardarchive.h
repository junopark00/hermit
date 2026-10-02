#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// APCF file archive used for clipboard file transfers (same format as the Shell host):
//   "APCF" | u32 version (1) | u32 entry count
//   entry: u8 kind (0 file, 1 directory) | u32 path length | UTF-8 path | u64 size | data
// All integers are little-endian; paths are relative with '/' separators.
//
// Archives are streamed: uploads are produced on the fly from the files on disk, downloads are
// validated and extracted from a file, so a whole archive is never held in memory.
namespace ClipboardArchive {

constexpr quint64 k_MaxFilesBytes = 256ULL * 1024 * 1024;  // same as the host
constexpr int k_MaxFileEntries = 1000;
constexpr qint64 k_MaxArchiveBytes = (qint64)k_MaxFilesBytes + k_MaxFileEntries * 1100LL + 12;
// Host files that are pasted as virtual files (fetched file by file while pasting, never as one
// archive) may total more, up to this limit (same as the host).
constexpr quint64 k_MaxStreamFilesBytes = 4ULL * 1024 * 1024 * 1024;

// A relative path with '/' separators whose every name Windows accepts as it is: no reserved
// device names, forbidden characters, trailing dots or spaces, at most 255 UTF-16 code units per
// name and 1024 UTF-8 bytes in all (the same rules as the Shell host).
bool isSafeRelativePath(const QString& path);

// Key for comparing paths without case, as Windows and the Shell host do (LCMAP_UPPERCASE): each
// UTF-16 code unit in its simple upper case form, so the length never changes (QString::toUpper
// would turn "ß" into "SS", which Windows does not).
QString foldPath(const QString& path);

struct Entry
{
    bool directory = false;
    QString path;          // relative, '/' separators
    QString sourcePath;    // upload: absolute local path
    quint64 size = 0;      // file size (0 for directories)
    qint64 dataOffset = 0; // download: offset of the data in the archive file
};

// Walks files and folders (recursively, never following links or junctions), checks names,
// duplicates and limits, and returns what would be packed together with the exact archive size.
// Nothing is read yet; each file is only opened once to check that it is readable.
bool planUpload(const QStringList& roots, QVector<Entry>& entries, qint64& archiveSize, QString& error);

// Token bucket. A rate of 0 means unlimited.
class RateLimiter
{
public:
    explicit RateLimiter(qint64 bytesPerSecond = 0);

    bool unlimited() const { return m_Rate <= 0; }
    qint64 rate() const { return m_Rate; }
    qint64 burst() const { return m_Burst; }

    // Bytes that may be moved now (a large number when unlimited).
    qint64 available();
    void consume(qint64 bytes);
    // Milliseconds until a useful amount of budget is available again.
    int msUntilAvailable();

private:
    void refill();

    qint64 m_Rate;
    qint64 m_Burst;
    double m_Tokens;
    QElapsedTimer m_Clock;
    qint64 m_LastNs;
};

// Sequential, read-only request body whose reads are throttled by a token bucket. When the
// budget is used up, read() returns 0 and readyRead() is emitted from a timer once it refills;
// end of data is reported with -1 only after exactly size bytes were produced.
class ThrottledUploadDevice : public QIODevice
{
public:
    ThrottledUploadDevice(qint64 totalSize, qint64 bytesPerSecond, QObject* parent = nullptr);

    bool isSequential() const override { return true; }
    bool atEnd() const override;
    qint64 bytesAvailable() const override;

    qint64 totalSize() const { return m_Total; }
    qint64 produced() const { return m_Produced; }
    bool failed() const { return m_Failed; }

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

    // Produces up to maxSize (> 0) bytes; never more than what remains of totalSize.
    // Returns the byte count, or -1 after calling fail().
    virtual qint64 produce(char* data, qint64 maxSize) = 0;
    void fail(const QString& message);

private:
    void scheduleWake();

    qint64 m_Total;
    qint64 m_Produced;
    bool m_Failed;
    bool m_WakeScheduled;
    RateLimiter m_Limiter;
};

// Request body from memory (clipboard images).
class BufferUploadDevice : public ThrottledUploadDevice
{
public:
    BufferUploadDevice(const QByteArray& data, qint64 bytesPerSecond, QObject* parent = nullptr);

protected:
    qint64 produce(char* data, qint64 maxSize) override;

private:
    QByteArray m_Data;
};

// APCF archive produced on the fly from planned entries. Fails (and the transfer with it) if a
// file cannot be opened or its size differs from the size measured when planning.
class ArchiveUploadDevice : public ThrottledUploadDevice
{
public:
    ArchiveUploadDevice(const QVector<Entry>& entries, qint64 archiveSize, qint64 bytesPerSecond, QObject* parent = nullptr);

    static qint64 archiveSize(const QVector<Entry>& entries);

protected:
    qint64 produce(char* data, qint64 maxSize) override;

private:
    bool startNextEntry();

    QVector<Entry> m_Entries;
    int m_NextEntry;
    bool m_HeaderWritten;
    QByteArray m_Pending;      // header bytes not yet produced
    qsizetype m_PendingPos;
    QFile m_File;
    quint64 m_FileRemaining;
};

// One item of a host file list (GET /actions/clipboard?type=filelist).
struct RemoteFile
{
    bool directory = false;
    QString path;          // relative, '/' separators
    quint64 size = 0;      // 0 for directories
    qint64 modifiedMs = 0; // last write time, Unix milliseconds (0 if unknown)
};

struct RemoteFileList
{
    quint32 seq = 0;       // host clipboard sequence number the list was made at
    QByteArray snapshot;   // id for GET type=filedata
    QVector<RemoteFile> entries;
    quint64 totalBytes = 0;

    // Items without a parent in the list (what the user copied).
    int topLevelCount() const;
};

// Parses and fully checks a host file list: header fields, every path (as for archives), sizes,
// duplicates, that each item's folder is listed before it, and the limits. The format is
//   seq=<n> | snapshot=<id> | entries=<count> | bytes=<total>   ("key=value" lines, any order)
//   <f|d> TAB <size> TAB <last write, Unix ms> TAB <path>        (one line per item, in order)
// Unknown "key=value" lines are ignored so the host can add fields later.
bool parseFileList(const QByteArray& body, RemoteFileList& list, QString& error);

// Checks a downloaded archive without writing anything: magic, version, entry count, every path,
// size, duplicate and the overall limits. Fills entries (with data offsets) on success.
bool validateArchive(QIODevice& archive, QVector<Entry>& entries, QString& error);

// Writes validated entries under a new "xfer-*" folder in rootPath and returns the top-level
// items. Older transfer folders are removed, keeping the newest few. If cancelled() returns
// true, stops, removes what was written and fails.
bool extractArchive(QIODevice& archive, const QVector<Entry>& entries, const QString& rootPath,
                    QStringList& topLevel, QString& error,
                    const std::function<bool()>& cancelled = std::function<bool()>());

}  // namespace ClipboardArchive
