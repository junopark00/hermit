#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"
#include "streaming/cursorpacket.h"

#include "SDL_compat.h"

#include <QHash>
#include <QList>

struct GamepadState {
    SDL_GameController* controller;
    SDL_JoystickID jsId;
    short index;

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_Haptic* haptic;
    int hapticMethod;
    int hapticEffectId;
#endif

    SDL_TimerID mouseEmulationTimer;
    uint32_t lastStartDownTime;

    bool clickpadButtonEmulationEnabled;
    bool emulatedClickpadButtonDown;

#if SDL_VERSION_ATLEAST(2, 0, 14)
    uint8_t gyroReportPeriodMs;
    float lastGyroEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastGyroEventTime;

    uint8_t accelReportPeriodMs;
    float lastAccelEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastAccelEventTime;
#endif

    int buttons;
    short lsX, lsY;
    short rsX, rsY;
    unsigned char lt, rt;
};


struct DualSenseOutputReport{
    uint8_t validFlag0;
    uint8_t validFlag1;

    /* For DualShock 4 compatibility mode. */
    uint8_t motorRight;
    uint8_t motorLeft;

    /* Audio controls */
    uint8_t reserved[4];
    uint8_t muteButtonLed;

    uint8_t powerSaveControl;
    uint8_t rightTriggerEffectType;
    uint8_t rightTriggerEffect[DS_EFFECT_PAYLOAD_SIZE];
    uint8_t leftTriggerEffectType;
    uint8_t leftTriggerEffect[DS_EFFECT_PAYLOAD_SIZE];
    uint8_t reserved2[6];

    /* LEDs and lightbar */
    uint8_t validFlag2;
    uint8_t reserved3[2];
    uint8_t lightbarSetup;
    uint8_t ledBrightness;
    uint8_t playerLeds;
    uint8_t lightbarRed;
    uint8_t lightbarGreen;
    uint8_t lightbarBlue;
};

// activeGamepadMask is a short, so we're bounded by the number of mask bits
#define MAX_GAMEPADS 16

#define MAX_FINGERS 2

#define GAMEPAD_HAPTIC_METHOD_NONE 0
#define GAMEPAD_HAPTIC_METHOD_LEFTRIGHT 1
#define GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE 2

#define GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT 0.33
#define GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT 0.8

// Hermit: host cursor shapes (cursorpacket.h) and the SDL cursors made from them, for the local
// cursor. Shapes are kept by id (the 128 most recently used, as many as the host remembers having
// sent, within 16 MiB; typical shapes are 4-16 KiB, the largest 256 KiB) so a window resize can
// make a cursor at the new scale; cursors are kept by shape and scale (the newest 32).
// Everything is freed with the input handler, that is when the stream ends.
class LocalCursorCache
{
public:
    ~LocalCursorCache();

    void addShape(const CursorPacket::Shape& shape);

    bool hasShape(uint32_t id) const
    {
        return m_Shapes.contains(id);
    }

    // The SDL cursor of a shape at this scale (physical pixels per shape pixel), made on first
    // use; nullptr when the shape is unknown or the cursor could not be created
    SDL_Cursor* cursor(uint32_t id, double scale);

    void clear();

    static const int k_MaxShapes = 128;
    static const int k_MaxShapeBytes = 16 * 1024 * 1024;
    static const int k_MaxCursors = 32;

private:
    QHash<uint32_t, CursorPacket::Shape> m_Shapes;
    QList<uint32_t> m_ShapeOrder;    // least recently used first
    int m_ShapeBytes = 0;            // pixel bytes of all shapes
    QHash<uint64_t, SDL_Cursor*> m_Cursors;
    QList<uint64_t> m_CursorOrder;   // oldest first
};

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs, int streamWidth, int streamHeight);

    ~SdlInputHandler();

    void setWindow(SDL_Window* window);

    void handleKeyEvent(SDL_KeyboardEvent* event);

    void handleMouseButtonEvent(SDL_MouseButtonEvent* event);

    void handleMouseMotionEvent(SDL_MouseMotionEvent* event);

    void handleMouseWheelEvent(SDL_MouseWheelEvent* event);

    void handleControllerAxisEvent(SDL_ControllerAxisEvent* event);

    void handleControllerButtonEvent(SDL_ControllerButtonEvent* event);

    void handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event);

#if SDL_VERSION_ATLEAST(2, 0, 14)
    void handleControllerSensorEvent(SDL_ControllerSensorEvent* event);

    void handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event);
#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)
    void handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event);
#endif

    void handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event);

    void sendText(QString& string);

    void rumble(uint16_t controllerNumber, uint16_t lowFreqMotor, uint16_t highFreqMotor);

    void rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    void setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    void setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    void setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report);

    void handleTouchFingerEvent(SDL_TouchFingerEvent* event);

    int getAttachedGamepadMask();

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    // Hermit: switch between relative and absolute mouse mode (stream panel); the capture
    // state is left as it is.
    void toggleAbsoluteMouseMode()
    {
        m_AbsoluteMouseMode = !m_AbsoluteMouseMode;
    }

    bool isAbsoluteMouseMode() const { return m_AbsoluteMouseMode; }

    bool isSystemKeyCaptureActive();

    void setCaptureActive(bool active);

    bool isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth = -1, int windowHeight = -1);

    void updateKeyboardGrabState();

    void updatePointerRegionLock();

    // Hermit: local cursor. While active (remote desktop mouse mode, and the Shell host agreed to
    // stop drawing the cursor into the video), the OS cursor inside the video area takes the host
    // cursor's shape and is shown only while the host shows its cursor. Outside the video area
    // and while inactive the cursor behaves as always (hidden inside the video area, the system
    // arrow outside). Ctrl+Alt+Shift+C still forces the system arrow.
    void setLocalCursorActive(bool active);

    bool isLocalCursorActive() const
    {
        return m_LocalCursorActive;
    }

    // A complete shape from the control stream: cached and shown (it is the host's current one)
    void handleCursorShape(const CursorPacket::Shape& shape);

    // A visibility change or a switch to a shape sent earlier (id 0: the default arrow). Returns
    // false when the shape is not in the cache, so the caller fetches it over HTTPS; the
    // previous cursor stays meanwhile.
    bool handleCursorState(const CursorPacket::State& state);

    // A shape fetched over HTTPS: cached, and shown if it is still the one the host last named
    void handleFetchedCursorShape(const CursorPacket::Shape& shape);

    // Whether the host draws the cursor into the video as well (another client streams the
    // desktop without a local cursor), as last reported
    bool localCursorHostComposing() const
    {
        return m_LocalCursorKnown && m_LocalCursorHostComposing;
    }

    // The shape the host last named, and whether it is still missing from the cache (a fetch
    // over HTTPS is wanted)
    uint32_t localCursorWantedId() const
    {
        return m_LocalCursorKnown ? m_LocalCursorWantedId : 0;
    }

    bool localCursorShapeMissing() const
    {
        return m_LocalCursorKnown && m_LocalCursorWantedId != 0 && m_LocalCursorWantedId != m_LocalCursorShapeId;
    }

    // After a window resize: the cursor is made again at the new scale
    void refreshLocalCursor();

    static
    QString getUnmappedGamepads();

private:
    enum KeyCombo {
        KeyComboQuit,
        KeyComboUngrabInput,
        KeyComboToggleFullScreen,
        KeyComboToggleStatsOverlay,
        KeyComboToggleMouseMode,
        KeyComboToggleCursorHide,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboQuitAndExit,
        KeyComboToggleKeyboardGrab,
        KeyComboCancelClipboardTransfer,
        KeyComboToggleStreamPanel,
        KeyComboToggleHotkeyHelp,
        KeyComboMax
    };

    GamepadState*
    findStateForGamepad(SDL_JoystickID id);

    void sendGamepadState(GamepadState* state);

    void sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level);

    void handleAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void emulateAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void disableTouchFeedback();

    void handleRelativeFingerEvent(SDL_TouchFingerEvent* event);

    void performSpecialKeyCombo(KeyCombo combo);

    static
    Uint32 longPressTimerCallback(Uint32 interval, void* param);

    static
    Uint32 mouseEmulationTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseLeftButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseRightButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 dragTimerCallback(Uint32 interval, void* param);

    // Hermit: local cursor helpers (mouse.cpp). The cursor to show inside the video area while
    // the mouse is captured, and whether it is shown at all.
    void showCapturedCursor();
    int capturedCursorVisibility() const;
    double localCursorScale() const;

    SDL_Window* m_Window;
    bool m_MultiController;
    bool m_GamepadMouse;
    bool m_SwapMouseButtons;
    bool m_ReverseScrollDirection;
    bool m_SwapFaceButtons;

    bool m_NeedsManualCaptureOnLeave;
    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;

    int m_GamepadMask;
    GamepadState m_GamepadState[MAX_GAMEPADS];
    QSet<uint32_t> m_KeysDown;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    QString m_OldIgnoreDevices;
    QString m_OldIgnoreDevicesExcept;
    QStringList m_IgnoreDeviceGuids;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    int m_MouseCursorCapturedVisibilityState;

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    SDL_TouchFingerEvent m_LastTouchDownEvent;
    SDL_TouchFingerEvent m_LastTouchUpEvent;
    SDL_TimerID m_LongPressTimer;
    int m_StreamWidth;
    int m_StreamHeight;
    bool m_AbsoluteMouseMode;
    bool m_AbsoluteTouchMode;
    bool m_DisabledTouchFeedback;

    SDL_TouchFingerEvent m_TouchDownEvent[MAX_FINGERS];
    SDL_TimerID m_LeftButtonReleaseTimer;
    SDL_TimerID m_RightButtonReleaseTimer;
    SDL_TimerID m_DragTimer;
    char m_DragButton;
    int m_NumFingersDown;

    // Hermit: local cursor state
    LocalCursorCache m_LocalCursors;
    bool m_LocalCursorActive;
    bool m_LocalCursorKnown;      // a shape or state has arrived since the stream started
    uint32_t m_LocalCursorShapeId;   // the shape shown (0: the default arrow)
    uint32_t m_LocalCursorWantedId;  // the shape the host last named (not yet in the cache)
    bool m_LocalCursorVisible;
    bool m_LocalCursorHostComposing;

    static const int k_ButtonMap[];
};
