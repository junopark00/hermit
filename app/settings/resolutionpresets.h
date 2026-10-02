#pragma once

#include <QList>
#include <QSize>
#include <QStringList>

#include <cmath>
#include <cstdlib>

// Hermit: resolution presets that match the aspect ratio of the client display, so that users of
// displays that are not 16:9 (21:9 ultrawides, 16:10 laptops, 3:2 tablets) do not have to type a
// custom resolution. The Settings page (SystemProperties::getDisplayAspectResolutions()) and the
// stream panel (StreamPanel::aspectResolutions()) offer them next to the standard 16:9 presets and
// leave out those equal or nearly equal to an entry they already have, so a 16:9 display gets
// nothing new.
namespace ResolutionPresets {

// Sizes the stream panel accepts (and within the Settings page's 8192 limit)
constexpr int kMaxWidth = 7680;
constexpr int kMaxHeight = 4320;

// For each standard height (720, 1080, 1440, and 2160 when include2160), the size with the aspect
// ratio of the display: width = height x aspect, used as is when it is an even whole number (a
// 2340x1080 display gives 2340x1080, not 2344x1080), else rounded to a multiple of 8 when that
// keeps the aspect within 1%, otherwise to an even number. On a portrait display the standard values are
// the width and the height follows the aspect. Sizes over 7680x4320 are left out.
inline QList<QSize> forDisplay(const QSize& display, bool include2160)
{
    QList<QSize> result;
    if (display.width() <= 0 || display.height() <= 0) {
        return result;
    }

    const bool portrait = display.height() > display.width();
    // Long side over short side, at least 1
    const double aspect = portrait ? double(display.height()) / display.width() :
                                     double(display.width()) / display.height();

    for (int shortSide : {720, 1080, 1440, 2160}) {
        if (shortSide == 2160 && !include2160) {
            continue;
        }

        const double exact = shortSide * aspect;
        const int even = int(std::lround(exact / 2.0)) * 2;
        int longSide;
        if (std::abs(exact - even) <= 0.01) {
            longSide = even;
        }
        else {
            longSide = int(std::lround(exact / 8.0)) * 8;
            if (std::abs(double(longSide) / shortSide - aspect) > aspect * 0.01) {
                longSide = even;
            }
        }

        const QSize size = portrait ? QSize(shortSide, longSide) : QSize(longSide, shortSide);
        if (size.width() > kMaxWidth || size.height() > kMaxHeight) {
            continue;
        }
        result.append(size);
    }
    return result;
}

// Sizes written as "WIDTHxHEIGHT" (entries that are not sizes are skipped)
inline QList<QSize> parseSizes(const QStringList& texts)
{
    QList<QSize> sizes;
    for (const QString& text : texts) {
        const QStringList parts = text.trimmed().split(QLatin1Char('x'));
        bool okWidth = false, okHeight = false;
        if (parts.size() == 2) {
            const int width = parts[0].trimmed().toInt(&okWidth);
            const int height = parts[1].trimmed().toInt(&okHeight);
            if (okWidth && okHeight && width > 0 && height > 0) {
                sizes.append(QSize(width, height));
            }
        }
    }
    return sizes;
}

// The candidates that are not (nearly) in the list already: a size is left out when an existing
// entry has the same height and a width within 1% of it (a 1360x768 display would otherwise add
// 1272x720 next to 1280x720).
inline QList<QSize> withoutNearDuplicates(const QList<QSize>& candidates, const QList<QSize>& existing)
{
    QList<QSize> result;
    for (const QSize& candidate : candidates) {
        bool near = false;
        for (const QSize& entry : existing) {
            if (entry.height() == candidate.height() &&
                    std::abs(entry.width() - candidate.width()) * 100 <= entry.width()) {
                near = true;
                break;
            }
        }
        if (!near) {
            result.append(candidate);
        }
    }
    return result;
}

// The display a window is on, from the approximate size of its screen in physical pixels (the
// Qt screen size times its scale factor, which can be off by a pixel): the native resolution of
// an attached display that is within 2% of it on both sides, else the approximate size itself.
// Without a usable screen size, the first display (the primary display) is used.
inline QSize matchDisplay(const QList<QSize>& nativeResolutions, const QSize& screen)
{
    if (screen.width() <= 0 || screen.height() <= 0) {
        return nativeResolutions.isEmpty() ? QSize() : nativeResolutions.first();
    }

    QSize best;
    int bestDistance = 0;
    for (const QSize& native : nativeResolutions) {
        const int dw = std::abs(native.width() - screen.width());
        const int dh = std::abs(native.height() - screen.height());
        if (dw * 50 > native.width() || dh * 50 > native.height()) {
            continue;
        }
        if (!best.isValid() || dw + dh < bestDistance) {
            best = native;
            bestDistance = dw + dh;
        }
    }
    return best.isValid() ? best : screen;
}

}
