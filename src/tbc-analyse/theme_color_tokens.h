/*
 * File:        theme_color_tokens.h
 * Module:      orc-gui
 * Purpose:     Shared color tokens for theme-aware custom painting
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QColor>
#include <QPalette>
#include <QtGlobal>

namespace theme_tokens {

enum class PlotColorToken {
    LumaPrimary,
    ChromaPrimary,
    LumaSecondary,
    ChromaSecondary,
    CompositePrimary,
    CompositeSecondary,
    RegionBurst,
    RegionActiveVideo,
    MarkerSelection,
    FieldBoundary
};

inline QColor blend(const QColor &from, const QColor &to, qreal ratio)
{
    const qreal clamped = qBound<qreal>(0.0, ratio, 1.0);
    return QColor::fromRgbF(
        from.redF() + (to.redF() - from.redF()) * clamped,
        from.greenF() + (to.greenF() - from.greenF()) * clamped,
        from.blueF() + (to.blueF() - from.blueF()) * clamped,
        from.alphaF() + (to.alphaF() - from.alphaF()) * clamped
    );
}

inline QColor mutedText(const QPalette &palette)
{
    return palette.color(QPalette::Disabled, QPalette::WindowText);
}

// Grid lines sit on a Base-filled canvas, so they are a step from Base toward
// Text. Not QPalette::Mid: that is a bevel role, and in Qt's dark Fusion
// palette it is almost exactly Base (38 on 36), so the grid would vanish.
inline QColor gridLine(const QPalette &palette)
{
    return blend(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.2);
}

// Whether a palette is a dark one: its window background is darker than the
// text drawn on it. For when QStyleHints::colorScheme() is Unknown.
inline bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightnessF() < palette.color(QPalette::WindowText).lightnessF();
}

inline QColor neutralLine(const QPalette &palette, qreal emphasis)
{
    return blend(palette.color(QPalette::Window), palette.color(QPalette::WindowText), emphasis);
}

inline QColor plotColor(PlotColorToken token, bool darkTheme)
{
    switch (token) {
    case PlotColorToken::LumaPrimary:
        return darkTheme ? QColor(255, 255, 100) : QColor(200, 180, 0);
    case PlotColorToken::ChromaPrimary:
        return darkTheme ? QColor(100, 150, 255) : QColor(0, 80, 200);
    case PlotColorToken::LumaSecondary:
        return darkTheme ? QColor(255, 255, 180) : QColor(230, 210, 40);
    case PlotColorToken::ChromaSecondary:
        return darkTheme ? QColor(160, 190, 255) : QColor(80, 120, 220);
    case PlotColorToken::CompositePrimary:
        return darkTheme ? QColor(100, 200, 255) : QColor(0, 100, 200);
    case PlotColorToken::CompositeSecondary:
        return darkTheme ? QColor(255, 255, 100) : QColor(200, 180, 0);
    case PlotColorToken::RegionBurst:
        return darkTheme ? QColor(0, 255, 255) : QColor(0, 140, 160);
    case PlotColorToken::RegionActiveVideo:
    case PlotColorToken::FieldBoundary:
        return plotColor(PlotColorToken::LumaPrimary, darkTheme);
    case PlotColorToken::MarkerSelection:
        return darkTheme ? QColor(0, 255, 0) : QColor(0, 150, 0);
    }

    return darkTheme ? QColor(255, 255, 255) : QColor(0, 0, 0);
}

} // namespace theme_tokens
