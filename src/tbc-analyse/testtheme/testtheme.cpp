/******************************************************************************
 * testtheme.cpp
 * tbc-analyse - theme unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Reece Dodge
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>
#include <QApplication>
#include <QPalette>
#include <QStyle>

#include "theme_color_tokens.h"
#include "tbc/uistyle.h"

// Release builds define NDEBUG, so a bare assert() would check nothing.
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "     \
                      << #condition << "\n";                                \
            std::exit(1);                                                   \
        }                                                                   \
    } while (false)

namespace {

// Qt 6.10's own Fusion palette, as qt_fusionPalette() in
// qtbase/src/gui/kernel/qplatformtheme.cpp builds it for each colour scheme.
// The offscreen platform the tests run on reports no scheme, so the dark
// palette can't be had from Qt here; the roles the tokens read are rebuilt.
QPalette fusionPalette(bool dark)
{
    const QColor windowText = dark ? QColor(240, 240, 240) : QColor(Qt::black);
    const QColor background = dark ? QColor(50, 50, 50) : QColor(239, 239, 239);
    const QColor base = dark ? background.darker(140) : QColor(Qt::white);
    QPalette palette(windowText, background, background.lighter(150), background.darker(150),
                     background.darker(130), windowText, base);
    return palette;
}

qreal lightnessGap(const QColor &first, const QColor &second)
{
    return qAbs(first.lightnessF() - second.lightnessF());
}

// Plot grids are drawn on a Base-filled canvas and must stand out from it in
// both schemes. QPalette::Mid, the old grid colour, is a bevel role: in Qt's
// dark Fusion palette it is 38 on a Base of 36, which is the regression.
void testGridLineVisibleOnBase()
{
    std::cerr << "Testing that grid lines stand out from the canvas\n";

    for (const bool dark : {true, false}) {
        const QPalette palette = fusionPalette(dark);
        CHECK(lightnessGap(theme_tokens::gridLine(palette), palette.color(QPalette::Base)) >= 0.1);
    }
    const QPalette dark = fusionPalette(true);
    CHECK(lightnessGap(dark.color(QPalette::Mid), dark.color(QPalette::Base)) < 0.1);
}

void testIsDarkPalette()
{
    std::cerr << "Testing dark-palette detection\n";

    CHECK(theme_tokens::isDarkPalette(fusionPalette(true)));
    CHECK(!theme_tokens::isDarkPalette(fusionPalette(false)));
}

// applyFusionTheme() is Qt's own theme: the Fusion style and a colour-scheme
// request, never an explicit palette. An explicitly set palette would stop
// later scheme changes from reaching the widgets.
void testApplyFusionThemeSetsNoPalette()
{
    std::cerr << "Testing that applyFusionTheme sets Fusion and no palette\n";

    for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Dark, Qt::ColorScheme::Light, Qt::ColorScheme::Unknown}) {
        tbc::ui::applyFusionTheme(scheme);
        CHECK(QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0);
        CHECK(QApplication::palette().resolveMask() == 0);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    testGridLineVisibleOnBase();
    testIsDarkPalette();
    testApplyFusionThemeSetsNoPalette();

    std::cerr << "All theme tests passed\n";
    return 0;
}
