/************************************************************************

    segmentchapters.cpp

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

#include "segmentchapters.h"

#include <algorithm>

#include "sceneshots.h"
#include "segmentsderive.h"
#include "shotboundarystore.h"

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

ChaptersBy chaptersByFromName(const QString &name, bool *ok)
{
    if (ok) *ok = true;
    if (name.compare(QLatin1String("scene"), Qt::CaseInsensitive) == 0) return ChaptersBy::Scene;
    if (name.compare(QLatin1String("shot"), Qt::CaseInsensitive) == 0) return ChaptersBy::Shot;
    if (ok && !name.isEmpty() && name.compare(QLatin1String("segment"), Qt::CaseInsensitive) != 0) *ok = false;
    return ChaptersBy::Segment;
}

QVector<SegmentChapter> chaptersBy(const TbcMetaData &metaData, FfmetadataSegmentMode mode, ChaptersBy by,
                                   const QVector<ShotBoundary> &boundaries)
{
    const QVector<SegmentChapter> segmentList = segmentChapters(metaData, mode);
    if (by == ChaptersBy::Segment || segmentList.isEmpty()) {
        return segmentList;
    }
    const QVector<TbcMetaData::Segment> segments = metaData.getSegments();
    const QVector<SegmentPart> parts = by == ChaptersBy::Scene ? scenesOf(segments, boundaries)
                                                                : shotsOf(segments, boundaries);
    QVector<SegmentChapter> chapters;
    // Content span chapters are replaced by their parts; the rest stay
    for (const TbcMetaData::Segment &segment : segments) {
        if (!isContentSpanKind(segment.kind)) {
            if (mode == FfmetadataSegmentMode::AllSegments) {
                SegmentChapter chapter;
                chapter.startField = segment.startField;
                chapter.endFieldExclusive = segment.endFieldExclusive;
                chapter.title = segment.title.trimmed().isEmpty() ? QStringLiteral("Segment %1").arg(segment.id)
                                                                  : segment.title.trimmed();
                chapter.comment = segment.comment.trimmed();
                chapters.append(chapter);
            }
            continue;
        }
        for (const SegmentPart &part : parts) {
            if (part.spanId != segment.id) continue;
            if (mode == FfmetadataSegmentMode::EnabledSegments && !part.enabled) continue;
            SegmentChapter chapter;
            chapter.startField = part.startField;
            chapter.endFieldExclusive = part.endFieldExclusive;
            chapter.title = part.title;
            chapters.append(chapter);
        }
    }
    std::stable_sort(chapters.begin(), chapters.end(), [](const SegmentChapter &a, const SegmentChapter &b) {
        return a.startField < b.startField;
    });
    return chapters;
}

QVector<ShotBoundary> shotBoundariesForExport(const TbcMetaData &metaData, const QString &metadataPath)
{
    QVector<ShotBoundary> boundaries;
    const QString database = ShotBoundaryStore::databasePathFor(metadataPath);
    if (!database.isEmpty()) {
        ShotBoundaryStore::read(database, &boundaries);
    } else {
        ShotBoundaryStore::readSidecar(metadataPath, &boundaries);   // tbc-analyse's export snapshot
    }
    if (!boundaries.isEmpty()) return boundaries;
    return deriveLayerForFile(metaData, SegmentsThresholds(), QStringLiteral("tbc-export-metadata")).shotBoundaries;
}
