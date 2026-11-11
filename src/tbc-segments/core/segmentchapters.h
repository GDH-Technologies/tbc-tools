/************************************************************************

    segmentchapters.h

    tbc-segments - Recording segments as export chapters
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

#include "restartmarks.h"
#include "tbcmetadata.h"

/*!
    Which stored recording segments become chapters.
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
    whole file (the caller clips them to its export range). A segment without
    a title is "Segment <id>". Empty when the metadata holds no segments or
    mode is NoSegments: the caller then falls back to its own chapters.
*/
QVector<SegmentChapter> segmentChapters(const TbcMetaData &metaData, FfmetadataSegmentMode mode);

/*!
    Cut chapters at camcorder restarts: every enabled mark that does not split
    (a split mark is a segment boundary, so a chapter boundary already) and
    falls strictly inside a chapter cuts it. The pieces of a cut chapter are
    titled "<title> · take 1", "· take 2"...; uncut chapters are unchanged.
*/
QVector<SegmentChapter> cutChaptersAtRestarts(const QVector<SegmentChapter> &chapters, const QVector<SegmentMark> &marks);

/*!
    The restart marks an export uses: those stored in the .tbc.db beside
    metadataPath, else a detection over the whole file.
*/
QVector<SegmentMark> restartMarksForExport(const TbcMetaData &metaData, const QString &metadataPath);

#endif // SEGMENTCHAPTERS_H
