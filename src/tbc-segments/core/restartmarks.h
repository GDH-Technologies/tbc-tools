/************************************************************************

    restartmarks.h

    tbc-segments - Camcorder restart marks
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

#ifndef RESTARTMARKS_H
#define RESTARTMARKS_H

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "segments.h"
#include "tbcmetadata.h"

// A mark at a recording boundary inside a segment: today a camcorder restart
// (a clean edit, so no RF gap, no snow, no sync loss). Marks are always kept;
// whether one splits its clip into two segments is its own flag.
struct SegmentMark {
    qint32 id = 0;
    qint32 field = 0;               // 0-based first field of the new take
    QString kind = QStringLiteral("restart");
    QString source = QStringLiteral("derived");   // derived | user
    bool enabled = true;            // false: dismissed, kept so a re-derive does not bring it back
    bool split = false;             // the mark splits its clip
    double confidence = 0.0;        // 0..1
    QString reason;                 // why it was marked, for people
    QString detailJson;             // the measurements behind it
    QString comment;
    QString createdBy;
    QString updatedAt;
    QString derivedFrom;

    bool operator==(const SegmentMark &other) const;
};

/*!
    Detect camcorder restarts inside the clips of baseSegments (the segments
    derived without marks). A restart replaces the picture between two fields,
    so the same-parity field difference (field n against n-2) spikes on two
    adjacent fields n and n+1:

    - the pair matches: the weaker spike is at least restartPairMatch of the
      stronger, and the peak is at least restartMinPeakIre;
    - it stands out: peak / local background >= restartMinContrast, the
      background being the larger median of fields n-12..n-2 and n+3..n+14;
    - its neighbours are quiet: fields n-1 and n+2 sit no higher than halfway
      from the background to the peak, nor above restartMaxNeighbourRatio of
      the peak (sustained motion keeps them high);
    - neither field is snow, and no flash (a single field restartFlashIre
      brighter than its neighbours) falls in the restartFlashWindowFields
      before it: a flash lights one field (spikes at n and n+2), and the
      camera's exposure settling after it can leave a matched pair.

    Fields within restartFlashWindowFields of a clip's edges are skipped (the
    edge is a boundary already). Marks closer than four fields keep the one
    with the higher contrast. New marks get split = splitAtRestarts.
*/
QVector<SegmentMark> detectRestartMarks(const SegmentsAnalysis &analysis,
                                        const FieldMetrics &metrics,
                                        const SegmentsThresholds &thresholds,
                                        const QVector<TbcMetaData::Segment> &baseSegments,
                                        const QString &createdBy);

/*!
    The marks after a fresh detection: every user mark and every dismissed
    mark is kept as it is, derived marks are replaced by the detection, and a
    detected mark within two fields of a kept one is dropped (the kept one
    stands for it). Ids are kept for kept marks and for derived marks the
    detection finds again (within two fields); new ones get ids above every
    stored id, so a vanished mark's id is never reused. The result is in
    field order.
*/
QVector<SegmentMark> mergeMarks(const QVector<SegmentMark> &stored, const QVector<SegmentMark> &detected);

/*!
    Cut clip segments at every enabled mark whose split flag is set. Both
    pieces are clips; a piece that gains a start keeps the segment's source
    and kind, a new id, and no title. Other segments, and marks that fall on
    a segment's start or outside every clip, change nothing. Ids are kept
    for the first piece of each segment and assigned after the highest for
    the rest; the result is in field order.
*/
QVector<TbcMetaData::Segment> splitSegmentsAtMarks(const QVector<TbcMetaData::Segment> &segments,
                                                   const QVector<SegmentMark> &marks);

// Enabled marks as restart events (kind "restart", one field each), for the
// report's event list.
QVector<SegmentEvent> restartEvents(const QVector<SegmentMark> &marks);

// Add the marks to a buildReport() report: a "marks" array (each with its
// field, seconds, 1-based frame, kind, source, enabled, split, confidence,
// reason, comment and measurements), "marksSource" (stored | derived |
// none), the enabled marks as "restart" events in field order, and
// counts.restart.
void addMarksToReport(QJsonObject *report, const TbcMetaData &metaData, const SegmentsAnalysis &analysis,
                      const QVector<SegmentMark> &marks, const QString &marksSource);

#endif // RESTARTMARKS_H
