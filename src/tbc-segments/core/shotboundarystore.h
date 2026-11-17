/************************************************************************

    shotboundarystore.h

    tbc-segments - Shot boundaries in the .tbc.db (the module's own tables)
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

#ifndef SHOTBOUNDARYSTORE_H
#define SHOTBOUNDARYSTORE_H

#include <QString>
#include <QVector>

#include "shotboundaries.h"

/*!
    Shot boundaries (Glossary: shot boundary, docs/Tools/tbc-segments-glossary.md#shot-boundary)
    live in the .tbc.db in tables this module owns: shot_boundary (one row
    per boundary) and tbc_segments_meta (key/value; shot_boundary_schema = 1).
    They are not part of the tbc library's schema or its user_version, and
    the .tbc.json never carries them. A tool that rewrites the whole database
    (tbc-analyse's Save writes a new file) drops them, so whoever holds them
    writes them back after such a save.

    Each call opens its own connection and closes it before returning, so it
    never overlaps the metadata writer. WAL is never used: these files live
    on network filesystems.
*/
namespace ShotBoundaryStore {

// The .tbc.db a metadata path stands for (its sibling for a .tbc.json), or
// an empty string when there is none.
QString databasePathFor(const QString &metadataPath);

// The stored shot boundaries in field order (empty when there are none, or no
// table). *hasTable reports whether the database holds the table at all.
bool read(const QString &databasePath, QVector<ShotBoundary> *boundaries, bool *hasTable = nullptr, QString *error = nullptr);

// Replace the stored shot boundaries with these (creating the tables when
// needed). Writes nothing, and returns true, when the stored ones already
// equal them. *changed reports whether anything was written.
bool write(const QString &databasePath, const QVector<ShotBoundary> &boundaries, bool *changed = nullptr, QString *error = nullptr);

// A sidecar beside a metadata snapshot that has no .tbc.db (tbc-analyse's
// export snapshot): <jsonPath>.shot-boundaries.json, so tbc-export-metadata
// sees the boundaries the person edited, unsaved ones included.
QString sidecarPathFor(const QString &jsonPath);
bool writeSidecar(const QString &jsonPath, const QVector<ShotBoundary> &boundaries, QString *error = nullptr);
bool readSidecar(const QString &jsonPath, QVector<ShotBoundary> *boundaries);

} // namespace ShotBoundaryStore

#endif // SHOTBOUNDARYSTORE_H
