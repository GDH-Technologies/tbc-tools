/************************************************************************

    segmentexportplan.h

    tbc-segments - Which segments a per-segment export runs, and how
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

#ifndef SEGMENTEXPORTPLAN_H
#define SEGMENTEXPORTPLAN_H

#include <QString>
#include <QVector>
#include <functional>

#include "shotboundaries.h"
#include "tbcmetadata.h"

// One export run: a content span's, scene's or shot's frame range and its
// output base. Glossary: export by (docs/Tools/tbc-segments-glossary.md#export-by).
struct SegmentExportItem {
    qint32 segmentId = 0;
    qint32 startFrameOneBased = -1;
    qint32 lengthFrames = -1;
    QString outputBase;
    QString label;
};

struct SegmentExportPlan {
    QVector<SegmentExportItem> items;
    QVector<qint32> skippedNoWholeFrame;   // ids of selected spans/scenes/shots that hold no whole frame
};

// The frames an export of a segment covers (the caller's segmentFrameRange).
using SegmentFrameRangeFn = std::function<bool(const TbcMetaData::Segment &, qint32 *, qint32 *)>;

/*!
    One item per selected content span, scene or shot, in field order.
    selection (export by):
      "enabled_content_spans"  enabled content span segments (the default)
      "enabled_scenes"         scenes enabled at the scene level
      "enabled_shots"          shots enabled at the shot level
      "enabled" / "all"        enabled segments of any kind / every segment
    Scenes and shots are worked out from the segments and boundaries
    (sceneshots.h). Output base <outputBase>_<NN> for a segment,
    <outputBase>_scene<NN> or <outputBase>_shot<NN>, NN zero-padded to the
    widest id (at least two digits); the title is appended only when asked,
    sanitised to [A-Za-z0-9._-], at most 40 characters.
*/
SegmentExportPlan planSegmentExports(const QVector<TbcMetaData::Segment> &segments,
                                     const QVector<ShotBoundary> &boundaries,
                                     const QString &selection,
                                     const QString &outputBase,
                                     bool appendTitle,
                                     const SegmentFrameRangeFn &frameRange);

#endif // SEGMENTEXPORTPLAN_H
