/************************************************************************

    segmentexportplan.h

    tbc-segments - Which recording segments a per-segment export runs, and how
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

#include "tbcmetadata.h"

// One export run: the segment's frame range and its output base.
struct SegmentExportItem {
    qint32 segmentId = 0;
    qint32 startFrameOneBased = -1;
    qint32 lengthFrames = -1;
    QString outputBase;
    QString label;
};

struct SegmentExportPlan {
    QVector<SegmentExportItem> items;
    QVector<qint32> skippedNoWholeFrame;   // ids of selected segments that hold no whole frame
};

// The frames an export of a segment covers (the caller's segmentFrameRange).
using SegmentFrameRangeFn = std::function<bool(const TbcMetaData::Segment &, qint32 *, qint32 *)>;

/*!
    One item per selected segment, in field order. selection is
    "enabled_clips" (enabled segments of kind clip, the default), "enabled",
    or "all". Output base <outputBase>_<NN>[_<title>], NN zero-padded to the
    widest id (at least two digits); the title is appended only when asked
    and sanitised to [A-Za-z0-9._-], at most 40 characters.
*/
SegmentExportPlan planSegmentExports(const QVector<TbcMetaData::Segment> &segments,
                                     const QString &selection,
                                     const QString &outputBase,
                                     bool appendTitle,
                                     const SegmentFrameRangeFn &frameRange);

#endif // SEGMENTEXPORTPLAN_H
