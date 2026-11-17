/************************************************************************

    segmentpreferences.h

    tbc-segments - tbc-analyse's segment preferences
    Copyright (C) 2026 GDH-Technologies LLC

    This file is part of tbc-tools.

    tbc-segments is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/


#ifndef SEGMENTPREFERENCES_H
#define SEGMENTPREFERENCES_H

#include <QString>

// The module's own settings, kept in tbc-analyse's ini under [segments] so
// they need nothing from its Configuration class.
namespace SegmentPreferences {

// Glossary: chapters by (docs/Tools/tbc-segments-glossary.md#chapters-by):
// "segment" (the default), "scene" or "shot"
QString chaptersBy();
void setChaptersBy(const QString &chaptersBy);

} // namespace SegmentPreferences

#endif // SEGMENTPREFERENCES_H
