#pragma once

#include "decoder.h"

#include <QString>
#include <QStringList>

// Hermit's performance overlay: which metrics are shown (chosen in Settings) and how the text
// is laid out. The overlay text is "label<TAB>value" lines, which OverlayManager draws as a
// two-column panel.
namespace StatsOverlay {

// Bits of StreamingPreferences::overlayMetrics. Values are stored in the settings, so existing
// bits must never be renumbered.
enum Metric : int {
    MetricVideo = 1 << 0,          // resolution, frame rate, codec
    MetricFrameRates = 1 << 1,     // incoming / decoded / rendered FPS
    MetricBitrate = 1 << 2,        // average and peak video bitrate
    MetricNetworkLoss = 1 << 3,    // frames dropped by the network
    MetricJitterDrops = 1 << 4,    // frames dropped due to jitter
    MetricRtt = 1 << 5,            // round trip time and variance
    MetricHostLatency = 1 << 6,    // host processing latency avg (min-max)
    MetricDecodeTime = 1 << 7,
    MetricQueueDelay = 1 << 8,
    MetricRenderTime = 1 << 9,
    MetricTotalLatency = 1 << 10,  // estimated end-to-end latency
    MetricDecoder = 1 << 11,       // renderer / decoder in use
};

// Shown by default: what tells whether the stream is healthy and responsive.
constexpr int kDefaultMetrics = MetricVideo | MetricBitrate | MetricNetworkLoss | MetricRtt |
                                MetricHostLatency | MetricTotalLatency;

struct Inputs {
    const VIDEO_STATS* stats = nullptr;  // statistics over the last ~2 seconds
    int width = 0;
    int height = 0;
    QString codec;
    double avgMbps = 0.0;
    double peakMbps = 0.0;
    QString decoder;
};

// Returns the overlay text for the selected metrics ("label\tvalue" per line).
QString format(const Inputs& in, int metrics);

}
