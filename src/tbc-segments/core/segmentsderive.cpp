/************************************************************************

    segmentsderive.cpp

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

#include "segmentsderive.h"

bool hasSegmentEvidence(const TbcMetaData &metaData)
{
    return !metaData.getDecoderEvents().isEmpty() || metaData.hasPictureMetrics();
}

SegmentLayer deriveLayerForFile(const TbcMetaData &metaData,
                                const SegmentsThresholds &thresholds,
                                const QString &createdBy,
                                const QVector<ShotBoundary> &storedBoundaries)
{
    SegmentLayer layer;
    FieldRange range;
    range.startField = 0;
    range.endFieldExclusive = metaData.getNumberOfFields();
    if (range.endFieldExclusive <= 0) {
        return layer;
    }
    const FieldMetrics metrics = FieldMetrics::fromMetadata(metaData);
    const FieldMetrics *metricsPtr = metrics.enabled ? &metrics : nullptr;
    const SegmentsAnalysis analysis = analyseSegments(metaData, range, thresholds, 0.0, metricsPtr);
    layer.segments = deriveSegments(metaData, analysis, thresholds, createdBy);
    layer.shotBoundaries = mergeShotBoundaries(
        storedBoundaries, detectShotBoundaries(analysis, metricsPtr, thresholds, layer.segments, createdBy));
    return layer;
}

QVector<TbcMetaData::Segment> deriveSegmentsForFile(const TbcMetaData &metaData,
                                                    const SegmentsThresholds &thresholds,
                                                    const QString &createdBy)
{
    return deriveLayerForFile(metaData, thresholds, createdBy).segments;
}

qint32 deriveSegmentsAtLoad(TbcMetaData &metaData, const QString &createdBy)
{
    if (!metaData.getSegments().isEmpty() || !hasSegmentEvidence(metaData)) {
        return 0;
    }
    const QVector<TbcMetaData::Segment> derived = deriveSegmentsForFile(metaData, SegmentsThresholds(), createdBy);
    if (derived.isEmpty()) {
        return 0;
    }
    metaData.setSegments(derived);
    return derived.size();
}
