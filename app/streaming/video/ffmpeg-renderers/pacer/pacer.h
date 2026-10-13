#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

#include <atomic>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)

class TimerVsyncSource;

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    // Hermit: timerPacing (no V-Sync) paces frames with a timer at the stream's frame rate when
    // there is no V-Sync pacing, so the pacing queue's lenient history smooths out uneven
    // arrivals (up to 3 frames): the jitter buffer of the quality priority playback policy.
    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool timerPacing);

    // Hermit: the frame priority playback policy, changed live from the main thread. On: only
    // the newest frame waits (pacing queue 1, render queue 1 behind the frame just rendered),
    // and the timer is bypassed: a new frame replaces any that is not rendering yet. Off: the
    // lenient queue history.
    void setLowLatency(bool lowLatency) { m_LowLatency = lowLatency; }

    // Hermit: the frame rate the host streams at (live frame rate changes): the timer's tick
    // and the pacing history follow it. Nothing to do without the timer.
    void setPacingFps(int fps);

    // Hermit: what playback does now (StatsOverlay::PlaybackMode): newest frame, jitter buffer
    // (the timer or V-Sync pacing) or direct
    int playbackMode() const;

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    // Hermit: network diagnostics (netdiag.h). Called with m_FrameQueueLock held after a queue
    // changed; one atomic flag check while the mode is off.
    void publishQueueLengths();

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    TimerVsyncSource* m_TimerSource;  // m_VsyncSource when the timer paces frames, else null
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    std::atomic<int> m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;
    std::atomic<bool> m_LowLatency;
};
