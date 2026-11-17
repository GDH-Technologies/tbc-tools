/************************************************************************

    segmentsderive.h

    tbc-segments - Whole-file segment derivation for tools that hold metadata
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

#ifndef SEGMENTSDERIVE_H
#define SEGMENTSDERIVE_H

#include <QString>
#include <QVector>

#include "shotboundaries.h"
#include "segments.h"
#include "tbcmetadata.h"

// True when the metadata holds what derivation needs: decoder events or
// stored picture metrics.
bool hasSegmentEvidence(const TbcMetaData &metaData);

// The derived layer of a whole file: its segments and its shot boundaries.
// Glossary: segment (docs/Tools/tbc-segments-glossary.md#segment), shot boundary (#shot-boundary).
struct SegmentLayer {
    QVector<TbcMetaData::Segment> segments;     // content spans, noise, blank, unknown
    QVector<ShotBoundary> shotBoundaries;       // storedBoundaries merged with a fresh detection
};

// Analyse the whole file (stored picture metrics when there are any), derive
// its segments, detect the shot boundaries inside the content spans and merge
// them with storedBoundaries (user and rejected kept, derived replaced: see
// mergeShotBoundaries). Empty when the metadata has no fields.
SegmentLayer deriveLayerForFile(const TbcMetaData &metaData,
                                const SegmentsThresholds &thresholds,
                                const QString &createdBy,
                                const QVector<ShotBoundary> &storedBoundaries = QVector<ShotBoundary>());

// The segments of deriveLayerForFile.
QVector<TbcMetaData::Segment> deriveSegmentsForFile(const TbcMetaData &metaData,
                                                    const SegmentsThresholds &thresholds,
                                                    const QString &createdBy);

// Derive and store segments in the metadata when none are stored but the
// decoder left evidence. Returns the number derived (0 when nothing was
// stored). The caller decides whether that counts as an edit of the metadata.
qint32 deriveSegmentsAtLoad(TbcMetaData &metaData, const QString &createdBy);

#endif // SEGMENTSDERIVE_H
