#include "netdiag.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>

std::atomic<bool> NetDiagLog::s_On {false};
NetDiagWindow NetDiagLog::s_Window;
std::atomic<int> NetDiagLog::s_RequestedKbps {0};
std::atomic<int> NetDiagLog::s_AppliedKbps {0};
std::atomic<int> NetDiagLog::s_AutoKbps {0};
std::atomic<bool> NetDiagLog::s_Vsync {false};
std::atomic<bool> NetDiagLog::s_FramePacing {false};
QMutex NetDiagLog::s_NoteLock;
QString NetDiagLog::s_Note;

NetDiagLog::NetDiagLog()
    : m_File(nullptr),
      m_Thread(nullptr),
      m_Stopping(false),
      m_StartUs(0),
      m_TotalReceived(0),
      m_TotalNetworkDropped(0),
      m_TotalPacerDropped(0),
      m_Summarised(false)
{
}

NetDiagLog::~NetDiagLog()
{
    stop();
    delete m_File;
}

QString NetDiagLog::fileName() const
{
    return QFileInfo(m_FilePath).fileName();
}

QString NetDiagLog::folder()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
}

void NetDiagLog::pruneOldFiles(const QString& folder, int keep)
{
    QDir dir(folder);
    // Newest first, like the log files in main.cpp
    const QStringList names = dir.entryList(QStringList(filePattern()), QDir::Files, QDir::Time);
    for (int i = (std::max)(keep, 0); i < names.size(); i++) {
        QFile::remove(dir.filePath(names.at(i)));
    }
}

bool NetDiagLog::start()
{
    if (m_Thread != nullptr) {
        return true;
    }

    const QString dirPath = folder();
    QDir().mkpath(dirPath);
    pruneOldFiles(dirPath, kKeepFiles - 1);

    const qint64 unixTime = QDateTime::currentSecsSinceEpoch();
    QString path = QDir(dirPath).filePath(QStringLiteral("Hermit-netdiag-%1.csv").arg(unixTime));
    for (int n = 2; QFile::exists(path) && n < 100; n++) {
        path = QDir(dirPath).filePath(QStringLiteral("Hermit-netdiag-%1-%2.csv").arg(unixTime).arg(n));
    }

    m_File = new QFile(path);
    if (!m_File->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Network diagnostics: cannot create %s",
                    qPrintable(path));
        delete m_File;
        m_File = nullptr;
        return false;
    }
    m_FilePath = path;
    m_File->write(NetDiagRow::header());
    m_File->flush();

    m_StartUs = LiGetMicroseconds();
    s_Window.reset(m_StartUs);
    {
        QMutexLocker lock(&s_NoteLock);
        s_Note = QStringLiteral("start");
    }
    m_Stopping = false;
    s_On.store(true, std::memory_order_release);

    m_Thread = QThread::create([this]() { threadProc(); });
    m_Thread->setObjectName("Network diagnostics");
    m_Thread->start();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Network diagnostics: recording to %s",
                qPrintable(path));
    return true;
}

void NetDiagLog::stop()
{
    // The frame path stops feeding the window first; a hook that is already past the flag only
    // touches the static window, never this object.
    s_On.store(false, std::memory_order_release);

    if (m_Thread != nullptr) {
        {
            QMutexLocker lock(&m_StopLock);
            m_Stopping = true;
            m_StopSignal.wakeAll();
        }
        m_Thread->wait();
        delete m_Thread;
        m_Thread = nullptr;
    }

    if (m_File != nullptr && m_File->isOpen()) {
        m_File->close();
    }

    if (!m_Summarised && !m_FilePath.isEmpty()) {
        m_Summarised = true;
        const uint32_t total = m_TotalReceived + m_TotalNetworkDropped;
        const double lossPct = total != 0 ? static_cast<double>(m_TotalNetworkDropped) * 100.0 / total : 0.0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Network diagnostics: %s, %d rows, max arrival gap %d ms, pacer dropped %u, network dropped %.2f%%",
                    qPrintable(m_FilePath), rows(), maxArrivalGapMs(), m_TotalPacerDropped, lossPct);
    }
}

void NetDiagLog::setBitrates(int requestedKbps, int appliedKbps, int autoKbps)
{
    s_RequestedKbps.store(requestedKbps, std::memory_order_relaxed);
    s_AppliedKbps.store(appliedKbps, std::memory_order_relaxed);
    s_AutoKbps.store(autoKbps, std::memory_order_relaxed);
}

void NetDiagLog::setRenderMode(bool vsync, bool framePacing)
{
    s_Vsync.store(vsync, std::memory_order_relaxed);
    s_FramePacing.store(framePacing, std::memory_order_relaxed);
}

void NetDiagLog::note(const QString& text)
{
    if (!isOn()) {
        return;
    }
    QMutexLocker lock(&s_NoteLock);
    if (!s_Note.isEmpty()) {
        s_Note += QStringLiteral("; ");
    }
    s_Note += text;
}

static void appendEvent(QString& event, const QString& text)
{
    if (!event.isEmpty()) {
        event += QStringLiteral("; ");
    }
    event += text;
}

bool NetDiagLog::writeRow(const NetDiagRow& row)
{
    if (m_File->write(row.toCsv()) < 0) {
        return false;
    }
    const int count = m_Rows.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % 10 == 0) {
        m_File->flush();  // once a second: a crash keeps all but the last second
    }
    return true;
}

void NetDiagLog::threadProc()
{
    const uint64_t intervalUs = static_cast<uint64_t>(kIntervalMs) * 1000;
    uint64_t next = m_StartUs + intervalUs;
    int lastRequested = -1, lastApplied = -1, lastAuto = -1;
    bool lastVsync = false, lastPacing = false, first = true;

    for (;;) {
        {
            // Sleep until the next 100 ms mark, or until stop() wakes us
            QMutexLocker lock(&m_StopLock);
            while (!m_Stopping) {
                const uint64_t now = LiGetMicroseconds();
                if (now >= next) {
                    break;
                }
                m_StopSignal.wait(&m_StopLock, static_cast<unsigned long>((next - now + 999) / 1000));
            }
            if (m_Stopping) {
                break;
            }
        }

        const uint64_t now = LiGetMicroseconds();
        NetDiagRow row;
        row.tMs = static_cast<int64_t>((now - m_StartUs) / 1000);
        s_Window.sample(now, row);

        uint32_t rtt = 0, rttVariance = 0;
        if (LiGetEstimatedRttInfo(&rtt, &rttVariance)) {
            row.rttMs = static_cast<int>(rtt);
            row.rttVarianceMs = static_cast<int>(rttVariance);
        }
        row.requestedKbps = s_RequestedKbps.load(std::memory_order_relaxed);
        row.appliedKbps = s_AppliedKbps.load(std::memory_order_relaxed);
        row.autoKbps = s_AutoKbps.load(std::memory_order_relaxed);
        row.vsync = s_Vsync.load(std::memory_order_relaxed);
        row.framePacing = s_FramePacing.load(std::memory_order_relaxed);

        {
            QMutexLocker lock(&s_NoteLock);
            row.event.swap(s_Note);
        }
        if (!first) {
            // Changes of the state columns are events of their own, so a plot can mark them
            if (row.requestedKbps != lastRequested) {
                appendEvent(row.event, QStringLiteral("requested %1->%2").arg(lastRequested).arg(row.requestedKbps));
            }
            if (row.appliedKbps != lastApplied) {
                appendEvent(row.event, QStringLiteral("applied %1->%2").arg(lastApplied).arg(row.appliedKbps));
            }
            if (row.autoKbps != lastAuto) {
                appendEvent(row.event, QStringLiteral("auto %1->%2").arg(lastAuto).arg(row.autoKbps));
            }
            if (row.vsync != lastVsync || row.framePacing != lastPacing) {
                appendEvent(row.event, QStringLiteral("vsync %1 pacing %2").arg(row.vsync ? 1 : 0).arg(row.framePacing ? 1 : 0));
            }
        }
        first = false;
        lastRequested = row.requestedKbps;
        lastApplied = row.appliedKbps;
        lastAuto = row.autoKbps;
        lastVsync = row.vsync;
        lastPacing = row.framePacing;

        m_TotalReceived += row.framesReceived;
        m_TotalNetworkDropped += row.framesNetworkDropped;
        m_TotalPacerDropped += row.pacerDropped;
        if (row.lastFrameArrivalGapMs > m_MaxGapMs.load(std::memory_order_relaxed)) {
            m_MaxGapMs.store(row.lastFrameArrivalGapMs, std::memory_order_relaxed);
        }

        if (!writeRow(row)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Network diagnostics: write failed, recording stopped (%s)",
                        qPrintable(m_File->errorString()));
            s_On.store(false, std::memory_order_release);
            break;
        }

        // The next mark; after a long stall (the thread starved), skip the missed marks
        next += intervalUs;
        while (next <= now) {
            next += intervalUs;
        }
    }

    m_File->flush();
}
