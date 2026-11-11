/************************************************************************

    markstore.h

    tbc-segments - Restart marks in the .tbc.db (the module's own tables)
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

#ifndef MARKSTORE_H
#define MARKSTORE_H

#include <QString>
#include <QVector>

#include "restartmarks.h"

/*!
    Marks live in the .tbc.db in tables this module owns: segment_mark (one
    row per mark) and tbc_segments_meta (key/value; marks_schema = 1). They
    are not part of the tbc library's schema or its user_version, and the
    .tbc.json never carries them. A tool that rewrites the whole database
    (tbc-analyse's Save writes a new file) drops them, so whoever holds marks
    writes them back after such a save.

    Each call opens its own connection and closes it before returning, so it
    never overlaps the metadata writer. WAL is never used: these files live
    on network filesystems.
*/
namespace MarkStore {

// The .tbc.db a metadata path stands for (its sibling for a .tbc.json), or
// an empty string when there is none.
QString databasePathFor(const QString &metadataPath);

// The stored marks in field order (empty when there are none, or no table).
// *hasTable reports whether the database holds the segment_mark table at all.
bool read(const QString &databasePath, QVector<SegmentMark> *marks, bool *hasTable = nullptr, QString *error = nullptr);

// Replace the stored marks with these (creating the tables when needed).
// Writes nothing, and returns true, when the stored marks already equal them.
// *changed reports whether anything was written.
bool write(const QString &databasePath, const QVector<SegmentMark> &marks, bool *changed = nullptr, QString *error = nullptr);

} // namespace MarkStore

#endif // MARKSTORE_H
