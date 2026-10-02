#include "sessionsummary.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QtDebug>

namespace {

const QStringList kColumns = {
    "ended_at", "host", "app", "duration_min", "resolution", "codec", "target_fps",
    "bitrate_mbps", "vsync", "frame_pacing", "windowed", "avg_fps", "network_drop_pct",
    "jitter_drop_pct", "host_latency_avg_ms", "host_latency_max_ms", "rtt_ms",
    "rtt_variance_ms", "decode_ms", "queue_ms", "render_ms",
};

QString csvField(const QString& value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) ||
            value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        QString escaped = value;
        escaped.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return value;
}

QStringList parseCsvLine(const QString& line)
{
    QStringList fields;
    QString current;
    bool quoted = false;
    for (int i = 0; i < line.size(); i++) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    current += c;
                    i++;
                }
                else {
                    quoted = false;
                }
            }
            else {
                current += c;
            }
        }
        else if (c == QLatin1Char('"')) {
            quoted = true;
        }
        else if (c == QLatin1Char(',')) {
            fields.append(current);
            current.clear();
        }
        else {
            current += c;
        }
    }
    fields.append(current);
    return fields;
}

QString number(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

}

void SessionSummary::start(const Settings& settings)
{
    QMutexLocker locker(&m_Lock);
    m_Settings = settings;
    m_Started = true;
    m_Timer.start();
    m_BitrateKbpsMs = 0;
    m_BitrateSinceMs = 0;
    m_CurrentKbps = settings.bitrateKbps;
    m_BitrateChanged = false;
    m_AutoBitrate = false;
    m_ChosenKbps = settings.bitrateKbps;
}

void SessionSummary::setBitrate(int kbps)
{
    QMutexLocker locker(&m_Lock);
    if (!m_Started || kbps == m_CurrentKbps) {
        return;
    }
    const qint64 now = m_Timer.elapsed();
    m_BitrateKbpsMs += double(m_CurrentKbps) * (now - m_BitrateSinceMs);
    m_BitrateSinceMs = now;
    m_CurrentKbps = kbps;
    m_BitrateChanged = true;
}

void SessionSummary::noteAutoBitrate()
{
    QMutexLocker locker(&m_Lock);
    m_AutoBitrate = true;
}

void SessionSummary::setEndSettings(int chosenKbps, bool vsync, bool framePacing)
{
    QMutexLocker locker(&m_Lock);
    if (chosenKbps > 0) {
        m_ChosenKbps = chosenKbps;
    }
    m_Settings.vsync = vsync;
    m_Settings.framePacing = framePacing;
}

void SessionSummary::addDecoderStats(const VIDEO_STATS& stats, int width, int height, int videoFormat, uint64_t endUs)
{
    const QString codec = codecName(videoFormat);

    QMutexLocker locker(&m_Lock);
    m_TotalFrames += stats.totalFrames;
    m_ReceivedFrames += stats.receivedFrames;
    m_DecodedFrames += stats.decodedFrames;
    m_RenderedFrames += stats.renderedFrames;
    m_NetworkDroppedFrames += stats.networkDroppedFrames;
    m_PacerDroppedFrames += stats.pacerDroppedFrames;
    m_HostLatencyTotal += stats.totalHostProcessingLatency;
    m_HostLatencyFrames += stats.framesWithHostProcessingLatency;
    if (stats.minHostProcessingLatency != 0 &&
            (m_HostLatencyMin == 0 || stats.minHostProcessingLatency < m_HostLatencyMin)) {
        m_HostLatencyMin = stats.minHostProcessingLatency;
    }
    if (stats.maxHostProcessingLatency > m_HostLatencyMax) {
        m_HostLatencyMax = stats.maxHostProcessingLatency;
    }
    m_DecodeUs += stats.totalDecodeTimeUs;
    m_PacerUs += stats.totalPacerTimeUs;
    m_RenderUs += stats.totalRenderTimeUs;
    if (stats.measurementStartUs != 0 && endUs > stats.measurementStartUs) {
        m_MeasuredUs += endUs - stats.measurementStartUs;
    }
    if (stats.lastRtt != 0) {
        m_LastRtt = stats.lastRtt;
        m_LastRttVariance = stats.lastRttVariance;
    }
    if (width > 0 && height > 0) {
        m_VideoWidth = width;
        m_VideoHeight = height;
    }
    if (!codec.isEmpty()) {
        m_Codec = codec;
    }
}

QVariantMap SessionSummary::finish()
{
    QMutexLocker locker(&m_Lock);
    if (!m_Started) {
        return {};
    }
    m_Started = false;

    const qint64 elapsedMs = m_Timer.elapsed();
    if (m_RenderedFrames == 0 || elapsedMs < m_MinimumMs) {
        return {};
    }

    const double measuredSecs = m_MeasuredUs > 0 ? m_MeasuredUs / 1000000.0 : elapsedMs / 1000.0;
    const double avgFps = m_RenderedFrames / measuredSecs;
    const double networkDropPct = m_TotalFrames ? 100.0 * m_NetworkDroppedFrames / m_TotalFrames : 0.0;
    const double jitterDropPct = m_DecodedFrames ? 100.0 * m_PacerDroppedFrames / m_DecodedFrames : 0.0;
    const double hostAvgMs = m_HostLatencyFrames ? m_HostLatencyTotal / 10.0 / m_HostLatencyFrames : 0.0;
    const double hostMaxMs = m_HostLatencyMax / 10.0;
    const double decodeMs = m_DecodedFrames ? m_DecodeUs / 1000.0 / m_DecodedFrames : 0.0;
    const double queueMs = m_RenderedFrames ? m_PacerUs / 1000.0 / m_RenderedFrames : 0.0;
    const double renderMs = m_RenderedFrames ? m_RenderUs / 1000.0 / m_RenderedFrames : 0.0;
    const int videoWidth = m_VideoWidth ? m_VideoWidth : m_Settings.width;
    const int videoHeight = m_VideoHeight ? m_VideoHeight : m_Settings.height;
    const QDateTime endedAt = QDateTime::currentDateTime();
    // Time-weighted when the bitrate changed during the stream (also in the history file)
    const double bitrateKbps = m_BitrateChanged && elapsedMs > 0 ?
            (m_BitrateKbpsMs + double(m_CurrentKbps) * (elapsedMs - m_BitrateSinceMs)) / elapsedMs :
            m_Settings.bitrateKbps;

    QVariantMap result;
    result["endedAt"] = endedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    result["host"] = m_Settings.host;
    result["app"] = m_Settings.app;
    result["durationMin"] = elapsedMs / 60000.0;
    result["resolution"] = QStringLiteral("%1x%2").arg(videoWidth).arg(videoHeight);
    result["codec"] = m_Codec;
    result["targetFps"] = m_Settings.fps;
    result["bitrateMbps"] = bitrateKbps / 1000.0;
    result["bitrateChanged"] = m_BitrateChanged;
    result["bitrateLastMbps"] = m_CurrentKbps / 1000.0;
    result["autoBitrate"] = m_AutoBitrate;
    result["bitrateChosenMbps"] = m_ChosenKbps / 1000.0;
    result["vsync"] = m_Settings.vsync;
    result["framePacing"] = m_Settings.framePacing;
    result["windowed"] = m_Settings.windowed;
    result["avgFps"] = avgFps;
    result["networkDropPct"] = networkDropPct;
    result["jitterDropPct"] = jitterDropPct;
    result["hostLatencyAvgMs"] = hostAvgMs;
    result["hostLatencyMaxMs"] = hostMaxMs;
    result["rttMs"] = m_LastRtt;
    result["rttVarianceMs"] = m_LastRttVariance;
    result["decodeMs"] = decodeMs;
    result["queueMs"] = queueMs;
    result["renderMs"] = renderMs;

    const QStringList row = {
        endedAt.toString(Qt::ISODate), m_Settings.host, m_Settings.app, number(elapsedMs / 60000.0, 1),
        result["resolution"].toString(), m_Codec, QString::number(m_Settings.fps),
        number(bitrateKbps / 1000.0, 1), m_Settings.vsync ? "1" : "0",
        m_Settings.framePacing ? "1" : "0", m_Settings.windowed ? "1" : "0", number(avgFps, 2),
        number(networkDropPct, 3), number(jitterDropPct, 3), number(hostAvgMs, 2), number(hostMaxMs, 1),
        QString::number(m_LastRtt), QString::number(m_LastRttVariance), number(decodeMs, 2),
        number(queueMs, 2), number(renderMs, 2),
    };

    const QString path = historyFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    const bool isNew = !file.exists() || file.size() == 0;
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);
        if (isNew) {
            // A byte order mark lets Excel open the Korean host and app names correctly.
            out << QChar(0xFEFF) << kColumns.join(QLatin1Char(',')) << '\n';
        }
        QStringList fields;
        for (const QString& value : row) {
            fields.append(csvField(value));
        }
        out << fields.join(QLatin1Char(',')) << '\n';
        result["historySaved"] = true;
    }
    else {
        qWarning() << "Could not write session history to" << path;
        result["historySaved"] = false;
    }
    result["historyPath"] = path;

    return result;
}

QString SessionSummary::historyFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("session-history.csv"));
}

QString SessionHistory::path() const
{
    return SessionSummary::historyFilePath();
}

QUrl SessionHistory::folderUrl() const
{
    const QString folder = QFileInfo(SessionSummary::historyFilePath()).absolutePath();
    QDir().mkpath(folder);
    return QUrl::fromLocalFile(folder);
}

QVariantMap SessionSummary::recentAverages(int count, int skip)
{
    QFile file(historyFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    QStringList lines;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (!line.trimmed().isEmpty()) {
            lines.append(line);
        }
    }
    if (lines.size() < 2) {
        return {};
    }

    QString headerLine = lines.takeFirst();
    if (headerLine.startsWith(QChar(0xFEFF))) {
        headerLine.remove(0, 1);
    }
    const QStringList header = parseCsvLine(headerLine);

    const int end = int(lines.size()) - skip;
    const int begin = qMax(0, end - count);
    if (end <= begin) {
        return {};
    }

    const QStringList keys = { "avg_fps", "network_drop_pct", "jitter_drop_pct",
                               "host_latency_avg_ms", "rtt_ms", "queue_ms", "duration_min" };
    const QStringList names = { "avgFps", "networkDropPct", "jitterDropPct",
                                "hostLatencyAvgMs", "rttMs", "queueMs", "durationMin" };
    QVector<double> sums(keys.size(), 0.0);
    QVector<int> counts(keys.size(), 0);
    for (int i = begin; i < end; i++) {
        const QStringList fields = parseCsvLine(lines.at(i));
        for (int k = 0; k < keys.size(); k++) {
            const int column = int(header.indexOf(keys.at(k)));
            bool ok = false;
            const double value = column >= 0 && column < fields.size() ? fields.at(column).toDouble(&ok) : 0.0;
            if (ok) {
                sums[k] += value;
                counts[k]++;
            }
        }
    }

    QVariantMap result;
    result["sessions"] = end - begin;
    for (int k = 0; k < keys.size(); k++) {
        if (counts[k] > 0) {
            result[names.at(k)] = sums[k] / counts[k];
        }
    }
    return result;
}

QString SessionSummary::codecName(int videoFormat)
{
    const bool hdr = LiGetCurrentHostDisplayHdrMode();
    switch (videoFormat) {
    case VIDEO_FORMAT_H264:
        return QStringLiteral("H.264");
    case VIDEO_FORMAT_H264_HIGH8_444:
        return QStringLiteral("H.264 4:4:4");
    case VIDEO_FORMAT_H265:
        return QStringLiteral("HEVC");
    case VIDEO_FORMAT_H265_REXT8_444:
        return QStringLiteral("HEVC 4:4:4");
    case VIDEO_FORMAT_H265_MAIN10:
        return hdr ? QStringLiteral("HEVC 10-bit HDR") : QStringLiteral("HEVC 10-bit");
    case VIDEO_FORMAT_H265_REXT10_444:
        return hdr ? QStringLiteral("HEVC 10-bit HDR 4:4:4") : QStringLiteral("HEVC 10-bit 4:4:4");
    case VIDEO_FORMAT_AV1_MAIN8:
        return QStringLiteral("AV1");
    case VIDEO_FORMAT_AV1_HIGH8_444:
        return QStringLiteral("AV1 4:4:4");
    case VIDEO_FORMAT_AV1_MAIN10:
        return hdr ? QStringLiteral("AV1 10-bit HDR") : QStringLiteral("AV1 10-bit");
    case VIDEO_FORMAT_AV1_HIGH10_444:
        return hdr ? QStringLiteral("AV1 10-bit HDR 4:4:4") : QStringLiteral("AV1 10-bit 4:4:4");
    default:
        return {};
    }
}
