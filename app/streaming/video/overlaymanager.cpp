#include "overlaymanager.h"
#include "path.h"
#include "gui/hermittheme.h"
#include "streaming/session.h"
#include "statsoverlay.h"

#include <QStringList>
#include <QVector>

namespace {

// Neutral white for notices; connection problems keep upstream's red.
const SDL_Color k_ToastColor = {0xF4, 0xF4, 0xF4, 0xFF};

// Shared by all OverlayManager instances so a late timer never hides a newer session's toast.
SDL_atomic_t s_NextToastSerial;

// Locks the overlay mutex: both overlays are updated from several threads (the decoder thread
// writes the performance overlay while the main thread toggles it; status messages come from
// the connection, clipboard and timer threads) and share TTF fonts that are not thread-safe.
class StatusGuard
{
public:
    StatusGuard(SDL_mutex* lock, bool active) : m_Lock(active ? lock : nullptr)
    {
        if (m_Lock != nullptr) {
            SDL_LockMutex(m_Lock);
        }
    }
    ~StatusGuard()
    {
        if (m_Lock != nullptr) {
            SDL_UnlockMutex(m_Lock);
        }
    }

private:
    SDL_mutex* m_Lock;
};

Uint32 SDLCALL hideToastCallback(Uint32, void* param)
{
    Session* session = Session::get();
    if (session != nullptr) {
        session->getOverlayManager().hideToast((int)(intptr_t)param);
    }
    return 0;
}

}

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;
    m_StatusColor = m_Overlays[OverlayType::OverlayStatusUpdate].color;
    SDL_AtomicSet(&m_ToastActive, 0);
    SDL_AtomicSet(&m_ToastSerial, 0);
    m_ToastTimer = 0;
    m_WarningCovered = false;
    m_CoveredWarning[0] = 0;
    m_StatusLock = SDL_CreateMutex();
    m_PanelLabelFont = nullptr;
    m_RowToastFont = nullptr;
    m_RowToastFontSize = 0;
    m_StatsMetrics = StatsOverlay::kDefaultMetrics;
    m_PanelLabelFontData = HermitTheme::overlayLabelFontData();

    // ModeSeven has no Hangul, so status messages (toasts, translated warnings) use IBM Plex Sans KR.
    m_StatusFontData = HermitTheme::overlayFontData();
    if (!m_StatusFontData.isEmpty()) {
        m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 30;
    }

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    if (m_ToastTimer != 0) {
        SDL_RemoveTimer(m_ToastTimer);
    }

    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }
    if (m_PanelLabelFont != nullptr) {
        TTF_CloseFont(m_PanelLabelFont);
    }
    if (m_RowToastFont != nullptr) {
        TTF_CloseFont(m_RowToastFont);
    }

    TTF_Quit();

    if (m_StatusLock != nullptr) {
        SDL_DestroyMutex(m_StatusLock);
    }

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    StatusGuard guard(m_StatusLock, true);

    // A connection status message replaces any toast and gets its warning colour back.
    if (type == OverlayType::OverlayStatusUpdate && SDL_AtomicSet(&m_ToastActive, 0) != 0) {
        m_Overlays[type].color = m_StatusColor;
    }
    if (type == OverlayType::OverlayStatusUpdate) {
        m_WarningCovered = false;  // the new message replaces a covered one
    }
    SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    setOverlayTextUpdated(type);
}

int OverlayManager::showToast(const QString& text, int durationMs, bool coverWarning)
{
    const OverlayType type = OverlayType::OverlayStatusUpdate;
    StatusGuard guard(m_StatusLock, true);

    // Do not cover a connection warning that is currently shown, unless asked to: then keep
    // it to show again when the toast ends.
    if (m_Overlays[type].enabled && SDL_AtomicGet(&m_ToastActive) == 0) {
        if (!coverWarning) {
            return 0;
        }
        SDL_utf8strlcpy(m_CoveredWarning, m_Overlays[type].text, sizeof(m_CoveredWarning));
        m_WarningCovered = true;
    }

    const int serial = SDL_AtomicAdd(&s_NextToastSerial, 1) + 1;
    SDL_AtomicSet(&m_ToastSerial, serial);
    SDL_AtomicSet(&m_ToastActive, 1);
    m_Overlays[type].color = k_ToastColor;
    SDL_utf8strlcpy(m_Overlays[type].text, text.toUtf8().constData(), sizeof(m_Overlays[0].text));
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
    else {
        setOverlayState(type, true);
    }

    if (m_ToastTimer != 0) {
        SDL_RemoveTimer(m_ToastTimer);
    }
    m_ToastTimer = SDL_AddTimer((Uint32)durationMs, hideToastCallback, (void*)(intptr_t)serial);
    return serial;
}

bool OverlayManager::isToastShowing(int serial)
{
    return serial != 0 && SDL_AtomicGet(&m_ToastActive) != 0 && SDL_AtomicGet(&m_ToastSerial) == serial;
}

void OverlayManager::hideToast(int serial)
{
    StatusGuard guard(m_StatusLock, true);
    if (SDL_AtomicGet(&m_ToastSerial) != serial || SDL_AtomicCAS(&m_ToastActive, 1, 0) == SDL_FALSE) {
        return;
    }
    const OverlayType type = OverlayType::OverlayStatusUpdate;
    m_Overlays[type].color = m_StatusColor;
    if (m_WarningCovered) {
        // The connection warning the toast covered is still current
        m_WarningCovered = false;
        SDL_utf8strlcpy(m_Overlays[type].text, m_CoveredWarning, sizeof(m_Overlays[0].text));
        setOverlayTextUpdated(type);
        return;
    }
    setOverlayState(type, false);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    StatusGuard guard(m_StatusLock, true);

    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    StatusGuard guard(m_StatusLock, true);

    if (type == OverlayType::OverlayStatusUpdate && !enabled && m_WarningCovered &&
            SDL_AtomicGet(&m_ToastActive) != 0) {
        // The warning under a toast ended (connection okay again): the toast stays
        m_WarningCovered = false;
        return;
    }

    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    StatusGuard guard(m_StatusLock, true);

    if (m_Renderer == nullptr) {
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // The font data must stay around until the font is closed. Status messages and the
        // performance overlay use IBM Plex Sans KR (Hangul); ModeSeven is the fallback.
        const QByteArray& fontData = !m_StatusFontData.isEmpty() ?
                                         m_StatusFontData : m_FontData;
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(fontData.constData(), fontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    SDL_Surface* newSurface = nullptr;
    if (m_Overlays[type].enabled) {
        if (type == OverlayType::OverlayDebug && SDL_strchr(m_Overlays[type].text, '\t') != nullptr) {
            if (m_PanelLabelFont == nullptr && !m_PanelLabelFontData.isEmpty()) {
                m_PanelLabelFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_PanelLabelFontData.constData(), m_PanelLabelFontData.size()),
                                                  1, m_Overlays[type].fontSize);
            }
            newSurface = renderStatsPanel(m_Overlays[type].font,
                                          m_PanelLabelFont != nullptr ? m_PanelLabelFont : m_Overlays[type].font,
                                          m_Overlays[type].text);
        }
        else if (type == OverlayType::OverlayStatusUpdate && SDL_strchr(m_Overlays[type].text, '\t') != nullptr) {
            // Hermit: toasts made of rows (the keyboard shortcut list), at the performance
            // overlay's size so the list fits in small windows
            const int rowSize = m_Overlays[OverlayType::OverlayDebug].fontSize;
            if (m_RowToastFont != nullptr && m_RowToastFontSize != rowSize) {
                TTF_CloseFont(m_RowToastFont);
                m_RowToastFont = nullptr;
            }
            if (m_RowToastFont == nullptr) {
                const QByteArray& rowFontData = m_StatusFontData.isEmpty() ? m_FontData : m_StatusFontData;
                m_RowToastFont = TTF_OpenFontRW(SDL_RWFromConstMem(rowFontData.constData(), rowFontData.size()), 1, rowSize);
                m_RowToastFontSize = rowSize;
            }
            TTF_Font* rowFont = m_RowToastFont != nullptr ? m_RowToastFont : m_Overlays[type].font;
            newSurface = renderStatsPanel(rowFont, rowFont, m_Overlays[type].text);
        }
        else {
            // The _Wrapped variant is required for line breaks to work
            newSurface = RenderTextOutlinedWrapped(m_Overlays[type].font,
                                                   m_Overlays[type].text,
                                                   m_Overlays[type].color,
                                                   {0, 0, 0, 255},
                                                   4,
                                                   1024);
        }
    }

    // Exchange the old surface with the new one
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, newSurface);

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

void OverlayManager::setStatsOptions(int metrics, int fontSize)
{
    m_StatsMetrics = metrics;
    if (fontSize > 0 && fontSize != m_Overlays[OverlayType::OverlayDebug].fontSize) {
        m_Overlays[OverlayType::OverlayDebug].fontSize = fontSize;
        // Reopen the fonts at the new size on the next update.
        if (m_Overlays[OverlayType::OverlayDebug].font != nullptr) {
            TTF_CloseFont(m_Overlays[OverlayType::OverlayDebug].font);
            m_Overlays[OverlayType::OverlayDebug].font = nullptr;
        }
        if (m_PanelLabelFont != nullptr) {
            TTF_CloseFont(m_PanelLabelFont);
            m_PanelLabelFont = nullptr;
        }
    }
}

SDL_Surface* OverlayManager::renderStatsPanel(TTF_Font* valueFont, TTF_Font* labelFont, const char* text)
{
    // Hermit's dark theme: layer #161616 at ~85% opacity, border #393939, helper text #A8A8A8 for
    // labels and primary text #F4F4F4 for values.
    const SDL_Color labelColor = {0xA8, 0xA8, 0xA8, 0xFF};
    const SDL_Color valueColor = {0xF4, 0xF4, 0xF4, 0xFF};

    struct Row {
        SDL_Surface* label;
        SDL_Surface* value;
    };
    QVector<Row> rows;
    int labelWidth = 0;
    int valueWidth = 0;
    for (const QString& line : QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int tab = line.indexOf(QLatin1Char('\t'));
        const QByteArray label = (tab >= 0 ? line.left(tab) : line).toUtf8();
        const QByteArray value = (tab >= 0 ? line.mid(tab + 1) : QString()).toUtf8();
        Row row = { label.isEmpty() ? nullptr : TTF_RenderUTF8_Blended(labelFont, label.constData(), labelColor),
                    value.isEmpty() ? nullptr : TTF_RenderUTF8_Blended(valueFont, value.constData(), valueColor) };
        if (row.label != nullptr) {
            labelWidth = SDL_max(labelWidth, row.label->w);
        }
        if (row.value != nullptr) {
            valueWidth = SDL_max(valueWidth, row.value->w);
        }
        rows.append(row);
    }
    if (rows.isEmpty()) {
        return nullptr;
    }

    const int lineHeight = TTF_FontLineSkip(valueFont);
    const int padding = SDL_max(8, lineHeight / 2);
    const int gap = lineHeight;
    const int width = padding * 2 + labelWidth + gap + valueWidth;
    const int height = padding * 2 + lineHeight * (int)rows.size();

    SDL_Surface* panel = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (panel != nullptr) {
        SDL_FillRect(panel, nullptr, SDL_MapRGBA(panel->format, 0x39, 0x39, 0x39, 0xE6));
        SDL_Rect inner = { 1, 1, width - 2, height - 2 };
        SDL_FillRect(panel, &inner, SDL_MapRGBA(panel->format, 0x16, 0x16, 0x16, 0xD9));

        int y = padding;
        for (const Row& row : rows) {
            if (row.label != nullptr) {
                SDL_Rect dst = { padding, y + (lineHeight - row.label->h) / 2, row.label->w, row.label->h };
                SDL_BlitSurface(row.label, nullptr, panel, &dst);
            }
            if (row.value != nullptr) {
                // Values are right-aligned so digits stay in place as they change.
                SDL_Rect dst = { width - padding - row.value->w, y + (lineHeight - row.value->h) / 2, row.value->w, row.value->h };
                SDL_BlitSurface(row.value, nullptr, panel, &dst);
            }
            y += lineHeight;
        }
    }

    for (const Row& row : rows) {
        SDL_FreeSurface(row.label);
        SDL_FreeSurface(row.value);
    }
    return panel;
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}


