#pragma once

// Hermit: network diagnostics (DEV-PLAN 1.4, step 1). A measurement mode, off by default, that
// records every 100 ms what the network and the playback pipeline did during a stream, so the
// slow-motion / fast-forward behaviour on an unstable link can be analysed before any control
// change. Rows go to %TEMP%\Hermit-netdiag-<unix time>.csv; the last 5 files are kept. Nothing
// leaves the PC.
//
// Three parts:
// - NetDiagRow: one CSV row and its formatting (pure, tested in hermit_core_test.cpp);
// - NetDiagWindow: the counters the frame path feeds (decoder thread: arrivals and decodes;
//   pacer threads: renders, drops, queue lengths) with relaxed atomics, and the sampler's read
//   that closes a window (pure, tested);
// - NetDiagLog: the recorder. Owns the file and a sampling thread (QThread::create with a timed
//   wait, no event loop, no moc) that writes one row per 100 ms. The frame path checks one
//   atomic flag (isOn) per frame; while the mode is off that branch is all it costs.

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <cstdint>

class QFile;
class QThread;

struct NetDiagRow
{
    int64_t tMs = 0;                       // since the recording started
    uint32_t framesReceived = 0;           // in this window
    uint32_t framesDecoded = 0;
    uint32_t framesRendered = 0;
    uint32_t framesNetworkDropped = 0;     // frame numbers skipped on arrival
    uint32_t pacerDropped = 0;             // discarded by the pacer (catch-up or a full queue)
    int pacingQueueLen = 0;                // longest pacing queue seen in the window
    int renderQueueLen = 0;                // longest render queue seen in the window
    int lastFrameArrivalGapMs = 0;         // longest gap between arrivals (a stall still in progress counts)
    float hostLatencyMs = 0;               // average host processing latency of the window's frames
    float decodeMsAvg = 0;                 // average queue-plus-decode time
    float renderMsAvg = 0;                 // average render call time
    int rttMs = 0;                         // control stream round trip (enet)
    int rttVarianceMs = 0;
    float lossPctWindow = -1;              // network dropped / (received + dropped), -1 without frames
    int requestedKbps = 0;                 // the bitrate the user chose (the ceiling)
    int appliedKbps = 0;                   // the bitrate the host confirmed
    int autoKbps = 0;                      // the automatic bitrate controller's value, 0 when off
    bool vsync = false;
    bool framePacing = false;
    QString event;                         // free text: what changed in this window

    static QByteArray header()
    {
        return QByteArrayLiteral("t_ms,frames_received,frames_decoded,frames_rendered,frames_network_dropped,"
                                 "pacer_dropped,pacing_queue_len,render_queue_len,last_frame_arrival_gap_ms,"
                                 "host_latency_ms,decode_ms_avg,render_ms_avg,rtt_ms,rtt_variance_ms,"
                                 "loss_pct_window,requested_kbps,applied_kbps,auto_kbps,vsync,frame_pacing,event\n");
    }

    // RFC 4180: quoted when it holds a comma, a quote or a line break; quotes doubled
    static QByteArray csvField(const QString& text)
    {
        if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')) &&
                !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r'))) {
            return text.toUtf8();
        }
        QString quoted = text;
        quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return '"' + quoted.toUtf8() + '"';
    }

    QByteArray toCsv() const
    {
        QByteArray line;
        line.reserve(160);
        line += QByteArray::number(static_cast<qlonglong>(tMs)) + ',';
        line += QByteArray::number(framesReceived) + ',';
        line += QByteArray::number(framesDecoded) + ',';
        line += QByteArray::number(framesRendered) + ',';
        line += QByteArray::number(framesNetworkDropped) + ',';
        line += QByteArray::number(pacerDropped) + ',';
        line += QByteArray::number(pacingQueueLen) + ',';
        line += QByteArray::number(renderQueueLen) + ',';
        line += QByteArray::number(lastFrameArrivalGapMs) + ',';
        line += QByteArray::number(hostLatencyMs, 'f', 1) + ',';
        line += QByteArray::number(decodeMsAvg, 'f', 2) + ',';
        line += QByteArray::number(renderMsAvg, 'f', 2) + ',';
        line += QByteArray::number(rttMs) + ',';
        line += QByteArray::number(rttVarianceMs) + ',';
        line += QByteArray::number(lossPctWindow, 'f', 1) + ',';
        line += QByteArray::number(requestedKbps) + ',';
        line += QByteArray::number(appliedKbps) + ',';
        line += QByteArray::number(autoKbps) + ',';
        line += (vsync ? "1," : "0,");
        line += (framePacing ? "1," : "0,");
        line += csvField(event);
        line += '\n';
        return line;
    }
};

// Counters of the current 100 ms window. Writers are the frame-path threads (one relaxed atomic
// operation each); the reader is the sampling thread, which exchanges the window counters for
// zero. A lost update in a race costs at most one frame in one row; nothing ever blocks.
class NetDiagWindow
{
public:
    void reset(uint64_t nowUs)
    {
        m_Received.store(0, std::memory_order_relaxed);
        m_Decoded.store(0, std::memory_order_relaxed);
        m_Rendered.store(0, std::memory_order_relaxed);
        m_NetworkDropped.store(0, std::memory_order_relaxed);
        m_PacerDropped.store(0, std::memory_order_relaxed);
        m_LastArrivalUs.store(nowUs, std::memory_order_relaxed);
        m_MaxGapUs.store(0, std::memory_order_relaxed);
        m_HostLatencyTenths.store(0, std::memory_order_relaxed);
        m_HostLatencyFrames.store(0, std::memory_order_relaxed);
        m_DecodeUs.store(0, std::memory_order_relaxed);
        m_RenderUs.store(0, std::memory_order_relaxed);
        m_PacingQueueMax.store(0, std::memory_order_relaxed);
        m_RenderQueueMax.store(0, std::memory_order_relaxed);
    }

    // Decoder thread, once per decode unit, recording or not: running totals for the network
    // adaptation controller (session.cpp reads them every 250 ms and takes the difference).
    // Two relaxed increments; never reset.
    void onFrameArrival(uint32_t droppedBefore)
    {
        m_TotalReceived.fetch_add(1, std::memory_order_relaxed);
        if (droppedBefore != 0) {
            m_TotalNetworkDropped.fetch_add(droppedBefore, std::memory_order_relaxed);
        }
    }

    void totals(uint32_t& received, uint32_t& networkDropped) const
    {
        received = m_TotalReceived.load(std::memory_order_relaxed);
        networkDropped = m_TotalNetworkDropped.load(std::memory_order_relaxed);
    }

    // Decoder thread, once per decode unit while recording. droppedBefore: frame numbers skipped
    // since the last one; hostLatencyTenthsMs: the host's processing time in 0.1 ms units (0 if
    // unknown).
    void onFrameReceived(uint64_t nowUs, uint32_t droppedBefore, uint16_t hostLatencyTenthsMs)
    {
        const uint64_t last = m_LastArrivalUs.load(std::memory_order_relaxed);
        m_LastArrivalUs.store(nowUs, std::memory_order_relaxed);
        if (last != 0 && nowUs > last) {
            const uint64_t gap = nowUs - last;
            if (gap > m_MaxGapUs.load(std::memory_order_relaxed)) {
                m_MaxGapUs.store(gap, std::memory_order_relaxed);
            }
        }
        m_Received.fetch_add(1, std::memory_order_relaxed);
        if (droppedBefore != 0) {
            m_NetworkDropped.fetch_add(droppedBefore, std::memory_order_relaxed);
        }
        if (hostLatencyTenthsMs != 0) {
            m_HostLatencyTenths.fetch_add(hostLatencyTenthsMs, std::memory_order_relaxed);
            m_HostLatencyFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Decoder thread: a frame came out of the decoder; decodeUs counts from its arrival
    void onFrameDecoded(uint64_t decodeUs)
    {
        m_Decoded.fetch_add(1, std::memory_order_relaxed);
        m_DecodeUs.fetch_add(decodeUs, std::memory_order_relaxed);
    }

    // Render thread (or the main thread): a frame was presented
    void onFrameRendered(uint64_t renderUs)
    {
        m_Rendered.fetch_add(1, std::memory_order_relaxed);
        m_RenderUs.fetch_add(renderUs, std::memory_order_relaxed);
    }

    // Pacer threads: a decoded frame was thrown away (catch-up, or a full queue)
    void onPacerDrop(uint32_t count = 1)
    {
        m_PacerDropped.fetch_add(count, std::memory_order_relaxed);
    }

    // Pacer, after a queue changed (called with the pacer's queue lock held, so it must stay cheap)
    void setQueueLengths(int pacing, int render)
    {
        if (pacing > m_PacingQueueMax.load(std::memory_order_relaxed)) {
            m_PacingQueueMax.store(pacing, std::memory_order_relaxed);
        }
        if (render > m_RenderQueueMax.load(std::memory_order_relaxed)) {
            m_RenderQueueMax.store(render, std::memory_order_relaxed);
        }
    }

    // Sampling thread: closes the window that ends at nowUs into row's frame columns and starts
    // the next one. The arrival gap is the longest gap between two arrivals in the window, or
    // the time since the last arrival when that is longer (a stall shows while it lasts).
    void sample(uint64_t nowUs, NetDiagRow& row)
    {
        row.framesReceived = m_Received.exchange(0, std::memory_order_relaxed);
        row.framesDecoded = m_Decoded.exchange(0, std::memory_order_relaxed);
        row.framesRendered = m_Rendered.exchange(0, std::memory_order_relaxed);
        row.framesNetworkDropped = m_NetworkDropped.exchange(0, std::memory_order_relaxed);
        row.pacerDropped = m_PacerDropped.exchange(0, std::memory_order_relaxed);
        row.pacingQueueLen = m_PacingQueueMax.exchange(0, std::memory_order_relaxed);
        row.renderQueueLen = m_RenderQueueMax.exchange(0, std::memory_order_relaxed);

        uint64_t gapUs = m_MaxGapUs.exchange(0, std::memory_order_relaxed);
        const uint64_t last = m_LastArrivalUs.load(std::memory_order_relaxed);
        if (last != 0 && nowUs > last) {
            gapUs = (std::max)(gapUs, nowUs - last);
        }
        row.lastFrameArrivalGapMs = static_cast<int>(gapUs / 1000);

        const uint64_t latencyTenths = m_HostLatencyTenths.exchange(0, std::memory_order_relaxed);
        const uint32_t latencyFrames = m_HostLatencyFrames.exchange(0, std::memory_order_relaxed);
        row.hostLatencyMs = latencyFrames != 0 ? static_cast<float>(latencyTenths) / (10.0f * latencyFrames) : 0.0f;
        const uint64_t decodeUs = m_DecodeUs.exchange(0, std::memory_order_relaxed);
        row.decodeMsAvg = row.framesDecoded != 0 ? static_cast<float>(decodeUs) / (1000.0f * row.framesDecoded) : 0.0f;
        const uint64_t renderUs = m_RenderUs.exchange(0, std::memory_order_relaxed);
        row.renderMsAvg = row.framesRendered != 0 ? static_cast<float>(renderUs) / (1000.0f * row.framesRendered) : 0.0f;

        const uint32_t total = row.framesReceived + row.framesNetworkDropped;
        row.lossPctWindow = total != 0 ? static_cast<float>(row.framesNetworkDropped) * 100.0f / total : -1.0f;
    }

private:
    std::atomic<uint32_t> m_Received {0};
    std::atomic<uint32_t> m_Decoded {0};
    std::atomic<uint32_t> m_Rendered {0};
    std::atomic<uint32_t> m_NetworkDropped {0};
    std::atomic<uint32_t> m_PacerDropped {0};
    std::atomic<uint64_t> m_LastArrivalUs {0};
    std::atomic<uint64_t> m_MaxGapUs {0};
    std::atomic<uint64_t> m_HostLatencyTenths {0};
    std::atomic<uint32_t> m_HostLatencyFrames {0};
    std::atomic<uint64_t> m_DecodeUs {0};
    std::atomic<uint64_t> m_RenderUs {0};
    std::atomic<int> m_PacingQueueMax {0};
    std::atomic<int> m_RenderQueueMax {0};
    std::atomic<uint32_t> m_TotalReceived {0};
    std::atomic<uint32_t> m_TotalNetworkDropped {0};
};

// The recorder. One per stream, owned by Session; created when the stream loop starts (or when
// the setting is turned on from the stream panel) and deleted when it ends (or the setting is
// turned off). Starts the sampling thread in start() and stops it in the destructor.
class NetDiagLog
{
public:
    static constexpr int kIntervalMs = 100;
    static constexpr int kKeepFiles = 5;

    NetDiagLog();
    ~NetDiagLog();

    // Prunes old files, opens a new one and starts sampling. False (and nothing recorded) when
    // the file cannot be created.
    bool start();

    // Stops sampling, closes the file and writes the summary line to the log. Called by the
    // destructor as well.
    void stop();

    QString filePath() const { return m_FilePath; }
    QString fileName() const;
    int rows() const { return m_Rows.load(std::memory_order_relaxed); }
    int maxArrivalGapMs() const { return m_MaxGapMs.load(std::memory_order_relaxed); }

    // ---- Frame path (any thread) ----
    static bool isOn() { return s_On.load(std::memory_order_relaxed); }
    static NetDiagWindow& window() { return s_Window; }

    // ---- Session (main thread) ----
    // The bitrate columns: the user's choice, the host-confirmed value and the controller's
    static void setBitrates(int requestedKbps, int appliedKbps, int autoKbps);
    static void setRenderMode(bool vsync, bool framePacing);
    // Free text for the event column of the next row (joined with "; " when several come)
    static void note(const QString& text);

    // The folder the files go to (QStandardPaths temp) and the file name pattern
    static QString folder();
    static QString filePattern() { return QStringLiteral("Hermit-netdiag-*.csv"); }
    // Removes all but the newest `keep` files matching filePattern() in folder
    static void pruneOldFiles(const QString& folder, int keep);

private:
    void threadProc();
    bool writeRow(const NetDiagRow& row);

    QString m_FilePath;
    QFile* m_File;
    QThread* m_Thread;
    QMutex m_StopLock;
    QWaitCondition m_StopSignal;
    bool m_Stopping;
    uint64_t m_StartUs;
    std::atomic<int> m_Rows {0};
    std::atomic<int> m_MaxGapMs {0};
    uint32_t m_TotalReceived;
    uint32_t m_TotalNetworkDropped;
    uint32_t m_TotalPacerDropped;
    bool m_Summarised;

    static std::atomic<bool> s_On;
    static NetDiagWindow s_Window;
    static std::atomic<int> s_RequestedKbps;
    static std::atomic<int> s_AppliedKbps;
    static std::atomic<int> s_AutoKbps;
    static std::atomic<bool> s_Vsync;
    static std::atomic<bool> s_FramePacing;
    static QMutex s_NoteLock;
    static QString s_Note;
};
