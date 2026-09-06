/************************************************************************

    segments.h

    tbc-segments - Recording-boundary analysis of TBC metadata and fields
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

#ifndef SEGMENTS_H
#define SEGMENTS_H

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include "tbcmetadata.h"

// Analysis thresholds (all CLI options have a matching field).
struct SegmentsThresholds {
    double gapTolerance = 0.333;         // fraction of the nominal field length
    qint32 syncConfThreshold = 50;       // syncConf below this = sync loss
    qint32 minRunFields = 2;             // minimum run for sync_loss / dropout_storm / field-data runs
    double dropoutStormThreshold = 0.25; // fraction of the active field area covered by dropouts
    double sceneThresholdIre = 12.0;     // same-parity field difference marking a scene change
    double noiseThresholdIre = 6.0;      // back-porch noise above which a field is snow
    double blankLumaIre = 5.0;           // active luma at or below which a quiet field is blank
};

// 0-based, half-open field range relative to the start of the TBC.
struct FieldRange {
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
};

struct SegmentEvent {
    QString kind;               // gap | sync_loss | parity_break | skipped_field | dropout_storm |
                                // scene_change | noise | blank_video | no_burst
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
    double severity = 0.0;      // 0..1
    QJsonObject detail;
};

struct SegmentSection {
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
};

// Per-field metrics from the --tbc field walk (whole-file indexed; NaN = not measured).
struct FieldMetrics {
    bool enabled = false;
    bool hasBurst = false;
    QVector<double> lumaMeanIre;
    QVector<double> fieldDiffIre;
    QVector<double> blankingDevIre;
    QVector<double> syncTipDevIre;
    QVector<double> noiseIre;
    QVector<double> burstAmpIre;

    void resize(qint32 numberOfFields);
};

struct SegmentsAnalysis {
    double fieldRate = 0.0;
    double secondsPerField = 0.0;
    double nominalSamplesPerField = 0.0;
    QString nominalSource;          // "rf-sample-rate-hz" | "median-delta" | "diskLoc" | "none"
    double impliedRfSampleRateHz = 0.0;
    QString gapDetection;           // "fileLoc" | "diskLoc" | "unavailable"
    qint64 fileLocRolloverFixups = 0;
    qint32 numberOfFields = 0;
    qint32 numberOfFrames = 0;
    qint32 frameOffset = 0;
    bool isFirstFieldFirst = true;
    qint32 activeWidth = 0;
    qint32 activeLines = 0;

    // Field-data thresholds actually applied: the configured value or a floor
    // relative to the file's own median (real tape noise raises both baselines).
    double effectiveSceneThresholdIre = 0.0;
    double effectiveNoiseThresholdIre = 0.0;
    double medianFieldDiffIre = 0.0;
    double medianNoiseIre = 0.0;

    QVector<SegmentSection> sections;   // gapless sections within the range
    QVector<SegmentEvent> events;       // clipped to the range, in field order
    QMap<QString, qint32> counts;

    // Whole-file per-field arrays (sliced to the range when written with --per-field).
    QVector<qint32> syncConf;
    QVector<qint32> decodeFaults;
    QVector<double> dropoutCoverage;
};

// Fields per second for the metadata's video system (50 for 625-line systems, 60000/1001 else).
double fieldRateForSystem(VideoSystem system);

// Nominal sync-tip level in IRE for the system (-40 NTSC/PAL-M, -43 625-line systems).
double syncTipIreForSystem(VideoSystem system);

// Port of tbc-export-metadata's frame-range resolution: 1-based frames in, 0-based fields out.
bool resolveFieldRange(const TbcMetaData &metaData, qint32 startFrameOneBased, qint32 lengthFrames, FieldRange *range);

// The analysis proper. rfSampleRateHz <= 0 means self-calibrate from the median field delta.
SegmentsAnalysis analyseSegments(const TbcMetaData &metaData,
                                 const FieldRange &range,
                                 const SegmentsThresholds &thresholds,
                                 double rfSampleRateHz,
                                 const FieldMetrics *fieldMetrics);

// The schema v1 report.
QJsonObject buildReport(const TbcMetaData &metaData,
                        const QString &inputPath,
                        const QString &inputKind,
                        qint32 startFrameOneBased,
                        qint32 lengthFrames,
                        const FieldRange &range,
                        const SegmentsThresholds &thresholds,
                        const SegmentsAnalysis &analysis,
                        const FieldMetrics *fieldMetrics,
                        const QJsonObject &fieldDataInfo,
                        bool perField);

// Human summary for --summary.
QString summariseAnalysis(const SegmentsAnalysis &analysis, const FieldRange &range);

#endif // SEGMENTS_H
