#include "statsoverlay.h"

#include <QCoreApplication>

namespace {

QString ms(double value)
{
    return QStringLiteral("%1 ms").arg(value, 0, 'f', 1);
}

QString pct(double value)
{
    return QStringLiteral("%1%").arg(value, 0, 'f', 2);
}

}

namespace StatsOverlay {

QString format(const Inputs& in, int metrics)
{
    if (in.stats == nullptr) {
        return {};
    }
    const VIDEO_STATS& s = *in.stats;

    const bool haveFrames = s.renderedFrames != 0;
    const double hostAvg = s.framesWithHostProcessingLatency != 0
            ? (double)s.totalHostProcessingLatency / 10.0 / s.framesWithHostProcessingLatency : 0.0;
    const double decodeMs = s.decodedFrames != 0 ? (double)s.totalDecodeTimeUs / 1000.0 / s.decodedFrames : 0.0;
    const double queueMs = s.renderedFrames != 0 ? (double)s.totalPacerTimeUs / 1000.0 / s.renderedFrames : 0.0;
    const double renderMs = s.renderedFrames != 0 ? (double)s.totalRenderTimeUs / 1000.0 / s.renderedFrames : 0.0;

    QStringList lines;
    auto add = [&](int metric, const QString& label, const QString& value) {
        if (metrics & metric) {
            lines.append(label + QLatin1Char('\t') + value);
        }
    };

    if (in.width > 0 && in.height > 0) {
        add(MetricVideo, QCoreApplication::translate("StatsOverlay", "Video"),
            QStringLiteral("%1×%2 · %3 FPS%4").arg(in.width).arg(in.height).arg(s.totalFps, 0, 'f', 1)
                .arg(in.codec.isEmpty() ? QString() : QStringLiteral(" · ") + in.codec));
    }
    if (in.cursorState != CursorNone) {
        // Who draws the mouse cursor on a host with local cursor support
        add(MetricCursor, QCoreApplication::translate("StatsOverlay", "Cursor"),
            in.cursorState == CursorLocal ? QCoreApplication::translate("StatsOverlay", "local") :
            in.cursorState == CursorHostOtherClient ? QCoreApplication::translate("StatsOverlay", "host (another client connected)") :
                                                      QCoreApplication::translate("StatsOverlay", "host"));
    }
    if (s.receivedFps > 0) {
        add(MetricFrameRates, QCoreApplication::translate("StatsOverlay", "Received / decoded / rendered"),
            QStringLiteral("%1 / %2 / %3").arg(s.receivedFps, 0, 'f', 1).arg(s.decodedFps, 0, 'f', 1).arg(s.renderedFps, 0, 'f', 1));
    }
    if (in.avgMbps > 0) {
        add(MetricBitrate, QCoreApplication::translate("StatsOverlay", "Bitrate"),
            QCoreApplication::translate("StatsOverlay", "%1 Mbps (peak %2)").arg(in.avgMbps, 0, 'f', 1).arg(in.peakMbps, 0, 'f', 1));
    }
    if (in.adaptBitrateKbps > 0) {
        // Network adaptation: where the controller holds the bitrate, its FEC step and, while the
        // link is not clean, which trouble it sees
        QString value = QStringLiteral("%1 Mbps").arg(in.adaptBitrateKbps / 1000.0, 0, 'f', 1);
        if (in.adaptFecPercent > 0) {
            value += QStringLiteral(" · FEC %1%").arg(in.adaptFecPercent);
        }
        else if (in.adaptFecPercent < 0) {
            value += QStringLiteral(" · FEC ?");  // the host has not told its value yet
        }
        if (in.adaptRegime == AdaptCongested) {
            value += QStringLiteral(" · ") + QCoreApplication::translate("StatsOverlay", "congested");
        }
        else if (in.adaptRegime == AdaptLoss) {
            value += QStringLiteral(" · ") + QCoreApplication::translate("StatsOverlay", "loss");
        }
        add(MetricBitrate, QCoreApplication::translate("StatsOverlay", "Automatic"), value);
    }
    if (in.playback != PlaybackNone) {
        // The playback policy: the newest frame at once, or a jitter buffer
        add(MetricBitrate, QCoreApplication::translate("StatsOverlay", "Playback"),
            in.playback == PlaybackNewestFrame ? QCoreApplication::translate("StatsOverlay", "newest frame")
                                               : QCoreApplication::translate("StatsOverlay", "jitter buffer"));
    }
    if (haveFrames) {
        add(MetricNetworkLoss, QCoreApplication::translate("StatsOverlay", "Network loss"), pct(s.totalFrames ? 100.0 * s.networkDroppedFrames / s.totalFrames : 0.0));
        add(MetricJitterDrops, QCoreApplication::translate("StatsOverlay", "Jitter drops"), pct(s.decodedFrames ? 100.0 * s.pacerDroppedFrames / s.decodedFrames : 0.0));
        add(MetricRtt, QCoreApplication::translate("StatsOverlay", "Round trip"),
            s.lastRtt != 0 ? QStringLiteral("%1 ms ±%2").arg(s.lastRtt).arg(s.lastRttVariance) : QStringLiteral("–"));
    }
    if (s.framesWithHostProcessingLatency != 0) {
        add(MetricHostLatency, QCoreApplication::translate("StatsOverlay", "Host latency"),
            QStringLiteral("%1 (%2–%3)").arg(ms(hostAvg))
                .arg((double)s.minHostProcessingLatency / 10.0, 0, 'f', 1)
                .arg((double)s.maxHostProcessingLatency / 10.0, 0, 'f', 1));
    }
    if (haveFrames) {
        add(MetricDecodeTime, QCoreApplication::translate("StatsOverlay", "Decode"), ms(decodeMs));
        add(MetricQueueDelay, QCoreApplication::translate("StatsOverlay", "Queue delay"), ms(queueMs));
        add(MetricRenderTime, QCoreApplication::translate("StatsOverlay", "Render"), ms(renderMs));
        if (s.lastRtt != 0) {
            // One-way network time is estimated as half the round trip.
            add(MetricTotalLatency, QCoreApplication::translate("StatsOverlay", "Estimated total latency"),
                ms(hostAvg + s.lastRtt / 2.0 + decodeMs + queueMs + renderMs));
        }
    }
    if (!in.decoder.isEmpty()) {
        add(MetricDecoder, QCoreApplication::translate("StatsOverlay", "Decoder"), in.decoder);
    }

    return lines.join(QLatin1Char('\n'));
}

}
