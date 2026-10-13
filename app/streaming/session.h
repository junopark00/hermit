#pragma once

#include <atomic>
#include <memory>

#include <QHash>
#include <QMutex>
#include <QSemaphore>
#include <QQuickWindow>

#include <Limelight.h>
#include <opus_multistream.h>
#include "settings/streamingpreferences.h"
#include "input/input.h"
#include "video/decoder.h"
#include "audio/renderers/renderer.h"
#include "video/overlaymanager.h"
#include "sessionsummary.h"
#include "cursorpacket.h"
#include "video/statsoverlay.h"

class SupportedVideoFormatList : public QList<int>
{
public:
    operator int() const
    {
        int value = 0;

        for (const int v : *this) {
            value |= v;
        }

        return value;
    }

    void
    removeByMask(int mask)
    {
        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                this->removeAt(i);
            }
            else {
                i++;
            }
        }
    }

    void
    deprioritizeByMask(int mask)
    {
        QList<int> deprioritizedList;

        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                deprioritizedList.append(this->takeAt(i));
            }
            else {
                i++;
            }
        }

        this->append(std::move(deprioritizedList));
    }

    int maskByServerCodecModes(int serverCodecModes)
    {
        int mask = 0;

        const QMap<int, int> mapping = {
            {SCM_H264, VIDEO_FORMAT_H264},
            {SCM_H264_HIGH8_444, VIDEO_FORMAT_H264_HIGH8_444},
            {SCM_HEVC, VIDEO_FORMAT_H265},
            {SCM_HEVC_MAIN10, VIDEO_FORMAT_H265_MAIN10},
            {SCM_HEVC_REXT8_444, VIDEO_FORMAT_H265_REXT8_444},
            {SCM_HEVC_REXT10_444, VIDEO_FORMAT_H265_REXT10_444},
            {SCM_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN8},
            {SCM_AV1_MAIN10, VIDEO_FORMAT_AV1_MAIN10},
            {SCM_AV1_HIGH8_444, VIDEO_FORMAT_AV1_HIGH8_444},
            {SCM_AV1_HIGH10_444, VIDEO_FORMAT_AV1_HIGH10_444},
        };

        for (QMap<int, int>::const_iterator it = mapping.cbegin(); it != mapping.cend(); ++it) {
            if (serverCodecModes & it.key()) {
                mask |= it.value();
                serverCodecModes &= ~it.key();
            }
        }

        // Make sure nobody forgets to update this for new SCM values
        SDL_assert(serverCodecModes == 0);

        int val = *this;
        return val & mask;
    }
};

class ClipboardSync;
class NetDiagLog;

#include "networkadapt.h"

class StreamPanel;

class Session : public QObject
{
    Q_OBJECT

    friend class SdlInputHandler;
    friend class DeferredSessionCleanupTask;
    friend class AsyncConnectionStartThread;

public:
    explicit Session(NvComputer* computer, NvApp& app, StreamingPreferences *preferences = nullptr);
    virtual ~Session();

    Q_INVOKABLE bool initialize(QQuickWindow* qtWindow);
    Q_INVOKABLE void start();
    Q_INVOKABLE void interrupt();
    Q_PROPERTY(QStringList launchWarnings MEMBER m_LaunchWarnings NOTIFY launchWarningsChanged);

    static
    void getDecoderInfo(SDL_Window* window,
                        bool& isHardwareAccelerated, bool& isFullScreenOnly,
                        bool& isHdrSupported, QSize& maxResolution);

    static Session* get()
    {
        return s_ActiveSession;
    }

    Overlay::OverlayManager& getOverlayManager()
    {
        return m_OverlayManager;
    }

    // Hermit: shows the keyboard shortcut list over the stream, or hides it (Ctrl+Alt+Shift+H)
    void toggleHotkeyHelp();

    // Hermit: a notice with the mouse mode now on, and the panel's button follows it
    void notifyMouseMode(bool absolute);

    // Hermit: local cursor. Tells a Shell host that supports it whether this client wants the
    // cursor sent as shapes (remote desktop mouse mode and the setting on) or drawn into the video
    // as usual; called when the stream starts, when the mouse mode changes and when the setting
    // changes. The host's answer arrives as an event and turns the input handler's local cursor
    // on or off. Hosts without the capability are never asked. A request that failed (timeout,
    // or 403 while the host still registers the session) is sent once more 2 seconds later;
    // retry marks that attempt.
    void updateCursorMode(bool retry = false);

    // Who draws the cursor, for the performance overlay (StatsOverlay::CursorState); read by the
    // decoder thread
    int cursorOverlayState() const
    {
        return m_CursorOverlayState.load();
    }

    // Network adaptation for the performance overlay (StatsOverlay::Inputs adapt fields); read by
    // the decoder thread. bitrateKbps is 0 while the controller is off.
    void adaptOverlayState(int& bitrateKbps, int& fecPercent, int& regime) const
    {
        const uint64_t packed = m_AdaptOverlayState.load();
        bitrateKbps = static_cast<int>(packed & 0xFFFFFFFFu);
        fecPercent = static_cast<int>((packed >> 32) & 0xFF);
        regime = static_cast<int>((packed >> 40) & 0xFF);
    }

    // Hermit: performance overlay on or off (Ctrl+Alt+Shift+S, gamepad combo). The setting
    // follows, so the stream panel and Settings show the same state.
    void toggleStatsOverlay();

    // Called by the video decoder when it is destroyed, for the end-of-session summary.
    void recordVideoStats(const VIDEO_STATS& stats, int width, int height, int videoFormat, uint64_t endUs)
    {
        m_Summary.addDecoderStats(stats, width, height, videoFormat, endUs);
    }

    // Summary of the finished session (empty if it was too short). Valid from sessionFinished().
    Q_INVOKABLE QVariantMap summary() const
    {
        return m_SummaryResult;
    }

    // Averages of the ten sessions before this one, from the history file.
    Q_INVOKABLE QVariantMap previousSessionsAverage() const
    {
        // Skip the newest row only if this session was actually appended to the file.
        return m_SummaryResult.isEmpty() ? QVariantMap()
                                         : SessionSummary::recentAverages(10, m_SummaryResult.value("historySaved").toBool() ? 1 : 0);
    }

    // True if an established stream was cut off by a network problem (connection terminated,
    // video stopped). False for a user quit, the host app exiting, a host-side encoder error,
    // no video ever arriving (firewall), or a failure before the stream started. Valid from
    // sessionFinished().
    Q_INVOKABLE bool connectionLost() const
    {
        return m_ConnectionLost && m_UnexpectedTermination && m_AsyncConnectionSuccess;
    }

    // False if the launch failed in a way another attempt cannot fix (the host refused it, or
    // a different app is running), so automatic reconnection stops.
    Q_INVOKABLE bool launchRetryable() const
    {
        return m_LaunchRetryable;
    }

    // A new session for the same host and app, used to reconnect automatically.
    Q_INVOKABLE Session* createReconnectSession();

    // True if the stream panel ended this session to reconnect with new settings (resolution,
    // frame rate, bitrate, codec or HDR). Valid from sessionFinished().
    Q_INVOKABLE bool restartRequested() const
    {
        return m_RestartRequested;
    }

    // False for command-line launches: their overrides live only in memory, so the stream
    // panel must not save the settings.
    Q_INVOKABLE void setSavePreferences(bool save)
    {
        m_SavePreferences = save;
    }

    // Whether the host is currently known to be online (from host polling).
    Q_INVOKABLE bool isHostOnline() const;

    void flushWindowEvents();

    void setShouldExit(bool quitHostApp = false);

signals:
    void stageStarting(QString stage);

    void stageFailed(QString stage, int errorCode, QString failingPorts);

    void connectionStarted();

    void displayLaunchError(QString text);

    void quitStarting();

    void sessionFinished();

    // Emitted after sessionFinished() when the session is ready to be destroyed
    void readyForDeletion();

    void launchWarningsChanged();

private:
    void exec();

    bool startConnectionAsync();

    bool validateLaunch(SDL_Window* testWindow);

    void emitLaunchWarning(QString text);

    bool populateDecoderProperties(SDL_Window* window);

    IAudioRenderer* createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig);

    bool initializeAudioRenderer();

    bool testAudio(int audioConfiguration);

    int getAudioRendererCapabilities(int audioConfiguration);

    void getWindowDimensions(int& x, int& y,
                             int& width, int& height);

    void toggleFullscreen();

    // Hermit: files dropped on the stream window
    void handleDroppedFiles();

    void notifyMouseEmulationMode(bool enabled);

    void updateOptimalWindowDisplayMode();

    // Hermit: stream panel (streampanel.h)
    void openStreamPanel();
    void closeStreamPanel(bool returnToStream);
    void handleStreamPanelAction(int action, intptr_t value);
    void applyLiveSettings();

    // Hermit: network diagnostics (netdiag.h). start/stop follow the setting; publish hands the
    // recorder the bitrate and renderer state columns (main thread, cheap atomics).
    void startNetDiag();
    void stopNetDiag(bool toast);
    void publishNetDiagState();

    // Hermit: network adaptation (networkadapt.h). The 250 ms timer (SDL timer thread) posts
    // SDL_CODE_NETWORK_WINDOW; the main loop reads the frame totals and the round trip, runs the
    // controller and sends its bitrate (stream panel path) and FEC (/actions/fec) decisions.
    static
    Uint32 adaptTimerCallback(Uint32 interval, void* param);
    void handleNetworkWindow(uint64_t now);
    NetworkAdapt::Policy networkPolicy() const;
    void sendLiveFec(int percent);
    void handleLiveFecResult(uint32_t serial, int result);

    enum class DecoderAvailability {
        None,
        Software,
        Hardware
    };

    static
    DecoderAvailability getDecoderAvailability(SDL_Window* window,
                                               StreamingPreferences::VideoDecoderSelection vds,
                                               int videoFormat, int width, int height, int frameRate);

    static
    bool chooseDecoder(StreamingPreferences::VideoDecoderSelection vds,
                       StreamingPreferences::RendererSelection renderer,
                       SDL_Window* window, int videoFormat, int width, int height,
                       int frameRate, bool enableVsync, bool enableFramePacing,
                       bool testOnly,
                       IVideoDecoder*& chosenDecoder);

    static
    void clStageStarting(int stage);

    static
    void clStageFailed(int stage, int errorCode);

    static
    void clConnectionTerminated(int errorCode);

    static
    void clLogMessage(const char* format, ...);

    static
    void clRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor);

    static
    void clConnectionStatusUpdate(int connectionStatus);

    static
    void clSetHdrMode(bool enabled);

    static
    void clRumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    static
    void clSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    static
    void clSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    static
    void clSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

    // Hermit: a cursor message from the host (control stream, called on the library's callback
    // thread): parsed and assembled there, handed to the main loop through the mailbox
    static
    void clCursorPacket(uint16_t packetType, const uint8_t* payload, uint32_t length);

    // Hermit: local cursor (main thread)
    void handleCursorUpdates();
    void requestCursorShape(uint32_t id);
    void finishCursorShapeFetch(uint32_t id, bool failed);
    void applyCursorModeResult(bool requestedLocal, int result);
    void refreshCursorOverlayState();
    void cancelCursorModeRetry();
    void cancelCursorTimers();
    SDL_TimerID startCursorRetryTimer(int kind, uint32_t id);

    // Hermit: the 2-second retries of the local cursor (SDL timer thread): posts an event
    static
    Uint32 cursorRetryTimerCallback(Uint32 interval, void* param);
    struct CursorMailbox;
    static void postCursorUpdate(CursorMailbox& mailbox, CursorPacket::Update&& update);

    static
    int arInit(int audioConfiguration,
               const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
               void* arContext, int arFlags);

    static
    void arCleanup();

    static
    void arDecodeAndPlaySample(char* sampleData, int sampleLength);

    static
    int drSetup(int videoFormat, int width, int height, int frameRate, void*, int);

    static
    void drCleanup();

    static
    int drSubmitDecodeUnit(PDECODE_UNIT du);

    StreamingPreferences* m_Preferences;
    bool m_IsFullScreen;
    SupportedVideoFormatList m_SupportedVideoFormats; // Sorted in order of descending priority
    STREAM_CONFIGURATION m_StreamConfig;
    DECODER_RENDERER_CALLBACKS m_VideoCallbacks;
    AUDIO_RENDERER_CALLBACKS m_AudioCallbacks;
    NvComputer* m_Computer;
    NvApp m_App;
    SDL_Window* m_Window;
    IVideoDecoder* m_VideoDecoder;
    SDL_mutex* m_DecoderLock;
    bool m_AudioDisabled;
    bool m_AudioMuted;      // in effect: m_UserMuted or m_FocusMuted
    bool m_UserMuted;       // muted from the stream panel (kept across reconnects)
    // Hermit: mouse mode (absolute) and full screen as the user left them (stream panel or
    // shortcuts), kept across reconnects like m_UserMuted. Carried: from the session this one
    // reconnects; final: when this stream ended. -1 when unknown (the settings apply).
    int m_CarriedAbsoluteMouse;
    int m_CarriedFullScreen;
    int m_FinalAbsoluteMouse;
    int m_FinalFullScreen;
    bool m_FocusMuted;      // muted while the stream window is in the background
    QStringList m_DroppedFiles;
    int m_HotkeyHelpSerial;
    uint64_t m_LoopStartMs;
    bool m_StartHintPending;  // the shortcut hint of a stream started in game mouse mode
    uint64_t m_AutoBitrateRetryMs;
    bool m_ResyncLiveBitrate;   // send the bitrate again once the previous session's request is done
    bool m_ResyncRepeat;        // and once more after that
    uint32_t m_ResyncRequestId;
    uint64_t m_ResyncNotBeforeMs;
    int m_ResyncFailures;
    bool m_BitrateFailureNotified;  // one timeout notice per run of failures while the panel is closed
    bool m_BitrateRefusalNotified;  // and one refusal notice
    // Hermit: local cursor. Messages from the host wait in the mailbox (filled by the library's
    // callback thread and by shape fetches on pool threads, which hold a reference so a late
    // result never touches a finished session) until the main loop takes them.
    struct CursorMailbox {
        QMutex lock;
        QList<CursorPacket::Update> updates;
        CursorPacket::ShapeAssembler assembler;  // used by the callback thread only
        std::atomic<int> invalidMessages{0};     // dropped messages (one log line per session)
    };
    std::shared_ptr<CursorMailbox> m_CursorMailbox;
    uint32_t m_CursorModeSerial;       // results of a previous session's requests are ignored
    bool m_CursorModeInFlight;
    bool m_CursorModeDirty;            // the wanted mode changed while a request was running
    int m_CursorModeSent;              // the mode the host was last asked for: -1 none, 0 host, 1 local
    bool m_CursorUnsupported;          // the host answered 404: not asked again this session
    bool m_CursorDeclineNotified;      // one notice per session when the host declines
    std::atomic<int> m_CursorOverlayState;
    // Shapes fetched over HTTPS: at most two attempts per shape, the second 2 seconds after a
    // failure (the host answers 404 once its cursor has changed again)
    struct CursorShapeFetch {
        int attempts = 0;
        uint64_t notBeforeMs = 0;
    };
    QHash<uint32_t, CursorShapeFetch> m_CursorShapeFetches;
    bool m_CursorShapeFetchInFlight;
    uint32_t m_CursorShapePendingId;   // a second shape to fetch once the running fetch is done
    SDL_TimerID m_CursorModeRetryTimer;
    SDL_TimerID m_CursorShapeRetryTimer;
    bool m_CursorModeRetried;          // the one retry of a failed mode request is used up
    Uint32 m_FullScreenFlag;
    QQuickWindow* m_QtWindow;
    bool m_UnexpectedTermination;
    bool m_ConnectionLost;
    bool m_IsReconnect;
    bool m_LaunchRetryable;
    SdlInputHandler* m_InputHandler;
    ClipboardSync* m_ClipboardSync;
    NetDiagLog* m_NetDiag;
    StreamPanel* m_Panel;
    bool m_PanelRestoreCapture;
    bool m_RestartRequested;
    bool m_QuitHostAppOnExit;
    bool m_AppliedVsync;
    bool m_AppliedFramePacing;
    bool m_SavePreferences;
    NetworkAdapt m_NetworkAdapt;
    // Read on the connection-status callback thread too
    std::atomic<bool> m_AutoBitrateRunning;
    SDL_TimerID m_AdaptTimer;
    uint32_t m_AdaptLastReceived;      // frame totals at the last window (netdiag.h)
    uint32_t m_AdaptLastDropped;
    std::atomic<uint64_t> m_AdaptOverlayState;  // bitrate | fec << 32 | regime << 40
    // Live FEC (/actions/fec): one request in flight, the newest value waits; serials are unique
    // across sessions so a late result of the previous stream is ignored
    bool m_LiveFec;                    // the host advertises LiveFec and has not refused it
    uint32_t m_FecPendingSerial;       // 0: nothing in flight
    int m_FecPendingPercent;
    int m_FecSentPercent;              // the last value the host confirmed (NetworkAdapt::kFecBase: the
                                       // default); -1: unknown, the default is sent once
    int m_FecEffectivePercent;         // the host's effective value from its last reply (0: unknown)
    bool m_FecUncertain;               // the last request failed on the way: sent again every 5 s
    uint64_t m_FecRetryMs;
    int m_MouseEmulationRefCount;
    int m_FlushingWindowEventsRef;
    QStringList m_LaunchWarnings;
    bool m_ShouldExit;

    bool m_AsyncConnectionSuccess;

    int m_ActiveVideoFormat;
    int m_ActiveVideoWidth;
    int m_ActiveVideoHeight;
    int m_ActiveVideoFrameRate;

    OpusMSDecoder* m_OpusDecoder;
    IAudioRenderer* m_AudioRenderer;
    OPUS_MULTISTREAM_CONFIGURATION m_ActiveAudioConfig;
    OPUS_MULTISTREAM_CONFIGURATION m_OriginalAudioConfig;
    int m_AudioSampleCount;
    Uint32 m_DropAudioEndTime;

    Overlay::OverlayManager m_OverlayManager;

    SessionSummary m_Summary;
    QVariantMap m_SummaryResult;

    static CONNECTION_LISTENER_CALLBACKS k_ConnCallbacks;
    static Session* s_ActiveSession;
    static QSemaphore s_ActiveSessionSemaphore;
};
