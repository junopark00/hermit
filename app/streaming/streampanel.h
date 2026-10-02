#pragma once

#include <QObject>
#include <QRect>
#include <QString>

#include <SDL.h>

class QQmlEngine;
class QQuickWindow;

// SDL user event codes for the stream panel (SDL_USEREVENT, handled by Session::exec()).
#define SDL_CODE_TOGGLE_STREAM_PANEL 107
#define SDL_CODE_STREAM_PANEL_ACTION 108
#define SDL_CODE_NETWORK_WINDOW 109

// Hermit: settings panel shown over the stream (Ctrl+Alt+Shift+P, or the chevron handle at the
// left or right edge of the stream window: always in remote desktop mouse mode, and in game mouse
// mode while the mouse is not captured). It is a Qt Quick window: Qt event processing is
// suspended during a stream, so Session::exec() pumps Qt events only while the panel or its handle
// is visible. Settings that only need the client change at once; resolution, frame rate, bitrate,
// codec and HDR are applied by reconnecting with the new settings (the host app keeps running).
//
// QML calls arrive while Session::exec() pumps Qt events; actions that must run in the SDL loop
// are posted back to it as SDL_CODE_STREAM_PANEL_ACTION events.
class StreamPanel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString hostName MEMBER m_HostName CONSTANT)
    Q_PROPERTY(QString appName MEMBER m_AppName CONSTANT)

    // What the stream was started with: changing these needs a reconnect.
    Q_PROPERTY(int startWidth MEMBER m_StartWidth CONSTANT)
    Q_PROPERTY(int startHeight MEMBER m_StartHeight CONSTANT)
    Q_PROPERTY(int startFps MEMBER m_StartFps CONSTANT)
    // The bitrate in effect: the start value, or the last one the host applied live
    Q_PROPERTY(int startBitrateKbps MEMBER m_StartBitrateKbps NOTIFY liveBitrateChanged)
    // 0 unknown, 1 the host changes the bitrate live (Shell), 2 it cannot (reconnect needed)
    Q_PROPERTY(int liveBitrateState MEMBER m_LiveBitrateState NOTIFY liveBitrateChanged)
    Q_PROPERTY(bool liveBitratePending MEMBER m_LiveBitratePending NOTIFY liveBitrateChanged)
    // The last live change failed on the way; the host may stream at another bitrate (retrying)
    Q_PROPERTY(bool appliedUncertain MEMBER m_AppliedUncertain NOTIFY liveBitrateChanged)
    // Bitrate the automatic adjustment is at (0 when it is off)
    Q_PROPERTY(int autoBitrateKbps MEMBER m_AutoBitrateKbps NOTIFY liveBitrateChanged)
    Q_PROPERTY(int startVideoCodecConfig MEMBER m_StartVideoCodecConfig CONSTANT)
    Q_PROPERTY(bool startEnableHdr MEMBER m_StartEnableHdr CONSTANT)
    // What Revert restores while the host cannot change the bitrate live: the user's last pick
    // that took effect (never a value automatic bitrate chose)
    Q_PROPERTY(int revertBitrateKbps MEMBER m_RevertBitrateKbps NOTIFY liveBitrateChanged)
    Q_PROPERTY(bool revertAutoAdjustBitrate MEMBER m_RevertAutoAdjustBitrate NOTIFY liveBitrateChanged)

    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    // Mouse mode in effect: true for remote desktop (absolute), false for game (relative)
    Q_PROPERTY(bool absoluteMouse READ absoluteMouse NOTIFY absoluteMouseChanged)
    // Edge of the stream window with the handle, where the panel opens too: 0 right, 1 left
    Q_PROPERTY(int handleSide READ handleSide WRITE setHandleSide NOTIFY handleSideChanged)

public:
    enum Action {
        ActionClose,
        ActionApplyLive,
        ActionReconnect,
        ActionDisconnect,
        ActionQuitAppAndDisconnect,
        ActionToggleFullScreen,
        ActionToggleMouseMode,
        ActionToggleMute,
        ActionOpen,
        ActionLiveBitrate,        // data2: request id (high half), kbps (low half)
        ActionLiveBitrateResult,  // data2: request id (high half), result (low half): encoding kbps,
                                  // 0 on a transient failure, -1 unsupported
        ActionShowHotkeys,
        ActionSavePreferences,
    };

    StreamPanel(QQmlEngine* engine, SDL_Window* streamWindow,
                const QString& hostName, const QString& appName);
    ~StreamPanel() override;

    bool isOpen() const { return m_Open; }

    // True if the panel is open but neither it nor the stream window has had focus for a while
    // (the user switched to another app).
    bool lostFocus(bool streamFocused) const;

    // True if the panel is open and the user clicked back into the stream window.
    bool streamClickedBack(bool streamFocused) const;

    // True while a Qt window of the panel is shown (Qt events must be processed).
    bool needsQtEvents() const { return m_Open || m_HandleShown; }

    void open();
    void close();

    // Called on every pass of the SDL loop: shows the handle while the stream window has focus
    // and the pointer can reach the handle (remote desktop mouse mode, where the pointer moves
    // freely, or a released mouse in game mode), and keeps the windows on the stream window.
    void sync(bool mouseCaptured, bool absoluteMouse, bool streamFocused, bool minimized);

    void setMuted(bool muted);
    bool muted() const { return m_Muted; }
    void setAbsoluteMouse(bool absolute)
    {
        if (m_AbsoluteMouse != absolute) {
            m_AbsoluteMouse = absolute;
            emit absoluteMouseChanged();
        }
    }
    bool absoluteMouse() const { return m_AbsoluteMouse; }

    // Result of a live bitrate request (see ActionLiveBitrateResult)
    void setLiveBitrateResult(uint32_t requestId, int result);

    // The request this panel waits for. Requests and results of the previous session can still
    // be in the event queue after a reconnect; their ids belong to no request of this panel.
    bool awaitingRequest(uint32_t requestId) const
    {
        return m_LiveBitratePending && requestId == m_PendingRequestId;
    }

    // Settings that need a reconnect differ from what the stream started with (QML revertNeeded)
    bool reconnectSettingsChanged() const;

    // The host is not known to refuse live bitrate changes
    bool liveBitrateSupported() const { return m_LiveBitrateState != 2; }

    // The bitrate the host streams at (the start value or the last one applied live)
    int appliedBitrateKbps() const { return m_StartBitrateKbps; }
    bool liveBitratePending() const { return m_LiveBitratePending; }
    int pendingBitrateKbps() const { return m_PendingBitrateKbps; }

    // The bitrate slider is held (QML)
    Q_INVOKABLE void setBitrateEditing(bool editing) { m_BitrateEditing = editing; }
    bool bitrateEditing() const { return m_BitrateEditing; }

    void setAutoBitrate(int kbps)
    {
        if (m_AutoBitrateKbps != kbps) {
            m_AutoBitrateKbps = kbps;
            emit liveBitrateChanged();
        }
    }

    Q_INVOKABLE void closePanel() { post(ActionClose); }
    Q_INVOKABLE void openPanel() { post(ActionOpen); }
    Q_INVOKABLE void applyLive() { post(ActionApplyLive); }
    Q_INVOKABLE void reconnect() { post(ActionReconnect); }
    Q_INVOKABLE void disconnect() { post(ActionDisconnect); }
    Q_INVOKABLE void quitAppAndDisconnect() { post(ActionQuitAppAndDisconnect); }
    Q_INVOKABLE void toggleFullScreen() { post(ActionToggleFullScreen); }
    Q_INVOKABLE void toggleMouseMode() { post(ActionToggleMouseMode); }
    Q_INVOKABLE void toggleMute() { post(ActionToggleMute); }
    Q_INVOKABLE void showHotkeys() { post(ActionShowHotkeys); }

    int handleSide() const { return m_HandleSide; }
    void setHandleSide(int side);
    // Moving the handle up or down (held, then dragged); follows the mouse pointer
    Q_INVOKABLE void beginHandleDrag();
    Q_INVOKABLE void dragHandle();
    Q_INVOKABLE void endHandleDrag();

    // The user picked a bitrate: save it and apply it live where the host can. When the host
    // cannot, it waits for "Apply and reconnect" (which saves) and can still be reverted.
    Q_INVOKABLE void chooseBitrate(int kbps);

    // Revert: the start values go back to disk too (a pick may have been saved already)
    Q_INVOKABLE void savePreferences() { post(ActionSavePreferences); }
    Q_INVOKABLE void applyLiveBitrate(int kbps);

    // The bitrate this stream started with, when the session changed it from the setting (the
    // YUV 4:4:4 fallback on hosts without 4:4:4 uses the 4:2:0 default)
    void setStreamBitrate(int kbps);

    // What the bitrate setting means for this stream: the stream's start bitrate while the
    // setting is unchanged, otherwise the setting
    Q_INVOKABLE int chosenBitrateFor(int prefKbps) const
    {
        return prefKbps == m_StartPrefBitrateKbps ? m_StreamStartBitrateKbps : prefKbps;
    }
    int chosenBitrateKbps() const;

    // The last request failed on the way (a timeout can come after the host applied it), so
    // the host may stream at another bitrate than the applied one: send even an equal value
    bool appliedUncertain() const { return m_AppliedUncertain; }

    // The user's bitrate choice is not confirmed on the host yet: picked in the panel, or being
    // brought back after automatic bitrate was turned off. Failures then get a notice.
    bool userPickUnconfirmed() const { return m_UserPickUnconfirmed; }
    void setUserPickUnconfirmed(bool unconfirmed) { m_UserPickUnconfirmed = unconfirmed; }

    // Sends a bitrate even if it equals the applied one (a request of the previous session may
    // have reached the host after this stream started). Returns the request id, 0 if not sent.
    uint32_t resendLiveBitrate(int kbps);

signals:
    void mutedChanged();
    void absoluteMouseChanged();
    void handleSideChanged();
    void liveBitrateChanged();

private:
    bool post(Action action, intptr_t value = 0);
    void sendLiveBitrate(int kbps);
    void promoteRevertPoint();
    QQuickWindow* createWindow(const char* qmlFile);
    QRect streamClientRect() const;
    QPoint cursorPos() const;
    void placeWindows();
    void placeWindow(QQuickWindow* window, int x, int y, int width, int height);
    qreal streamDpr() const;

    QQmlEngine* m_Engine;
    SDL_Window* m_StreamWindow;
    QString m_HostName;
    QString m_AppName;
    QQuickWindow* m_Panel = nullptr;
    QQuickWindow* m_Handle = nullptr;
    bool m_Open = false;
    Uint32 m_OpenedAt = 0;
    bool m_HandleShown = false;
    bool m_Muted = false;
    bool m_AbsoluteMouse = false;
    QRect m_LastStreamRect;

    int m_StartWidth;
    int m_StartHeight;
    int m_StartFps;
    int m_StartBitrateKbps;
    int m_StartVideoCodecConfig;
    bool m_StartEnableHdr;
    int m_LiveBitrateState = 0;
    bool m_LiveBitratePending = false;
    int m_PendingBitrateKbps = 0;  // the request in flight
    uint32_t m_PendingRequestId = 0;
    int m_RevertBitrateKbps;
    bool m_RevertAutoAdjustBitrate;
    bool m_AppliedUncertain = false;
    bool m_UserPickUnconfirmed = false;
    int m_StartPrefBitrateKbps;
    int m_StreamStartBitrateKbps;
    int m_CandidateBitrateKbps = 0;  // a pick (setting value) waiting to take effect
    bool m_CandidateAutoAdjustBitrate = false;
    int m_QueuedBitrateKbps = 0;   // the latest value asked for while a request was pending
    int m_AutoBitrateKbps = 0;   // where automatic bitrate is (0 when it is off)
    bool m_BitrateEditing = false;
    bool m_HandleFailed = false;   // the handle's QML could not be created; do not retry
    qreal m_LastDpr = 0;
    int m_HandleSide = 0;          // 0 right, 1 left
    double m_HandleY = 0.5;        // the handle's centre, 0 top to 1 bottom of the stream window
    bool m_HandleDragging = false;
    int m_HandleGrabOffset = 0;    // pointer to handle centre while dragging, physical pixels
};
