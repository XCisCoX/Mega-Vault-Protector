// Geometry regression for the Telegram-style photo chrome.
// Values come from Telegram Desktop 7.3.1 media/view/media_view.style and
// media_view_overlay_widget.cpp::updateControls (see telegram_photo_chrome.hpp).
#include "videovault/app/telegram_photo_chrome.hpp"

#include <cstdio>
#include <cstdlib>

namespace photo = videovault::app::telegram_photo;

namespace {

int failures = 0;

void check(const bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    check(photo::cellWidth == 46 && photo::cellHeight == 54,
        "icon cell matches mediaviewIconSize 46x54");
    check(photo::hoverDiameter == 36, "hover circle matches mediaviewIconOver 36");
    check(photo::navWidth == 90, "edge strip matches mediaviewControlSize 90");
    check(photo::navTop == 47, "edge strip starts at mediaviewHeaderTop 47");
    check(photo::footerLeft == 14 && photo::footerBottom == 26,
        "name footer matches mediaviewTextLeft 14 / mediaviewTextTop 26");

    const QSize viewport(960, 620);
    const QRect more = photo::cell(1, viewport);
    const QRect rotate = photo::cell(2, viewport);
    check(more == QRect(960 - 46, 620 - 54, 46, 54), "more cell hugs the bottom-right corner");
    check(rotate.x() == more.x() - 46, "cells are laid right to left with no gap");
    check(photo::hoverCircle(more).center() == more.center(), "hover circle is centred in its cell");
    check(photo::hoverCircle(more).width() == 36, "hover circle is 36px");

    const QRect leftNav = photo::navStrip(false, viewport);
    const QRect rightNav = photo::navStrip(true, viewport);
    check(leftNav.x() == 0 && leftNav.width() == 90, "left strip is flush with the edge");
    check(rightNav.right() == viewport.width() - 1, "right strip is flush with the edge");
    check(leftNav.y() == 47 && leftNav.height() == 620 - 94,
        "edge strips leave the header skip above and below");
    check(photo::navCentre(false, viewport) == QPoint(45, 47 + 263), "left glyph is centred in its strip");
    check(photo::navCentre(true, viewport) == QPoint(960 - 45, 47 + 263), "right glyph is centred");

    const QRect footer = photo::footer(viewport, 120, 16);
    check(footer.left() == 14, "footer starts 14px from the left");
    check(footer.bottom() == 620 - 26 - 1 + 0, "footer sits 26px above the bottom");
    check(footer.width() == 120, "footer takes the measured text width");

    // Never overrun the icon row or the viewport on a small window.
    const QSize small(320, 240);
    const QRect smallFooter = photo::footer(small, 500, 16);
    check(smallFooter.right() < photo::cell(2, small).x(), "long names are elided before the icons");
    check(smallFooter.bottom() < small.height(), "footer stays inside the picture");

    // Degenerate sizes must not produce negative rects.
    const QSize tiny(80, 60);
    check(photo::navStrip(false, tiny).height() >= 0, "nav strip height never goes negative");
    check(photo::footer(tiny, 0, 16).width() >= 0, "footer width never goes negative");
    check(photo::cell(0, tiny) == photo::cell(1, tiny), "cell index is clamped to the rightmost slot");

    if (failures != 0) {
        std::fprintf(stderr, "%d photo chrome geometry checks failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("Photo chrome: source geometry, containment and clamping checks passed.\n");
    return EXIT_SUCCESS;
}
