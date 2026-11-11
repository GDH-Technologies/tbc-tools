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

// The module's own settings, kept in tbc-analyse's ini under [segments] so
// they need nothing from its Configuration class. Both default off.
namespace SegmentPreferences {

// Newly derived restart marks split their clip into segments
bool splitAtRestarts();
void setSplitAtRestarts(bool enabled);

// Video exports cut each segment's chapter at its restart marks
// (tbc-video-export --export-metadata-restart-chapters)
bool restartChapters();
void setRestartChapters(bool enabled);

} // namespace SegmentPreferences

#endif // SEGMENTPREFERENCES_H
