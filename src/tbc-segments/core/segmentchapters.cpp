/************************************************************************

    segmentchapters.cpp

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

#include "segmentchapters.h"

#include <algorithm>

#include "markstore.h"
#include "segmentsderive.h"

QVector<SegmentChapter> segmentChapters(const TbcMetaData &metaData, FfmetadataSegmentMode mode)
{
    QVector<SegmentChapter> chapters;
    QVector<TbcMetaData::Segment> segments = metaData.getSegments();
    if (mode == FfmetadataSegmentMode::NoSegments || segments.isEmpty()) {
        return chapters;
    }
    std::stable_sort(segments.begin(), segments.end(),
                     [](const TbcMetaData::Segment &a, const TbcMetaData::Segment &b) {
                         return a.startField < b.startField;
                     });
    chapters.reserve(segments.size());
    for (const TbcMetaData::Segment &segment : segments) {
        if (mode == FfmetadataSegmentMode::EnabledSegments && !segment.enabled) {
            continue;
        }
        SegmentChapter chapter;
        chapter.startField = segment.startField;
        chapter.endFieldExclusive = segment.endFieldExclusive;
        chapter.title = segment.title.trimmed().isEmpty()
                            ? QStringLiteral("Segment %1").arg(segment.id)
                            : segment.title.trimmed();
        chapter.comment = segment.comment.trimmed();
        chapters.append(chapter);
    }
    return chapters;
}

QVector<SegmentChapter> cutChaptersAtRestarts(const QVector<SegmentChapter> &chapters, const QVector<SegmentMark> &marks)
{
    QVector<qint32> cuts;
    for (const SegmentMark &mark : marks) {
        if (mark.enabled && !mark.split) cuts.append(mark.field);
    }
    if (cuts.isEmpty()) return chapters;
    std::sort(cuts.begin(), cuts.end());
    QVector<SegmentChapter> result;
    for (const SegmentChapter &chapter : chapters) {
        QVector<qint32> inside;
        for (qint32 cut : cuts) {
            if (cut > chapter.startField && cut < chapter.endFieldExclusive) inside.append(cut);
        }
        if (inside.isEmpty()) {
            result.append(chapter);
            continue;
        }
        qint32 start = chapter.startField;
        inside.append(chapter.endFieldExclusive);
        for (qint32 take = 0; take < inside.size(); take++) {
            SegmentChapter piece = chapter;
            piece.startField = start;
            piece.endFieldExclusive = inside.at(take);
            piece.title = QStringLiteral("%1 · take %2").arg(chapter.title).arg(take + 1);
            result.append(piece);
            start = inside.at(take);
        }
    }
    return result;
}

QVector<SegmentMark> restartMarksForExport(const TbcMetaData &metaData, const QString &metadataPath)
{
    QVector<SegmentMark> marks;
    MarkStore::read(MarkStore::databasePathFor(metadataPath), &marks);
    if (!marks.isEmpty()) return marks;
    return deriveLayerForFile(metaData, SegmentsThresholds(), QStringLiteral("tbc-export-metadata")).marks;
}
