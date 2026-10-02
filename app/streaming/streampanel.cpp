#include "streampanel.h"

#include "settings/resolutionpresets.h"
#include "settings/streamingpreferences.h"
#include "streaming/streamutils.h"

#include <QCursor>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSettings>
#include <QUrl>

#include <SDL_syswm.h>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

namespace {

// Sizes in device-independent pixels
constexpr int kPanelWidth = 380;
constexpr int kHandleWidth = 20;
constexpr int kHandleHeight = 60;

// Saved with the other Hermit settings (the same handle on the next stream)
const char* const kHandleSideKey = "hermit/panelHandleSide";
const char* const kHandleYKey = "hermit/panelHandleY";

}

StreamPanel::StreamPanel(QQmlEngine* engine, SDL_Window* streamWindow,
                         const QString& hostName, const QString& appName)
    : m_Engine(engine),
      m_StreamWindow(streamWindow),
      m_HostName(hostName),
      m_AppName(appName)
{
    StreamingPreferences* prefs = StreamingPreferences::get();
    m_StartWidth = prefs->width;
    m_StartHeight = prefs->height;
    m_StartFps = prefs->fps;
    m_StartBitrateKbps = prefs->bitrateKbps;
    m_StartVideoCodecConfig = static_cast<int>(prefs->videoCodecConfig);
    m_StartEnableHdr = prefs->enableHdr;
    m_StartPrefBitrateKbps = prefs->bitrateKbps;
    m_StreamStartBitrateKbps = prefs->bitrateKbps;
    m_RevertBitrateKbps = prefs->bitrateKbps;
    m_RevertAutoAdjustBitrate = prefs->autoAdjustBitrate;

    QSettings settings;
    m_HandleSide = settings.value(kHandleSideKey, 0).toInt() == 1 ? 1 : 0;
    m_HandleY = qBound(0.0, settings.value(kHandleYKey, 0.5).toDouble(), 1.0);
}

void StreamPanel::setHandleSide(int side)
{
    side = side == 1 ? 1 : 0;
    if (side == m_HandleSide) {
        return;
    }
    m_HandleSide = side;
    QSettings().setValue(kHandleSideKey, side);
    emit handleSideChanged();
    m_LastStreamRect = QRect();
    placeWindows();
}

// The mouse pointer in the coordinates of streamClientRect() (physical pixels on Windows)
QPoint StreamPanel::cursorPos() const
{
#ifdef Q_OS_WIN32
    POINT point;
    if (GetCursorPos(&point)) {
        return QPoint(point.x, point.y);
    }
#endif
    return QCursor::pos();
}

void StreamPanel::beginHandleDrag()
{
    QRect stream = m_LastStreamRect.isValid() ? m_LastStreamRect : streamClientRect();
    int height = qRound(kHandleHeight * streamDpr());
    int travel = qMax(1, stream.height() - height);
    int centre = stream.top() + height / 2 + qRound(m_HandleY * travel);
    m_HandleGrabOffset = cursorPos().y() - centre;
    m_HandleDragging = true;
}

void StreamPanel::dragHandle()
{
    if (!m_HandleDragging) {
        return;
    }
    QRect stream = m_LastStreamRect.isValid() ? m_LastStreamRect : streamClientRect();
    int height = qRound(kHandleHeight * streamDpr());
    int travel = qMax(1, stream.height() - height);
    int centre = cursorPos().y() - m_HandleGrabOffset;
    m_HandleY = qBound(0.0, double(centre - stream.top() - height / 2) / travel, 1.0);
    placeWindows();
}

void StreamPanel::endHandleDrag()
{
    if (m_HandleDragging) {
        m_HandleDragging = false;
        QSettings().setValue(kHandleYKey, m_HandleY);
    }
}

StreamPanel::~StreamPanel()
{
    // The windows were created with a context owned by this object; delete them first.
    delete m_Panel;
    delete m_Handle;
}

QQuickWindow* StreamPanel::createWindow(const char* qmlFile)
{
    if (m_Engine == nullptr) {
        return nullptr;
    }
    QQmlComponent component(m_Engine, QUrl(QStringLiteral("qrc:/gui/") + QLatin1String(qmlFile)));
    auto* context = new QQmlContext(m_Engine->rootContext(), this);
    context->setContextProperty(QStringLiteral("panel"), this);
    QObject* object = component.create(context);
    if (object == nullptr) {
        for (const QQmlError& error : component.errors()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Stream panel: %s",
                        qPrintable(error.toString()));
        }
        return nullptr;
    }
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (window == nullptr) {
        delete object;
        return nullptr;
    }
    return window;
}

bool StreamPanel::post(Action action, intptr_t value)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_STREAM_PANEL_ACTION;
    event.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(action));
    event.user.data2 = reinterpret_cast<void*>(value);
    return SDL_PushEvent(&event) == 1;
}

void StreamPanel::applyLiveBitrate(int kbps)
{
    if (m_LiveBitrateState == 2) {
        return;  // needs a reconnect
    }
    if (m_LiveBitratePending) {
        // One request at a time, so they cannot finish out of order; the latest value follows
        m_QueuedBitrateKbps = kbps;
        return;
    }
    if (kbps == m_StartBitrateKbps && !m_AppliedUncertain) {
        // Nothing to change; a pick of this value is in effect
        if (m_CandidateBitrateKbps != 0 && chosenBitrateFor(m_CandidateBitrateKbps) == kbps) {
            promoteRevertPoint();
        }
        return;
    }
    sendLiveBitrate(kbps);
}

void StreamPanel::setStreamBitrate(int kbps)
{
    m_StreamStartBitrateKbps = kbps;
    m_StartBitrateKbps = kbps;
    emit liveBitrateChanged();
}

int StreamPanel::chosenBitrateKbps() const
{
    return chosenBitrateFor(StreamingPreferences::get()->bitrateKbps);
}

bool StreamPanel::reconnectSettingsChanged() const
{
    // Same test as revertNeeded in StreamPanel.qml
    StreamingPreferences* prefs = StreamingPreferences::get();
    return prefs->width != m_StartWidth || prefs->height != m_StartHeight || prefs->fps != m_StartFps ||
           (m_LiveBitrateState == 2 && chosenBitrateKbps() != m_StartBitrateKbps) ||
           static_cast<int>(prefs->videoCodecConfig) != m_StartVideoCodecConfig ||
           prefs->enableHdr != m_StartEnableHdr;
}

uint32_t StreamPanel::resendLiveBitrate(int kbps)
{
    if (m_LiveBitrateState == 2 || m_LiveBitratePending) {
        return 0;
    }
    sendLiveBitrate(kbps);
    return m_LiveBitratePending ? m_PendingRequestId : 0;
}

void StreamPanel::sendLiveBitrate(int kbps)
{
    static_assert(sizeof(intptr_t) == 8, "the request id and value share one 64-bit event field");
    // Unique across sessions (all panels live on the SDL main thread)
    static uint32_t s_NextRequestId = 0;
    if (++s_NextRequestId == 0) {
        s_NextRequestId = 1;
    }
    m_LiveBitratePending = true;
    m_PendingBitrateKbps = kbps;
    m_PendingRequestId = s_NextRequestId;
    emit liveBitrateChanged();
    if (!post(ActionLiveBitrate, static_cast<intptr_t>((static_cast<uint64_t>(m_PendingRequestId) << 32) | static_cast<uint32_t>(kbps)))) {
        // The event queue refused it: no result will come
        m_LiveBitratePending = false;
        emit liveBitrateChanged();
    }
}

void StreamPanel::chooseBitrate(int kbps)
{
    m_UserPickUnconfirmed = true;
    if (m_LiveBitrateState != 2) {
        post(ActionSavePreferences);
        // Becomes the revert point once it takes effect on the host
        m_CandidateBitrateKbps = kbps;
        m_CandidateAutoAdjustBitrate = StreamingPreferences::get()->autoAdjustBitrate;
        if (chosenBitrateFor(kbps) == m_StartBitrateKbps && !m_LiveBitratePending) {
            promoteRevertPoint();  // the host already streams at it
        }
    }
    applyLiveBitrate(chosenBitrateFor(kbps));
    if (!m_LiveBitratePending && !m_AppliedUncertain && chosenBitrateFor(kbps) == m_StartBitrateKbps) {
        m_UserPickUnconfirmed = false;  // nothing to send: the host already streams at it
    }
}

void StreamPanel::promoteRevertPoint()
{
    if (m_CandidateBitrateKbps != 0) {
        m_RevertBitrateKbps = m_CandidateBitrateKbps;
        m_RevertAutoAdjustBitrate = m_CandidateAutoAdjustBitrate;
        m_CandidateBitrateKbps = 0;
        emit liveBitrateChanged();
    }
}

void StreamPanel::setLiveBitrateResult(uint32_t requestId, int result)
{
    if (!awaitingRequest(requestId)) {
        return;  // not this panel's request (previous session)
    }
    const int requestedKbps = m_PendingBitrateKbps;
    m_LiveBitratePending = false;
    m_AppliedUncertain = result == 0;
    if (result > 0) {
        m_LiveBitrateState = 1;
        m_StartBitrateKbps = requestedKbps;
        if (requestedKbps == chosenBitrateKbps()) {
            m_UserPickUnconfirmed = false;  // the host streams at the user's choice
        }
    }
    else if (result < 0) {
        m_LiveBitrateState = 2;
    }
    if (m_CandidateBitrateKbps != 0 && chosenBitrateFor(m_CandidateBitrateKbps) == m_StartBitrateKbps) {
        promoteRevertPoint();  // the user's pick is what the host streams at
    }
    else if (result < 0) {
        m_CandidateBitrateKbps = 0;  // it will not take effect without reconnecting
    }
    emit liveBitrateChanged();

    // The newer value asked for meanwhile, also after a transient failure (it is a different
    // request, so this is no retry loop); not when the host cannot change it live
    int queued = m_QueuedBitrateKbps;
    m_QueuedBitrateKbps = 0;
    if (queued != 0 && result >= 0) {
        applyLiveBitrate(queued);
    }
}

bool StreamPanel::streamClickedBack(bool streamFocused) const
{
    return m_Open && streamFocused && m_Panel != nullptr && !m_Panel->isActive() &&
           SDL_TICKS_PASSED(SDL_GetTicks(), m_OpenedAt + 1000);
}

void StreamPanel::open()
{
    if (m_Panel == nullptr) {
        m_Panel = createWindow("StreamPanel.qml");
        if (m_Panel == nullptr) {
            return;
        }
    }
    m_Open = true;
    m_OpenedAt = SDL_GetTicks();
    if (m_HandleShown && m_Handle != nullptr) {
        endHandleDrag();
        m_Handle->hide();
    }
    m_HandleShown = false;
    m_Panel->show();
    m_LastStreamRect = QRect();
    placeWindows();
    m_Panel->raise();
    m_Panel->requestActivate();
}

void StreamPanel::close()
{
    m_Open = false;
    if (m_BitrateEditing) {
        // Closed while the slider was held and its release never arrived: the dragged value
        // is still the pick
        m_BitrateEditing = false;
        chooseBitrate(StreamingPreferences::get()->bitrateKbps);
    }
    if (m_Panel != nullptr) {
        m_Panel->hide();
    }
}

bool StreamPanel::lostFocus(bool streamFocused) const
{
    // Activation of the panel arrives a moment after the stream window lost focus to it.
    return m_Open && m_Panel != nullptr && !streamFocused && !m_Panel->isActive() &&
           SDL_TICKS_PASSED(SDL_GetTicks(), m_OpenedAt + 1000);
}

void StreamPanel::setMuted(bool muted)
{
    if (m_Muted != muted) {
        m_Muted = muted;
        emit mutedChanged();
    }
}

QVariantList StreamPanel::aspectResolutions(int display, const QStringList& existing) const
{
    QVariantList result;
    SDL_DisplayMode mode;
    SDL_Rect safeArea;
    if (display < 0 || !StreamUtils::getNativeDesktopMode(display, &mode, &safeArea)) {
        return result;
    }
    const QList<QSize> sizes = ResolutionPresets::withoutNearDuplicates(
        ResolutionPresets::forDisplay(QSize(mode.w, mode.h), true), ResolutionPresets::parseSizes(existing));
    for (const QSize& size : sizes) {
        result.append(size);
    }
    return result;
}

void StreamPanel::sync(bool mouseCaptured, bool absoluteMouse, bool streamFocused, bool minimized)
{
    const int display = SDL_GetWindowDisplayIndex(m_StreamWindow);
    if (display >= 0 && display != m_StreamDisplay) {
        m_StreamDisplay = display;
        emit streamDisplayChanged();
    }

    // The handle is only useful while the mouse can reach it, and must not float over other
    // apps when the stream window is in the background. In remote desktop mouse mode the mouse
    // counts as captured (the cursor is only hidden over the stream window), but the pointer
    // moves freely and reaches the handle: the handle is a separate window that takes the
    // pointer from the stream window, so hovering or clicking it sends nothing to the host, and
    // Qt shows its cursor over it. A click opens the panel, which frees the mouse.
    bool wantHandle = !m_Open && (!mouseCaptured || absoluteMouse) && streamFocused && !minimized;
    if (wantHandle != m_HandleShown && !m_HandleFailed) {
        if (wantHandle && m_Handle == nullptr) {
            m_Handle = createWindow("StreamPanelHandle.qml");
            m_HandleFailed = m_Handle == nullptr;
        }
        if (m_Handle != nullptr) {
            m_HandleShown = wantHandle;
            if (wantHandle) {
                m_Handle->show();
                m_LastStreamRect = QRect();
            }
            else {
                endHandleDrag();
                m_Handle->hide();
            }
        }
    }

    if (needsQtEvents()) {
        // Re-place on a move or resize, and when the stream window reaches a monitor with
        // another DPI (Qt also rescales its windows then)
        QRect rect = streamClientRect();
        qreal dpr = streamDpr();
        if (rect != m_LastStreamRect || dpr != m_LastDpr) {
            m_LastStreamRect = rect;
            m_LastDpr = dpr;
            placeWindows();
        }
    }
}

// The stream window's client area in screen coordinates (physical pixels on Windows).
QRect StreamPanel::streamClientRect() const
{
#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_StreamWindow, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        HWND hwnd = info.info.win.window;
        RECT client;
        POINT topLeft = {0, 0};
        if (GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &topLeft)) {
            return QRect(topLeft.x, topLeft.y, client.right - client.left, client.bottom - client.top);
        }
    }
#endif
    int x, y, width, height;
    SDL_GetWindowPosition(m_StreamWindow, &x, &y);
    SDL_GetWindowSize(m_StreamWindow, &width, &height);
    return QRect(x, y, width, height);
}

// Scale of the stream window's monitor (the panel's sizes are in device-independent pixels)
qreal StreamPanel::streamDpr() const
{
#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_StreamWindow, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        UINT dpi = GetDpiForWindow(info.info.win.window);
        if (dpi != 0) {
            return dpi / 96.0;
        }
    }
#endif
    return 1.0;
}

void StreamPanel::placeWindow(QQuickWindow* window, int x, int y, int width, int height)
{
#ifdef Q_OS_WIN32
    // Physical pixels, like the stream window's rectangle; topmost so it stays above the stream.
    SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
#else
    window->setGeometry(x, y, width, height);
#endif
}

void StreamPanel::placeWindows()
{
    QRect stream = m_LastStreamRect.isValid() ? m_LastStreamRect : streamClientRect();
    qreal dpr = streamDpr();
    bool left = m_HandleSide == 1;
    if (m_Handle != nullptr && m_HandleShown) {
        int width = qRound(kHandleWidth * dpr);
        int height = qMin(qRound(kHandleHeight * dpr), stream.height());
        int y = stream.top() + qRound(m_HandleY * qMax(0, stream.height() - height));
        placeWindow(m_Handle, left ? stream.left() : stream.right() + 1 - width, y, width, height);
    }
    if (m_Panel != nullptr && m_Open) {
        int width = qMin(qRound(kPanelWidth * dpr), stream.width());
        placeWindow(m_Panel, left ? stream.left() : stream.right() + 1 - width, stream.top(), width, stream.height());
    }
}
