/************************************************************************

    segments.h

    tbc-tools TBC library - segment derivation
    Copyright (C) 2026 GDH-Technologies LLC

    This file is part of tbc-tools.

    tbc-tools is free software: you can redistribute it and/or
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

// The one place the segment rules live. Every tool (tbc-segments,
// tbc-export-metadata, tbc-analyse) derives events and segments through these
// functions; none re-implements the gap, section or frame-range rule.
// Vocabulary: docs/Tools/tbc-segments-glossary.md (glossary_generated.h).

enum class SegmentSensitivity { Low, Normal, High };

// Analysis thresholds (all tbc-segments CLI options have a matching field).
struct SegmentsThresholds {
    double gapTolerance = 1.0 / 3.0;     // fraction of the nominal field length (tbc-audio-align's exact value)
    qint32 syncConfThreshold = 50;       // % of the file's median syncConf; below = sync loss
    qint32 minRunFields = 2;             // minimum run for sync_loss / dropout_storm / field-data runs
    double dropoutStormThreshold = 0.25; // fraction of the active field area covered by dropouts
    double fieldDiffSpikeIre = 12.0;     // Glossary: field diff spike (#field-diff-spike): the threshold, a floor
    double noiseThresholdIre = 6.0;      // Glossary: noise (#noise): back-porch noise above which a field is snow
    double blankLumaIre = 5.0;           // Glossary: blank (#blank): active luma at or below which a quiet field is blank...
    double blankLevelDevIre = 5.0;       // ...when its levels have collapsed: blanking this far below, or
                                         // sync tip this far above, nominal (a dark recorded picture keeps them)
    double blankBelowBlackIre = -5.0;    // ...or its active luma sits below this (no picture at all)

    // Segment derivation (Glossary: segment, #segment)
    qint32 minContentSpanFields = 10;    // a piece shorter than this is 'unknown', never a content span
    qint32 minNonContentRunFields = 50;  // a noise/blank run at least this long ends a content span
    double nonContentCoverage = 0.6;     // fraction of a piece that makes it noise or blank

    // Glossary: shot boundary (#shot-boundary), detected from the picture
    // (shotboundaries.h): the field difference spikes on two adjacent fields,
    // matched, with quiet fields either side and no flash just before
    bool detectShotBoundaries = true;
    double shotBoundaryPairMatch = 0.90;      // the weaker spike of the pair is at least this fraction of the stronger
    double shotBoundaryMinPeakIre = 5.0;      // the pair's peak field difference
    double shotBoundaryMinContrast = 1.5;     // peak over the local background
    double shotBoundaryMaxNeighbourRatio = 0.6; // fields n-1 and n+2 at most this fraction of the peak (motion keeps them high)
    qint32 shotBoundaryFlashWindowFields = 12; // a one-field flash this close before the pair rules it out
    double shotBoundaryFlashIre = 10.0;       // how far above its neighbours a flashed field's luma sits

    static SegmentsThresholds preset(SegmentSensitivity sensitivity);
};

// 0-based, half-open field range relative to the start of the TBC.
struct FieldRange {
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
};

struct SegmentEvent {
    QString kind;               // gap | sync_loss | parity_break | skipped_field | dropout_storm |
                                // field_diff_spike | noise | blank_video | no_burst | shot_boundary |
                                // and the decoder's own kinds (no_sync_pulses, no_field_start,
                                // duplicate_field, dropped_field, resume_seam, redo)
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
    double severity = 0.0;      // 0..1
    QJsonObject detail;
};

struct SegmentSection {
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
};

// Per-field metrics (whole-file indexed; NaN = not measured), either stored in
// the metadata by the decoder / a backfill or produced by a --tbc field walk.
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

    // The metrics stored with the metadata (Field::pictureMetrics); enabled
    // only when at least one field carries a finite value.
    static FieldMetrics fromMetadata(const TbcMetaData &metaData);
    // Every field has stored metrics (nothing for a walk to add)
    static bool metadataIsComplete(const TbcMetaData &metaData);
};

struct SegmentsAnalysis {
    double fieldRate = 0.0;
    double secondsPerField = 0.0;
    double nominalSamplesPerField = 0.0;
    QString nominalSource;          // "rf-sample-rate-hz" | "metadata-rf-sample-rate-hz" | "median-delta" | "diskLoc" | "none"
    double impliedRfSampleRateHz = 0.0;
    QString gapDetection;           // "fileLoc" | "diskLoc" | "unavailable"
    qint64 fileLocRolloverFixups = 0;
    qint32 numberOfFields = 0;
    qint32 numberOfFrames = 0;
    qint32 frameOffset = 0;
    bool isFirstFieldFirst = true;
    qint32 activeWidth = 0;
    qint32 activeLines = 0;
    qint32 decoderEventCount = 0;   // decoder events merged into `events`

    // Thresholds actually applied. syncConf is scaled by the file's own median
    // (vhs-decode writes 45 for a healthy VHS field, ld-decode 100); the
    // field-data thresholds are floors under a multiple of the file median
    // (real tape noise raises both baselines).
    double effectiveSyncConfThreshold = 0.0;
    double medianSyncConf = 0.0;
    double effectiveFieldDiffSpikeIre = 0.0;
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

// The stored kind of a content span (Glossary: content span, #content-span).
// A stored "clip" is the kind's old name and reads the same. glossary-legacy
inline const QString &contentSpanKind()
{
    static const QString kind = QStringLiteral("content_span");
    return kind;
}
inline bool isContentSpanKind(const QString &kind)
{
    return kind == contentSpanKind() || kind == QLatin1String("clip"); // glossary-legacy: stored before 3.3.1-gdh-1.2
}

// Fields per second for the metadata's video system (50 for 625-line systems, 60000/1001 else).
double fieldRateForSystem(VideoSystem system);

// Nominal sync-tip level in IRE for the system (-40 NTSC/PAL-M, -43 625-line systems).
double syncTipIreForSystem(VideoSystem system);

// tbc-export-metadata's frame-range rule: 1-based inclusive frames in, 0-based
// half-open fields out (the first field of the start frame to the second field
// of the end frame). totalFields is the number of fields actually present.
bool resolveFieldRange(const TbcMetaData &metaData, qint32 startFrameOneBased, qint32 lengthFrames, FieldRange *range);

// The 1-based frame that contains 0-based field `field`, or -1 when no frame
// does (a leading or trailing orphan field).
qint32 frameContainingField(const TbcMetaData &metaData, qint32 field);

// The frames an export of `segment` covers, under the mixed-frame rule: a frame
// whose second field starts a segment belongs to the segment that owns its
// first field, so contiguous segments tile the frame range exactly once.
// Returns false when the segment holds no whole frame.
bool segmentFrameRange(const TbcMetaData &metaData, const TbcMetaData::Segment &segment,
                       qint32 *startFrameOneBased, qint32 *lengthFrames);

// The analysis proper. rfSampleRateHz > 0 overrides the rate stored in the
// metadata (videoParameters.rfSourceSampleRateHz); with neither, the nominal
// field length is the median of the positive fileLoc deltas. Decoder events
// stored in the metadata are merged into the event list; they never create a
// section on their own (sections break only at fileLoc gaps, the rule
// tbc-audio-align applies), but a gap they coincide with is marked confirmed.
SegmentsAnalysis analyseSegments(const TbcMetaData &metaData,
                                 const FieldRange &range,
                                 const SegmentsThresholds &thresholds,
                                 double rfSampleRateHz,
                                 const FieldMetrics *fieldMetrics);

// Segments from an analysis (Glossary: segment, #segment). Every gapless
// section becomes one piece, broken further at noise/blank runs of at least
// thresholds.minNonContentRunFields; each piece is classified content_span |
// blank | noise | unknown, and enabled when it is a content span. Adjacent
// noise/blank/unknown pieces then merge (across gaps too: snow broken by
// single quiet fields or decoder resyncs is one stretch); a piece shorter
// than minContentSpanFields between two content pieces joins them; and
// content pieces that meet across an RF gap merge into one content span (the
// gap is a shot boundary, detectShotBoundaries). Ids run 1..N in field order;
// source is "derived"; createdBy names the tool.
QVector<TbcMetaData::Segment> deriveSegments(const TbcMetaData &metaData,
                                             const SegmentsAnalysis &analysis,
                                             const SegmentsThresholds &thresholds,
                                             const QString &createdBy);

// The schema v1 report (+ segments and their frame ranges). segmentsSource
// says where `segments` came from: "stored" | "derived" | "none".
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
                        bool perField,
                        const QVector<TbcMetaData::Segment> &segments = QVector<TbcMetaData::Segment>(),
                        const QString &segmentsSource = QStringLiteral("none"));

// Human summary for --summary.
QString summariseAnalysis(const SegmentsAnalysis &analysis, const FieldRange &range);

#endif // SEGMENTS_H
