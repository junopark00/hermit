#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

    // Shows a short notice in the status overlay (in a neutral colour) and hides it after
    // durationMs, unless a connection status message replaced it meanwhile. May be called from
    // any thread: all status overlay updates are serialized by m_StatusLock.
    // Returns the toast's serial (for hideToast / isToastShowing), 0 if a connection warning
    // kept it from showing. Text with tabs is drawn as label/value rows on a panel, like the
    // performance overlay. With coverWarning (the shortcut list, asked for by the user) it
    // shows over a connection warning, which comes back when the toast ends.
    int showToast(const QString& text, int durationMs = 3500, bool coverWarning = false);
    void hideToast(int serial);
    bool isToastShowing(int serial);

    // Performance overlay options from Settings: metric bits (StatsOverlay::Metric) and text size
    // in pixels. Call before the stream starts.
    void setStatsOptions(int metrics, int fontSize);
    int statsMetrics() const
    {
        return m_StatsMetrics;
    }

private:
    void notifyOverlayUpdated(OverlayType type);
    // Draws "label<TAB>value" lines as a two-column panel on a translucent dark background.
    SDL_Surface* renderStatsPanel(TTF_Font* valueFont, TTF_Font* labelFont, const char* text);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;

    // Hangul-capable font for the status overlay (toasts and connection messages).
    QByteArray m_StatusFontData;
    SDL_Color m_StatusColor;
    SDL_atomic_t m_ToastActive;
    SDL_atomic_t m_ToastSerial;
    SDL_TimerID m_ToastTimer;
    // A connection warning under the current toast (coverWarning), restored when it ends
    bool m_WarningCovered;
    char m_CoveredWarning[1024];

    // Both overlays are updated from several threads (decoder thread, connection status
    // callbacks, clipboard worker, toast timer, main loop) and TTF fonts are not thread-safe,
    // so every update and render takes this lock. SDL mutexes are recursive.
    SDL_mutex* m_StatusLock;

    // Performance overlay: labels use the regular weight, values the status font.
    QByteArray m_PanelLabelFontData;
    TTF_Font* m_PanelLabelFont;
    int m_StatsMetrics;

    // Toasts made of rows (the keyboard shortcut list), at the performance overlay's size
    TTF_Font* m_RowToastFont;
    int m_RowToastFontSize;
};

}
