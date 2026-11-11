/************************************************************************

    segmentchapters.h

    tbc-segments - Segments as export chapters
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

#ifndef SEGMENTCHAPTERS_H
#define SEGMENTCHAPTERS_H

#include <QString>
#include <QVector>

#include "shotboundaries.h"
#include "tbcmetadata.h"

/*!
    Which stored segments become chapters.
*/
enum class FfmetadataSegmentMode {
    EnabledSegments,   // one chapter per enabled stored segment (the default)
    AllSegments,       // every stored segment, disabled ones included
    NoSegments         // ignore stored segments: LaserDisc navigation chapters only
};

struct SegmentChapter {
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
    QString title;
    QString comment;
};

/*!
    The stored segments selected by mode, in field order, as chapters over the
    whole file (the caller limits them to its export range). A segment without
    a title is "Segment <id>". Empty when the metadata holds no segments or
    mode is NoSegments: the caller then falls back to its own chapters.
*/
QVector<SegmentChapter> segmentChapters(const TbcMetaData &metaData, FfmetadataSegmentMode mode);

// Glossary: chapters by (docs/Tools/tbc-segments-glossary.md#chapters-by)
enum class ChaptersBy {
    Segment,   // one chapter per selected segment (the default)
    Scene,     // one chapter per scene inside each content span
    Shot       // one chapter per shot inside each content span
};

// "segment" / "scene" / "shot", as on the command line; an empty name is
// Segment. Anything else is Segment too, with *ok set false when ok is given.
ChaptersBy chaptersByFromName(const QString &name, bool *ok = nullptr);

/*!
    Chapters by scene or by shot: each content span chapter is replaced by its
    scenes or shots (sceneshots.h), titled with their titles and filtered by
    their own enabled flags under EnabledSegments. Other chapters (noise,
    blank, unknown under AllSegments) stay as they are. by = Segment returns
    the chapters unchanged.
*/
QVector<SegmentChapter> chaptersBy(const TbcMetaData &metaData, FfmetadataSegmentMode mode, ChaptersBy by,
                                   const QVector<ShotBoundary> &boundaries);

/*!
    The shot boundaries an export uses: those stored in the .tbc.db beside
    metadataPath, else those in its sidecar (ShotBoundaryStore::writeSidecar,
    for a snapshot with no .tbc.db), else a detection over the whole file.
*/
QVector<ShotBoundary> shotBoundariesForExport(const TbcMetaData &metaData, const QString &metadataPath);

#endif // SEGMENTCHAPTERS_H
