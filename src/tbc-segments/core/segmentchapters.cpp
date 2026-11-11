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
