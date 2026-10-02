#pragma once

#include "video/decoder.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QUrl>
#include <QString>
#include <QVariantMap>

// Collects the video statistics of one streaming session across decoder lifetimes (a decoder
// is recreated at start and on every window resize), so the end-of-session summary and the
// history file describe the whole session rather than the last decoder.
class SessionSummary
{
public:
    struct Settings {
        QString host;
        QString app;
        int width = 0;
        int height = 0;
        int fps = 0;
        int bitrateKbps = 0;
        bool vsync = false;
        bool framePacing = false;
        bool windowed = false;
    };

    // Starts timing when the connection is established.
    void start(const Settings& settings);

    // Sessions shorter than this are not summarized (default 30 s). Only tests change it.
    void setMinimumDurationMs(qint64 ms)
    {
        m_MinimumMs = ms;
    }

    // The host now streams at another bitrate (changed live, by the user or automatic bitrate).
    // The summary then shows the time-weighted average. Thread-safe.
    void setBitrate(int kbps);

    // Automatic bitrate adjusted this session.
    void noteAutoBitrate();

    // At the end of the stream: the chosen bitrate (automatic bitrate's ceiling), and V-Sync and
    // frame pacing as last applied (the stream panel changes them live). Thread-safe.
    void setEndSettings(int chosenKbps, bool vsync, bool framePacing);

    // Adds the statistics of a decoder that is being destroyed; endUs is when it stopped
    // (LiGetMicroseconds). Thread-safe.
    void addDecoderStats(const VIDEO_STATS& stats, int width, int height, int videoFormat, uint64_t endUs);

    // Computes the summary, appends it to the history file and returns it for the UI.
    // Returns an empty map if the session never started or rendered no frames.
    QVariantMap finish();

    // CSV file with one row per session, in the app data folder.
    static QString historyFilePath();

    // Averages of the most recent sessions in the history file, skipping the newest `skip`
    // rows (the session that was just added). Empty when there is no earlier session.
    static QVariantMap recentAverages(int count, int skip);

    // Display name of a VIDEO_FORMAT_* value (also used by the performance overlay).
    static QString codecName(int videoFormat);

private:

    QMutex m_Lock;
    qint64 m_MinimumMs = 30 * 1000;
    bool m_Started = false;
    Settings m_Settings;
    QElapsedTimer m_Timer;

    // Live bitrate: kbps x ms at earlier bitrates, and since when the current one is in effect
    double m_BitrateKbpsMs = 0;
    qint64 m_BitrateSinceMs = 0;
    int m_CurrentKbps = 0;
    bool m_BitrateChanged = false;
    bool m_AutoBitrate = false;
    int m_ChosenKbps = 0;

    // Totals across decoders
    quint64 m_TotalFrames = 0;
    quint64 m_ReceivedFrames = 0;
    quint64 m_DecodedFrames = 0;
    quint64 m_RenderedFrames = 0;
    quint64 m_NetworkDroppedFrames = 0;
    quint64 m_PacerDroppedFrames = 0;
    quint64 m_HostLatencyTotal = 0;     // 0.1 ms units
    quint64 m_HostLatencyFrames = 0;
    quint32 m_HostLatencyMin = 0;       // 0.1 ms units
    quint32 m_HostLatencyMax = 0;
    quint64 m_DecodeUs = 0;
    quint64 m_PacerUs = 0;
    quint64 m_RenderUs = 0;
    quint64 m_MeasuredUs = 0;           // time covered by decoder statistics
    quint32 m_LastRtt = 0;
    quint32 m_LastRttVariance = 0;
    int m_VideoWidth = 0;
    int m_VideoHeight = 0;
    QString m_Codec;
};

// QML access to the history file (SessionHistory singleton).
class SessionHistory : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString path READ path CONSTANT)
    // Folder containing the history file, created if needed, for opening in the file manager.
    Q_PROPERTY(QUrl folderUrl READ folderUrl CONSTANT)

public:
    using QObject::QObject;

    QString path() const;
    QUrl folderUrl() const;
};
