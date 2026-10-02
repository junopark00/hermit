#include "clipboardarchive.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTimer>
#include <QtEndian>

#include <cstring>

#ifdef Q_OS_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ClipboardArchive {

namespace {

constexpr qint64 k_HeaderBytes = 12;          // "APCF" | version | count
constexpr qint64 k_EntryFixedBytes = 1 + 4 + 8; // kind | path length | size
constexpr qint64 k_CopyChunkBytes = 1024 * 1024;

bool isLinkOrJunction(const QFileInfo& info)
{
#ifdef Q_OS_WIN32
    const QString native = QDir::toNativeSeparators(info.absoluteFilePath());
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
#endif
    return info.isSymLink();
}

void appendU32(QByteArray& out, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

void appendU64(QByteArray& out, quint64 value)
{
    char bytes[8];
    qToLittleEndian(value, bytes);
    out.append(bytes, 8);
}

bool readExact(QIODevice& in, char* data, qint64 size)
{
    return in.read(data, size) == size;
}

}  // namespace

bool isSafeRelativePath(const QString& path)
{
    static const char* const k_Reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    static const QString k_Forbidden = QStringLiteral("\\:*?\"<>|");

    if (path.isEmpty() || path.toUtf8().size() > 1024) {
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString& part : parts) {
        // 255 UTF-16 code units per name, as Windows and the Shell host count them
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..") || part.size() > 255) {
            return false;
        }
        for (QChar c : part) {
            if (c.unicode() < 0x20 || k_Forbidden.contains(c)) {
                return false;
            }
        }
        // Windows strips trailing dots and spaces, which could alias another entry.
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) {
            return false;
        }
        const QString base = part.section(QLatin1Char('.'), 0, 0).toUpper();
        for (const char* reserved : k_Reserved) {
            if (base == QLatin1String(reserved)) {
                return false;
            }
        }
    }
    return true;
}

QString foldPath(const QString& path)
{
    QString key(path);
#ifdef Q_OS_WIN32
    // The same call as the Shell host, so both sides see the same duplicates. LCMAP_UPPERCASE maps
    // code units one to one, so the key keeps the path's length.
    if (!key.isEmpty()) {
        const int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                                         reinterpret_cast<LPCWSTR>(path.utf16()), path.size(),
                                         reinterpret_cast<LPWSTR>(key.data()), key.size(), nullptr, nullptr, 0);
        if (mapped == key.size()) {
            return key;
        }
        key = path;  // not mapped (an unexpected length or failure): the fallback below
    }
#endif
    for (QChar& c : key) {
        c = c.toUpper();
    }
    return key;
}

// ---- Upload planning ------------------------------------------------------------------------

bool planUpload(const QStringList& roots, QVector<Entry>& entries, qint64& archiveSize, QString& error)
{
    entries.clear();
    archiveSize = 0;
    QSet<QString> seen;
    quint64 total = 0;

    auto add = [&](const QFileInfo& info, const QDir& base) -> bool {
        Entry entry;
        entry.directory = info.isDir();
        entry.path = base.relativeFilePath(info.absoluteFilePath());
        entry.sourcePath = info.absoluteFilePath();
        if (!isSafeRelativePath(entry.path)) {
            error = QStringLiteral("unsupported file name: ") + entry.path;
            return false;
        }
        const QString key = foldPath(entry.path);
        if (seen.contains(key)) {
            error = QStringLiteral("duplicate name: ") + entry.path;
            return false;
        }
        if (entries.size() >= k_MaxFileEntries) {
            error = QStringLiteral("too many files (limit %1)").arg(k_MaxFileEntries);
            return false;
        }
        if (!entry.directory) {
            entry.size = (quint64)qMax<qint64>(0, info.size());
            total += entry.size;
            if (total > k_MaxFilesBytes) {
                error = QStringLiteral("files too large (limit %1 MB)").arg(k_MaxFilesBytes / (1024 * 1024));
                return false;
            }
            // Fail now rather than in the middle of the upload.
            QFile file(entry.sourcePath);
            if (!file.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("cannot read ") + entry.path;
                return false;
            }
        }
        seen.insert(key);
        entries.append(std::move(entry));
        return true;
    };

    for (const QString& root : roots) {
        const QFileInfo info(root);
        if (!info.exists() || isLinkOrJunction(info)) {
            continue;
        }
        if (info.fileName().isEmpty()) {
            error = QStringLiteral("cannot copy a whole drive");
            return false;
        }
        const QDir base = info.absoluteDir();
        if (info.isFile()) {
            if (!add(info, base)) {
                return false;
            }
            continue;
        }
        if (!info.isDir() || !add(info, base)) {
            if (!error.isEmpty()) {
                return false;
            }
            continue;
        }
        QStringList pending {info.absoluteFilePath()};
        while (!pending.isEmpty()) {
            const QDir dir(pending.takeLast());
            const QFileInfoList children = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo& child : children) {
                if (isLinkOrJunction(child)) {
                    continue;
                }
                if (child.isDir()) {
                    if (!add(child, base)) {
                        return false;
                    }
                    pending.append(child.absoluteFilePath());
                }
                else if (child.isFile()) {
                    if (!add(child, base)) {
                        return false;
                    }
                }
            }
        }
    }
    if (entries.isEmpty()) {
        error = QStringLiteral("nothing to copy");
        return false;
    }
    archiveSize = ArchiveUploadDevice::archiveSize(entries);
    return true;
}

// ---- Rate limiter ---------------------------------------------------------------------------

RateLimiter::RateLimiter(qint64 bytesPerSecond)
    : m_Rate(qMax<qint64>(0, bytesPerSecond)),
      // 50 ms worth of data, so short timer delays do not lower the average rate but bursts
      // stay small next to the video stream.
      m_Burst(qMax<qint64>(16 * 1024, m_Rate / 20)),
      m_Tokens(0),
      m_LastNs(0)
{
    m_Clock.start();
    // Start with one burst so the transfer begins at once.
    m_Tokens = (double)m_Burst;
}

void RateLimiter::refill()
{
    const qint64 now = m_Clock.nsecsElapsed();
    const qint64 elapsed = qMin<qint64>(now - m_LastNs, 1000000000LL);
    m_LastNs = now;
    m_Tokens = qMin((double)m_Burst, m_Tokens + (double)m_Rate * (double)elapsed / 1e9);
}

qint64 RateLimiter::available()
{
    if (unlimited()) {
        return 1LL << 40;
    }
    refill();
    return m_Tokens > 0 ? (qint64)m_Tokens : 0;
}

void RateLimiter::consume(qint64 bytes)
{
    if (!unlimited()) {
        m_Tokens -= (double)bytes;
    }
}

int RateLimiter::msUntilAvailable()
{
    if (unlimited()) {
        return 0;
    }
    refill();
    // Wait for a quarter burst rather than a single byte, to avoid tiny reads.
    const double needed = (double)(m_Burst / 4) - m_Tokens;
    if (needed <= 0) {
        return 0;
    }
    return qBound(1, (int)(needed * 1000.0 / (double)m_Rate) + 1, 100);
}

// ---- Upload devices -------------------------------------------------------------------------

ThrottledUploadDevice::ThrottledUploadDevice(qint64 totalSize, qint64 bytesPerSecond, QObject* parent)
    : QIODevice(parent),
      m_Total(totalSize),
      m_Produced(0),
      m_Failed(false),
      m_WakeScheduled(false),
      m_Limiter(bytesPerSecond)
{
}

bool ThrottledUploadDevice::atEnd() const
{
    return m_Failed || m_Produced >= m_Total;
}

qint64 ThrottledUploadDevice::bytesAvailable() const
{
    // What is left to send (reads may still return 0 while throttled), so a reader never takes
    // an empty budget for the end of the data.
    return QIODevice::bytesAvailable() + (m_Failed ? 0 : m_Total - m_Produced);
}

void ThrottledUploadDevice::fail(const QString& message)
{
    if (!m_Failed) {
        m_Failed = true;
        setErrorString(message);
    }
}

void ThrottledUploadDevice::scheduleWake()
{
    if (m_WakeScheduled) {
        return;
    }
    m_WakeScheduled = true;
    QTimer::singleShot(m_Limiter.msUntilAvailable(), Qt::PreciseTimer, this, [this]() {
        m_WakeScheduled = false;
        if (!m_Failed && m_Produced < m_Total) {
            emit readyRead();
        }
    });
}

qint64 ThrottledUploadDevice::readData(char* data, qint64 maxSize)
{
    if (m_Failed || m_Produced >= m_Total) {
        return -1;
    }
    if (maxSize <= 0) {
        return 0;
    }
    qint64 want = qMin(maxSize, m_Total - m_Produced);
    const qint64 budget = m_Limiter.available();
    if (budget <= 0) {
        scheduleWake();
        return 0;
    }
    want = qMin(want, budget);

    qint64 done = 0;
    while (done < want) {
        const qint64 n = produce(data + done, want - done);
        if (n < 0 || m_Failed) {
            fail(errorString().isEmpty() ? QStringLiteral("read error") : errorString());
            return -1;
        }
        if (n == 0) {
            fail(QStringLiteral("unexpected end of data"));
            return -1;
        }
        done += n;
    }
    m_Produced += done;
    m_Limiter.consume(done);
    return done;
}

BufferUploadDevice::BufferUploadDevice(const QByteArray& data, qint64 bytesPerSecond, QObject* parent)
    : ThrottledUploadDevice(data.size(), bytesPerSecond, parent),
      m_Data(data)
{
}

qint64 BufferUploadDevice::produce(char* data, qint64 maxSize)
{
    const qint64 n = qMin(maxSize, (qint64)m_Data.size() - produced());
    if (n <= 0) {
        return 0;
    }
    memcpy(data, m_Data.constData() + produced(), (size_t)n);
    return n;
}

ArchiveUploadDevice::ArchiveUploadDevice(const QVector<Entry>& entries, qint64 archiveSize, qint64 bytesPerSecond, QObject* parent)
    : ThrottledUploadDevice(archiveSize, bytesPerSecond, parent),
      m_Entries(entries),
      m_NextEntry(0),
      m_HeaderWritten(false),
      m_PendingPos(0),
      m_FileRemaining(0)
{
}

qint64 ArchiveUploadDevice::archiveSize(const QVector<Entry>& entries)
{
    qint64 size = k_HeaderBytes;
    for (const Entry& entry : entries) {
        size += k_EntryFixedBytes + entry.path.toUtf8().size() + (entry.directory ? 0 : (qint64)entry.size);
    }
    return size;
}

bool ArchiveUploadDevice::startNextEntry()
{
    m_Pending.clear();
    m_PendingPos = 0;
    if (!m_HeaderWritten) {
        m_HeaderWritten = true;
        m_Pending = QByteArray("APCF");
        appendU32(m_Pending, 1);
        appendU32(m_Pending, (quint32)m_Entries.size());
        return true;
    }
    if (m_NextEntry >= m_Entries.size()) {
        fail(QStringLiteral("archive longer than planned"));
        return false;
    }

    const Entry& entry = m_Entries.at(m_NextEntry++);
    const QByteArray path = entry.path.toUtf8();
    m_Pending.append(char(entry.directory ? 1 : 0));
    appendU32(m_Pending, (quint32)path.size());
    m_Pending.append(path);
    appendU64(m_Pending, entry.directory ? 0 : entry.size);
    if (!entry.directory) {
        m_File.setFileName(entry.sourcePath);
        if (!m_File.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("cannot read ") + entry.path);
            return false;
        }
        if ((quint64)m_File.size() != entry.size) {
            m_File.close();
            fail(QStringLiteral("file changed while reading: ") + entry.path);
            return false;
        }
        m_FileRemaining = entry.size;
        if (m_FileRemaining == 0) {
            m_File.close();
        }
    }
    return true;
}

qint64 ArchiveUploadDevice::produce(char* data, qint64 maxSize)
{
    if (m_PendingPos < m_Pending.size()) {
        const qint64 n = qMin(maxSize, (qint64)(m_Pending.size() - m_PendingPos));
        memcpy(data, m_Pending.constData() + m_PendingPos, (size_t)n);
        m_PendingPos += (qsizetype)n;
        return n;
    }
    if (m_File.isOpen()) {
        const Entry& entry = m_Entries.at(m_NextEntry - 1);
        const qint64 n = m_File.read(data, (qint64)qMin<quint64>((quint64)maxSize, m_FileRemaining));
        if (n <= 0) {
            m_File.close();
            fail(QStringLiteral("file changed while reading: ") + entry.path);
            return -1;
        }
        m_FileRemaining -= (quint64)n;
        if (m_FileRemaining == 0) {
            // A file that grew meanwhile would no longer match its announced size.
            const bool changed = (quint64)m_File.size() != entry.size;
            m_File.close();
            if (changed) {
                fail(QStringLiteral("file changed while reading: ") + entry.path);
                return -1;
            }
        }
        return n;
    }
    if (!startNextEntry()) {
        return -1;
    }
    return produce(data, maxSize);
}

// ---- Download validation and extraction -----------------------------------------------------

bool validateArchive(QIODevice& archive, QVector<Entry>& entries, QString& error)
{
    entries.clear();
    const qint64 archiveSize = archive.size();
    if (archiveSize > k_MaxArchiveBytes) {
        error = QStringLiteral("archive too large");
        return false;
    }
    if (!archive.seek(0)) {
        error = QStringLiteral("cannot read the archive");
        return false;
    }

    char header[k_HeaderBytes];
    if (!readExact(archive, header, sizeof(header)) || memcmp(header, "APCF", 4) != 0) {
        error = QStringLiteral("not an APCF archive");
        return false;
    }
    const quint32 version = qFromLittleEndian<quint32>(header + 4);
    const quint32 count = qFromLittleEndian<quint32>(header + 8);
    if (version != 1) {
        error = QStringLiteral("unsupported archive version");
        return false;
    }
    if (count == 0 || count > (quint32)k_MaxFileEntries) {
        error = QStringLiteral("entry count out of range");
        return false;
    }

    qint64 pos = k_HeaderBytes;
    QSet<QString> seen;
    QSet<QString> files;
    quint64 total = 0;
    entries.reserve((int)count);
    for (quint32 i = 0; i < count; i++) {
        Entry entry;
        char fixed[5];
        if (!readExact(archive, fixed, sizeof(fixed))) {
            error = QStringLiteral("malformed entry");
            return false;
        }
        const quint8 kind = (quint8)fixed[0];
        const quint32 pathLength = qFromLittleEndian<quint32>(fixed + 1);
        pos += 5;
        if (kind > 1 || pathLength > 1024 || (quint64)(archiveSize - pos) < pathLength) {
            error = QStringLiteral("malformed entry");
            return false;
        }
        QByteArray rawPath((qsizetype)pathLength, '\0');
        if (!readExact(archive, rawPath.data(), pathLength)) {
            error = QStringLiteral("malformed entry");
            return false;
        }
        pos += pathLength;
        entry.path = QString::fromUtf8(rawPath);
        entry.directory = kind == 1;
        if (entry.path.toUtf8() != rawPath || !isSafeRelativePath(entry.path)) {
            error = QStringLiteral("unsafe path");
            return false;
        }
        char sizeBytes[8];
        if (!readExact(archive, sizeBytes, sizeof(sizeBytes))) {
            error = QStringLiteral("malformed entry");
            return false;
        }
        pos += 8;
        entry.size = qFromLittleEndian<quint64>(sizeBytes);
        if (entry.directory && entry.size != 0) {
            error = QStringLiteral("malformed entry");
            return false;
        }
        total += entry.size;
        if (entry.size > k_MaxFilesBytes || total > k_MaxFilesBytes) {
            error = QStringLiteral("files too large");
            return false;
        }
        if ((quint64)(archiveSize - pos) < entry.size) {
            error = QStringLiteral("truncated data");
            return false;
        }
        entry.dataOffset = pos;
        pos += (qint64)entry.size;
        if (entry.size != 0 && !archive.seek(pos)) {
            error = QStringLiteral("truncated data");
            return false;
        }

        const QString key = foldPath(entry.path);
        if (seen.contains(key)) {
            error = QStringLiteral("duplicate path");
            return false;
        }
        seen.insert(key);
        if (!entry.directory) {
            files.insert(key);
        }
        entries.append(std::move(entry));
    }
    if (pos != archiveSize) {
        error = QStringLiteral("trailing data");
        return false;
    }
    // A file must not also be used as a parent directory of another entry.
    for (const Entry& entry : entries) {
        const QString key = foldPath(entry.path);
        for (int slash = key.indexOf(QLatin1Char('/')); slash >= 0; slash = key.indexOf(QLatin1Char('/'), slash + 1)) {
            if (files.contains(key.left(slash))) {
                error = QStringLiteral("file used as directory");
                return false;
            }
        }
    }
    return true;
}

bool extractArchive(QIODevice& archive, const QVector<Entry>& entries, const QString& rootPath,
                    QStringList& topLevel, QString& error, const std::function<bool()>& cancelled)
{
    topLevel.clear();
    if (!QDir().mkpath(rootPath)) {
        error = QStringLiteral("cannot create ") + rootPath;
        return false;
    }
    QDir root(rootPath);
    QStringList previous = root.entryList({QStringLiteral("xfer-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    while (previous.size() > 4) {
        QDir(root.filePath(previous.takeFirst())).removeRecursively();
    }

    QString name = QStringLiteral("xfer-") + QString::number(QDateTime::currentMSecsSinceEpoch());
    if (!root.mkdir(name)) {
        name += QStringLiteral("-1");
        if (!root.mkdir(name)) {
            error = QStringLiteral("cannot create a transfer folder");
            return false;
        }
    }
    const QString folder = root.filePath(name);
    auto fail = [&](const QString& message) {
        error = message;
        QDir(folder).removeRecursively();
        topLevel.clear();
        return false;
    };

    QByteArray buffer;
    QSet<QString> tops;
    for (const Entry& entry : entries) {
        if (cancelled && cancelled()) {
            return fail(QStringLiteral("cancelled"));
        }
        const QString target = folder + QLatin1Char('/') + entry.path;
        if (entry.directory) {
            if (!QDir().mkpath(target)) {
                return fail(QStringLiteral("cannot create folder ") + entry.path);
            }
        }
        else {
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                return fail(QStringLiteral("cannot create folder for ") + entry.path);
            }
            QFile file(target);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !archive.seek(entry.dataOffset)) {
                return fail(QStringLiteral("cannot write ") + entry.path);
            }
            quint64 remaining = entry.size;
            while (remaining > 0) {
                if (cancelled && cancelled()) {
                    file.close();
                    return fail(QStringLiteral("cancelled"));
                }
                const qint64 chunk = (qint64)qMin<quint64>(remaining, (quint64)k_CopyChunkBytes);
                buffer.resize((qsizetype)chunk);
                if (!readExact(archive, buffer.data(), chunk) || file.write(buffer.constData(), chunk) != chunk) {
                    file.close();
                    return fail(QStringLiteral("cannot write ") + entry.path);
                }
                remaining -= (quint64)chunk;
            }
            if (!file.flush()) {
                file.close();
                return fail(QStringLiteral("cannot write ") + entry.path);
            }
            file.close();
        }
        const QString top = entry.path.section(QLatin1Char('/'), 0, 0);
        const QString topKey = foldPath(top);
        if (!tops.contains(topKey)) {
            tops.insert(topKey);
            topLevel.append(QDir::toNativeSeparators(folder + QLatin1Char('/') + top));
        }
    }
    return true;
}

int RemoteFileList::topLevelCount() const
{
    int count = 0;
    for (const RemoteFile& entry : entries) {
        if (!entry.path.contains(QLatin1Char('/'))) {
            count++;
        }
    }
    return count;
}

bool parseFileList(const QByteArray& body, RemoteFileList& list, QString& error)
{
    list = RemoteFileList();
    bool haveSeq = false, haveSnapshot = false, haveCount = false, haveBytes = false;
    quint64 declaredCount = 0, declaredBytes = 0;
    QSet<QString> seen;
    QSet<QString> directories;

    const QList<QByteArray> lines = body.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        if (line.size() >= 2 && (line[0] == 'f' || line[0] == 'd') && line[1] == '\t') {
            // kind, size, time, path; the path is the rest of the line
            const QList<QByteArray> fields = line.split('\t');
            if (fields.size() != 4) {
                error = QStringLiteral("malformed entry");
                return false;
            }
            RemoteFile entry;
            bool sizeOk = false, timeOk = false;
            entry.directory = line[0] == 'd';
            entry.size = fields[1].toULongLong(&sizeOk);
            entry.modifiedMs = fields[2].toLongLong(&timeOk);
            entry.path = QString::fromUtf8(fields[3]);
            if (!sizeOk || !timeOk || entry.modifiedMs < 0 || (entry.directory && entry.size != 0)) {
                error = QStringLiteral("malformed entry");
                return false;
            }
            if (entry.path.toUtf8() != fields[3] || !isSafeRelativePath(entry.path)) {
                error = QStringLiteral("unsafe path");
                return false;
            }
            if (list.entries.size() >= k_MaxFileEntries) {
                error = QStringLiteral("too many files");
                return false;
            }
            if (entry.size > k_MaxStreamFilesBytes || list.totalBytes + entry.size > k_MaxStreamFilesBytes) {
                error = QStringLiteral("files too large");
                return false;
            }
            const QString key = foldPath(entry.path);
            if (seen.contains(key)) {
                error = QStringLiteral("duplicate path");
                return false;
            }
            // Paste creates folders in list order, so each folder must come before its contents.
            const int slash = key.lastIndexOf(QLatin1Char('/'));
            if (slash >= 0 && !directories.contains(key.left(slash))) {
                error = QStringLiteral("item listed before its folder");
                return false;
            }
            seen.insert(key);
            if (entry.directory) {
                directories.insert(key);
            }
            list.totalBytes += entry.size;
            list.entries.append(entry);
            continue;
        }

        const int eq = line.indexOf('=');
        if (eq <= 0) {
            error = QStringLiteral("malformed line");
            return false;
        }
        const QByteArray key = line.left(eq);
        const QByteArray value = line.mid(eq + 1);
        bool ok = true;
        if (key == "seq") {
            list.seq = value.toUInt(&ok);
            haveSeq = ok;
        }
        else if (key == "snapshot") {
            // Goes into request URLs: short and alphanumeric only.
            ok = !value.isEmpty() && value.size() <= 64;
            for (char c : value) {
                ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
            }
            list.snapshot = value;
            haveSnapshot = ok;
        }
        else if (key == "entries") {
            declaredCount = value.toULongLong(&ok);
            haveCount = ok;
        }
        else if (key == "bytes") {
            declaredBytes = value.toULongLong(&ok);
            haveBytes = ok;
        }
        if (!ok) {
            error = QStringLiteral("malformed field ") + QString::fromLatin1(key);
            return false;
        }
    }

    if (!haveSeq || !haveSnapshot || !haveCount || !haveBytes) {
        error = QStringLiteral("missing field");
        return false;
    }
    if (list.entries.isEmpty()) {
        error = QStringLiteral("empty list");
        return false;
    }
    if (declaredCount != (quint64)list.entries.size() || declaredBytes != list.totalBytes) {
        error = QStringLiteral("incomplete list");
        return false;
    }
    return true;
}

}  // namespace ClipboardArchive
