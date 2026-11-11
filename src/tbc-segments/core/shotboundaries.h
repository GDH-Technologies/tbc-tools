/************************************************************************

    shotboundaries.h

    tbc-segments - Shot boundaries
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

#ifndef SHOTBOUNDARIES_H
#define SHOTBOUNDARIES_H

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "segments.h"
#include "tbcmetadata.h"

// Glossary: shot boundary (docs/Tools/tbc-segments-glossary.md#shot-boundary).
// Stored beside the segments, never as one: scenes and shots are worked out
// from these (sceneshots.h), and the stored segments stay content spans.
struct ShotBoundary {
    qint32 id = 0;
    qint32 field = 0;               // 0-based first field of the new shot
    QString evidence = QStringLiteral("picture");   // Glossary: evidence (#evidence): picture | rf_gap
    QString source = QStringLiteral("derived");     // Glossary: derived / user (#source)
    QString verdict = QStringLiteral("detected");   // Glossary: verdict (#verdict): detected | confirmed | rejected
    bool sceneChange = false;       // Glossary: scene change (#scene-change)
    QString sceneTitle;             // Glossary: title (#title), of the scene this scene change starts
    bool sceneEnabled = true;       // Glossary: enabled (#enabled), of that scene
    bool shotEnabled = true;        // Glossary: enabled (#enabled), of the shot this boundary starts
    double confidence = 0.0;        // Glossary: confidence (#confidence), 0..1
    QString reason;                 // why it was detected, for people
    QString detailJson;             // the measurements behind it
    QString comment;
    QString createdBy;
    QString updatedAt;
    QString derivedFrom;

    bool rejected() const { return verdict == QLatin1String("rejected"); }
    bool operator==(const ShotBoundary &other) const;
};

/*!
    Detect shot boundaries inside the content spans of baseSegments (the
    segments as derived). Two kinds of evidence:

    picture: the same-parity field difference spikes on two adjacent fields n
    and n+1, matched (the weaker at least shotBoundaryPairMatch of the
    stronger, the peak at least shotBoundaryMinPeakIre), standing out
    (peak / local background >= shotBoundaryMinContrast, the background being
    the larger median of fields n-12..n-2 and n+3..n+14) with quiet neighbours
    (fields n-1 and n+2 no higher than halfway from the background to the
    peak, nor above shotBoundaryMaxNeighbourRatio of it). Neither field may be
    noise, and no flash (one field shotBoundaryFlashIre brighter than its
    neighbours) may fall in the shotBoundaryFlashWindowFields before it.
    Fields that close to a span's edges are skipped. metrics may be null
    (no picture evidence then).

    rf_gap: a gap event (Glossary: RF gap, #rf-gap) strictly inside a content
    span. Confidence 0.9 when the decoder confirmed the seam, else 0.75.

    Boundaries closer than four fields keep the stronger (picture beats a gap
    at the same place). Ids run 1..N in field order.
*/
QVector<ShotBoundary> detectShotBoundaries(const SegmentsAnalysis &analysis,
                                           const FieldMetrics *metrics,
                                           const SegmentsThresholds &thresholds,
                                           const QVector<TbcMetaData::Segment> &baseSegments,
                                           const QString &createdBy);

/*!
    The shot boundaries after a fresh detection: every user boundary and every
    rejected one is kept as it is, derived ones are replaced by the detection,
    and a detected boundary within two fields of a kept one is dropped (the
    kept one stands for it). Ids are kept for kept boundaries and for derived
    ones the detection finds again (within two fields); new ones get ids above
    every stored id, so a vanished boundary's id is never reused. The result
    is in field order.
*/
QVector<ShotBoundary> mergeShotBoundaries(const QVector<ShotBoundary> &stored, const QVector<ShotBoundary> &detected);

// Boundaries that are not rejected as shot_boundary events (one field each).
QVector<SegmentEvent> shotBoundaryEvents(const QVector<ShotBoundary> &boundaries);

// Add the shot boundaries to a buildReport() report: "shotBoundaries" (each
// with field, seconds, frame, evidence, source, verdict, scene change and its
// title and enabled flags, shot enabled, confidence, reason, comment,
// measurements), "shotBoundariesSource" (stored | derived | none), the
// unrejected ones as shot_boundary events in field order,
// counts.shot_boundary, and the "scenes" and "shots" worked out from them
// and the report's segments (sceneshots.h).
void addShotBoundariesToReport(QJsonObject *report, const TbcMetaData &metaData, const SegmentsAnalysis &analysis,
                               const QVector<TbcMetaData::Segment> &segments,
                               const QVector<ShotBoundary> &boundaries, const QString &boundariesSource);

#endif // SHOTBOUNDARIES_H
