#pragma once

#include <atomic>

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

#include "autobitrate.h"

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

    // Hermit: network figures of the last stats window (decoder thread), for automatic bitrate.
    void reportNetworkWindow(float lossPct, int rttMs);

    // Hermit: shows the keyboard shortcut list over the stream, or hides it (Ctrl+Alt+Shift+H)
    void toggleHotkeyHelp();

    // Hermit: a notice with the mouse mode now on, and the panel's button follows it
    void notifyMouseMode(bool absolute);

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
    Uint32 m_FullScreenFlag;
    QQuickWindow* m_QtWindow;
    bool m_UnexpectedTermination;
    bool m_ConnectionLost;
    bool m_IsReconnect;
    bool m_LaunchRetryable;
    SdlInputHandler* m_InputHandler;
    ClipboardSync* m_ClipboardSync;
    StreamPanel* m_Panel;
    bool m_PanelRestoreCapture;
    bool m_RestartRequested;
    bool m_QuitHostAppOnExit;
    bool m_AppliedVsync;
    bool m_AppliedFramePacing;
    bool m_SavePreferences;
    AutoBitrate m_AutoBitrate;
    // Read on the connection-status callback thread too
    std::atomic<bool> m_AutoBitrateRunning;
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
