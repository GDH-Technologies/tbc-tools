/************************************************************************

    testsegments.cpp

    tbc-tools TBC library - unit tests for the recording-segment derivation
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

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QVector>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "segments.h"
#include "tbcmetadata.h"

// Release builds strip assert(); a test that cannot fail proves nothing.
#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

namespace {

const qint32 kWidth = 910;
const qint32 kHeight = 263;
const qint64 kNominal = 667333;            // 40 MHz / 59.94
const double kBlack = 16384.0, kWhite = 54016.0, kBlanking = 16384.0;
const double kNtscFieldRate = 60000.0 / 1001.0;

TbcMetaData::VideoParameters ntscParameters(qint32 fields)
{
    TbcMetaData::VideoParameters vp;
    vp.system = NTSC;
    vp.numberOfSequentialFields = fields;
    vp.fieldWidth = kWidth;
    vp.fieldHeight = kHeight;
    vp.sampleRate = 4.0 * 315.0 / 88.0 * 1e6;
    vp.colourBurstStart = 76;
    vp.colourBurstEnd = 112;
    vp.activeVideoStart = 135;
    vp.activeVideoEnd = 895;
    vp.firstActiveFieldLine = 20;
    vp.lastActiveFieldLine = 259;
    vp.firstActiveFrameLine = 40;
    vp.lastActiveFrameLine = 525;
    vp.black16bIre = static_cast<qint32>(kBlack);
    vp.white16bIre = static_cast<qint32>(kWhite);
    vp.blanking16bIre = static_cast<qint32>(kBlanking);
    vp.tapeFormat = QStringLiteral("VHS");
    vp.isValid = true;
    return vp;
}

// Clean metadata: alternating parity, syncConf 100, fileLoc stepping nominally.
void buildMetadata(TbcMetaData &m, qint32 fields, qint64 firstLoc = 0, bool firstFieldFirst = true)
{
    m.clear();
    m.setVideoParameters(ntscParameters(fields));
    m.setIsFirstFieldFirst(true);
    for (qint32 i = 0; i < fields; i++) {
        TbcMetaData::Field f;
        f.seqNo = i + 1;
        f.isFirstField = firstFieldFirst ? (i % 2 == 0) : (i % 2 == 1);
        f.syncConf = 100;
        f.fieldPhaseID = (i % 4) + 1;
        f.fileLoc = firstLoc + static_cast<qint64>(i) * kNominal;
        f.diskLoc = static_cast<double>(i);
        f.decodeFaults = 0;
        m.appendField(f);
    }
}

qint32 countKind(const SegmentsAnalysis &a, const char *kind)
{
    return a.counts.value(QString::fromLatin1(kind), 0);
}

const SegmentEvent *firstOfKind(const SegmentsAnalysis &a, const char *kind)
{
    for (const SegmentEvent &e : a.events) if (e.kind == QLatin1String(kind)) return &e;
    return nullptr;
}

FieldRange fullRange(const TbcMetaData &m)
{
    FieldRange r;
    CHECK(resolveFieldRange(m, 0, 0, &r));
    return r;
}

// Shift every field from `from` onward by `fields` nominal field lengths
void insertGap(TbcMetaData &m, qint32 from, double fields)
{
    for (qint32 i = from; i < m.getNumberOfFields(); i++) {
        TbcMetaData::Field f = m.getField(i + 1);
        f.fileLoc += static_cast<qint64>(std::llround(fields * kNominal));
        m.updateField(f, i + 1);
    }
}

// --- metadata tier ---------------------------------------------------------------

void testCleanCapture()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), SegmentsThresholds(), 0.0, nullptr);
    CHECK(a.numberOfFields == 400 && a.numberOfFrames == 200 && a.frameOffset == 0);
    CHECK(a.gapDetection == QLatin1String("fileLoc"));
    CHECK(a.nominalSource == QLatin1String("median-delta"));
    CHECK(std::abs(a.nominalSamplesPerField - kNominal) < 1.0);
    CHECK(std::abs(a.impliedRfSampleRateHz - 40e6) < 1000.0);
    CHECK(a.sections.size() == 1 && a.events.isEmpty());
    CHECK(std::abs(a.secondsPerField - 1001.0 / 60000.0) < 1e-12);
    CHECK(a.decoderEventCount == 0);

    const SegmentsAnalysis withRate = analyseSegments(m, fullRange(m), SegmentsThresholds(), 40e6, nullptr);
    CHECK(withRate.nominalSource == QLatin1String("rf-sample-rate-hz"));
    CHECK(withRate.events.isEmpty());
}

void testRateFromMetadata()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    TbcMetaData::VideoParameters vp = m.getVideoParameters();
    vp.rfSourceSampleRateHz = 40e6;
    m.setVideoParameters(vp);
    const SegmentsAnalysis stored = analyseSegments(m, fullRange(m), SegmentsThresholds(), 0.0, nullptr);
    CHECK(stored.nominalSource == QLatin1String("metadata-rf-sample-rate-hz"));
    CHECK(std::abs(stored.nominalSamplesPerField - 40e6 / kNtscFieldRate) < 1e-6);
    // An explicit rate still wins over the stored one
    const SegmentsAnalysis overridden = analyseSegments(m, fullRange(m), SegmentsThresholds(), 28.6e6, nullptr);
    CHECK(overridden.nominalSource == QLatin1String("rf-sample-rate-hz"));
    CHECK(std::abs(overridden.nominalSamplesPerField - 28.6e6 / kNtscFieldRate) < 1e-6);
}

void testGapSplitsSections()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    insertGap(m, 100, 5.0);   // five fields of tape went by unseen
    SegmentsThresholds t;
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), t, 0.0, nullptr);
    CHECK(countKind(a, "gap") == 1);
    const SegmentEvent *gap = firstOfKind(a, "gap");
    CHECK(gap && gap->startField == 100 && gap->endFieldExclusive == 101);
    CHECK(gap->detail.value("missingFields").toInt() == 5);
    CHECK(gap->detail.value("seamAfterField").toInt() == 99);
    CHECK(a.sections.size() == 2 && a.sections[0].endFieldExclusive == 100 && a.sections[1].startField == 100);

    t.gapTolerance = 0.9;   // still outside: the delta is 6× nominal
    CHECK(countKind(analyseSegments(m, fullRange(m), t, 0.0, nullptr), "gap") == 1);
}

void testFaultsAndSyncLoss()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    {
        TbcMetaData::Field f = m.getField(201); f.decodeFaults = 1; m.updateField(f, 201);
        f = m.getField(211); f.decodeFaults = 4; m.updateField(f, 211);
        for (qint32 i = 250; i < 260; i++) { f = m.getField(i + 1); f.syncConf = 10; m.updateField(f, i + 1); }
        f = m.getField(301); f.syncConf = 10; m.updateField(f, 301);   // a lone dip
        f = m.getField(351); f.decodeFaults = -1; m.updateField(f, 351);  // "absent" in JSON = 0
    }
    SegmentsThresholds t;
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), t, 40e6, nullptr);
    CHECK(countKind(a, "parity_break") == 1 && firstOfKind(a, "parity_break")->startField == 200);
    CHECK(countKind(a, "skipped_field") == 1 && firstOfKind(a, "skipped_field")->startField == 210);
    CHECK(countKind(a, "sync_loss") == 1);
    const SegmentEvent *sync = firstOfKind(a, "sync_loss");
    CHECK(sync->startField == 250 && sync->endFieldExclusive == 260);
    CHECK(sync->detail.value("minSyncConf").toInt() == 10);
    t.minRunFields = 1;
    CHECK(countKind(analyseSegments(m, fullRange(m), t, 40e6, nullptr), "sync_loss") == 2);
}

void testSameParityRepeatIsAParityBreak()
{
    TbcMetaData m;
    buildMetadata(m, 100);
    TbcMetaData::Field f = m.getField(51);
    f.isFirstField = m.getField(50).isFirstField;
    m.updateField(f, 51);
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), SegmentsThresholds(), 40e6, nullptr);
    CHECK(countKind(a, "parity_break") >= 1);
    CHECK(firstOfKind(a, "parity_break")->detail.value("reason").toString() == QLatin1String("same-parity-repeat"));
}

void testDropoutStorm()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    for (qint32 i = 320; i < 331; i++) {
        DropOuts d;
        for (qint32 line = 20; line < 259; line += 2) d.append(135, 135 + 456, line);   // 60% of the active width on half the lines
        m.updateFieldDropOuts(d, i + 1);
    }
    {
        DropOuts outside;
        outside.append(0, 900, 5);       // above the active lines
        outside.append(0, 100, 100);     // left of the active area
        m.updateFieldDropOuts(outside, 380);
    }
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), SegmentsThresholds(), 40e6, nullptr);
    CHECK(countKind(a, "dropout_storm") == 1);
    const SegmentEvent *storm = firstOfKind(a, "dropout_storm");
    CHECK(storm->startField == 320 && storm->endFieldExclusive == 331);
    CHECK(std::abs(a.dropoutCoverage[325] - 0.30) < 0.02);
    CHECK(a.dropoutCoverage[379] == 0.0);
}

void testRolloverThroughJson()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    TbcMetaData m;
    buildMetadata(m, 400, 2147483647LL - 200LL * kNominal);
    // Wrap like a 32-bit writer would: values past 2^31-1 go negative.
    for (qint32 i = 0; i < 400; i++) {
        TbcMetaData::Field f = m.getField(i + 1);
        if (f.fileLoc > 2147483647LL) f.fileLoc -= 4294967296LL;
        m.updateField(f, i + 1);
    }
    const QString path = dir.filePath("wrap.tbc.json");
    CHECK(m.write(path));
    TbcMetaData back;
    CHECK(back.read(path));
    const SegmentsAnalysis a = analyseSegments(back, fullRange(back), SegmentsThresholds(), 40e6, nullptr);
    CHECK(a.fileLocRolloverFixups == 1);
    CHECK(countKind(a, "gap") == 0);
    CHECK(a.sections.size() == 1);
}

void testRangeResolution()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    insertGap(m, 100, 5.0);
    FieldRange r;
    CHECK(resolveFieldRange(m, 51, 10, &r));
    CHECK(r.startField == 100 && r.endFieldExclusive == 120);
    const SegmentsAnalysis a = analyseSegments(m, r, SegmentsThresholds(), 40e6, nullptr);
    CHECK(countKind(a, "gap") == 1);   // the seam at field 100 is the range's first field
    CHECK(a.sections.size() == 1 && a.sections[0].startField == 100 && a.sections[0].endFieldExclusive == 120);
    CHECK(!resolveFieldRange(m, 500, 1, &r));

    TbcMetaData pal;
    buildMetadata(pal, 100);
    TbcMetaData::VideoParameters vp = ntscParameters(100);
    vp.system = PAL; vp.fieldHeight = 313; vp.fieldWidth = 1135;
    pal.setVideoParameters(vp);
    const SegmentsAnalysis p = analyseSegments(pal, fullRange(pal), SegmentsThresholds(), 0.0, nullptr);
    CHECK(std::abs(p.secondsPerField - 0.02) < 1e-12);
}

// --- tbc-audio-align equivalence ---------------------------------------------------

// The reference: VhsDecodeAutoAudioAlign's ExtractGapLessSection over
// TbcJsonFixup-corrected positions. avg = rate / fieldRate, tol = avg / 3,
// strict comparisons, unsigned subtraction (a backward step wraps to a huge
// length and starts a section). TbcJsonFixup.Fix: a negative raw fileLoc
// whose raw predecessor was positive (or which has none) adds 2^32 to the
// running correction applied to it and everything after it.
QVector<qint32> referenceSectionStarts(const QVector<qint64> &rawFileLoc, double rateHz, double fieldRate)
{
    QVector<quint64> positions;
    qint64 rolloverCorrection = 0;
    bool havePrevious = false;
    qint64 previousRaw = 0;
    for (qint64 raw : rawFileLoc) {
        if (raw < 0 && (!havePrevious || previousRaw > 0)) rolloverCorrection += 4294967296LL;
        positions.append(static_cast<quint64>(raw + rolloverCorrection));
        previousRaw = raw;
        havePrevious = true;
    }
    const double avg = rateHz / fieldRate;
    const double tol = avg / 3.0;
    QVector<qint32> starts;
    starts.append(0);
    for (qint32 i = 1; i < positions.size(); i++) {
        const quint64 length = positions[i] - positions[i - 1];   // wraps on a backward step
        const double len = static_cast<double>(length);
        const bool inTolerance = len > (avg - tol) && len < (avg + tol);
        if (!inTolerance) starts.append(i);
    }
    return starts;
}

void checkAaaEquivalence(TbcMetaData &m, const QVector<qint64> &rawFileLoc, double rateHz, bool storeRate)
{
    for (qint32 i = 0; i < rawFileLoc.size(); i++) {
        TbcMetaData::Field f = m.getField(i + 1);
        f.fileLoc = rawFileLoc[i];
        m.updateField(f, i + 1);
    }
    TbcMetaData::VideoParameters vp = m.getVideoParameters();
    vp.rfSourceSampleRateHz = storeRate ? rateHz : -1.0;
    m.setVideoParameters(vp);
    const double fieldRate = fieldRateForSystem(vp.system);
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), SegmentsThresholds(), storeRate ? 0.0 : rateHz, nullptr);
    const QVector<qint32> expected = referenceSectionStarts(rawFileLoc, rateHz, fieldRate);
    QVector<qint32> got;
    for (const SegmentSection &s : a.sections) got.append(s.startField);
    if (got != expected) {
        std::cerr << "sections differ: library";
        for (qint32 g : got) std::cerr << " " << g;
        std::cerr << " | reference";
        for (qint32 e : expected) std::cerr << " " << e;
        std::cerr << "\n";
    }
    CHECK(got == expected);
}

void testAudioAlignEquivalence()
{
    for (const bool pal : {false, true}) {
        for (const bool storeRate : {true, false}) {
            const qint32 fields = 600;
            TbcMetaData m;
            buildMetadata(m, fields);
            if (pal) {
                TbcMetaData::VideoParameters vp = ntscParameters(fields);
                vp.system = PAL; vp.fieldHeight = 313; vp.fieldWidth = 1135;
                m.setVideoParameters(vp);
            }
            const double rateHz = pal ? 35.8e6 : 40e6;
            const double nominal = rateHz / fieldRateForSystem(m.getVideoParameters().system);

            QRandomGenerator rng(pal ? 7 : 11);
            QVector<qint64> raw;
            qint64 pos = 2147483647LL - static_cast<qint64>(150 * nominal);   // wraps past field ~150
            for (qint32 i = 0; i < fields; i++) {
                if (i > 0) {
                    double step = nominal * (0.7 + 0.6 * rng.generateDouble());   // ±30 % jitter
                    if (i == 100) step = 1.5 * nominal;
                    if (i == 200) step = 3.0 * nominal;
                    if (i == 300) step = 5.0 * nominal;
                    if (i == 400) step = -1.0 * nominal;                          // a backward re-read
                    if (i == 500) step = nominal * (1.0 + 0.3333);               // exactly at the edge
                    pos += static_cast<qint64>(std::llround(step));
                }
                qint64 stored = pos;
                if (stored > 2147483647LL) stored -= 4294967296LL;            // 32-bit writer
                raw.append(stored);
            }
            checkAaaEquivalence(m, raw, rateHz, storeRate);
        }
    }
}

// --- decoder events -----------------------------------------------------------------

void testDecoderEventsMerge()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    insertGap(m, 100, 3.0);
    {
        TbcMetaData::Field f = m.getField(211);
        f.decodeFaults = 4;
        m.updateField(f, 211);
    }
    TbcMetaData::DecoderEvent jump;
    jump.kind = QStringLiteral("no_sync_pulses");
    jump.field = 100;
    jump.hasRfDeltaSamples = true;
    jump.rfDeltaSamples = 3 * kNominal;
    jump.rfDeltaFields = 3.0;
    jump.detailJson = QStringLiteral("{\"jumpSamples\":4000000}");
    m.appendDecoderEvent(jump);
    TbcMetaData::DecoderEvent skipped;
    skipped.kind = QStringLiteral("skipped_field");
    skipped.field = 210;
    m.appendDecoderEvent(skipped);
    TbcMetaData::DecoderEvent resume;
    resume.kind = QStringLiteral("resume_seam");
    resume.field = 300;
    m.appendDecoderEvent(resume);

    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), SegmentsThresholds(), 40e6, nullptr);
    CHECK(a.decoderEventCount == 3);
    // The gap the decoder saw is confirmed, its severity raised
    const SegmentEvent *gap = firstOfKind(a, "gap");
    CHECK(gap && gap->startField == 100 && gap->detail.value("decoderConfirmed").toBool());
    CHECK(gap->severity > 0.5);
    // The decoder's own event is present with its detail, once
    const SegmentEvent *jumpEvent = firstOfKind(a, "no_sync_pulses");
    CHECK(jumpEvent && jumpEvent->startField == 100);
    CHECK(jumpEvent->detail.value("source").toString() == QLatin1String("decoder"));
    CHECK(jumpEvent->detail.value("decoderDetail").toObject().value("jumpSamples").toInt() == 4000000);
    // The decoder's skipped_field for a field the fault bit already marks is not a second event
    CHECK(countKind(a, "skipped_field") == 1);
    // A resume seam is an event but never a section boundary
    CHECK(countKind(a, "resume_seam") == 1);
    CHECK(a.sections.size() == 2);
}

// --- stored picture metrics -----------------------------------------------------------

void testStoredMetricsDriveFieldDataEvents()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    CHECK(!FieldMetrics::fromMetadata(m).enabled);
    CHECK(!FieldMetrics::metadataIsComplete(m));
    for (qint32 i = 0; i < 400; i++) {
        TbcMetaData::PictureMetrics p;
        p.lumaMeanIre = (i >= 200 && i < 220) ? 2.0 : 50.0;
        p.fieldDiffIre = (i == 60) ? 30.0 : 0.3;
        p.noiseIre = (i >= 120 && i < 140) ? 25.0 : 0.4;
        p.blankingDevIre = 0.1;
        p.syncTipDevIre = -0.2;
        p.burstAmpIre = (i >= 300 && i < 320) ? 0.5 : 40.0;
        m.updateFieldPictureMetrics(p, i + 1);
    }
    const FieldMetrics stored = FieldMetrics::fromMetadata(m);
    CHECK(stored.enabled && stored.hasBurst);
    CHECK(FieldMetrics::metadataIsComplete(m));
    CHECK(stored.lumaMeanIre.size() == 400 && std::abs(stored.noiseIre[130] - 25.0) < 1e-9);

    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), SegmentsThresholds(), 40e6, &stored);
    const SegmentEvent *scene = firstOfKind(a, "scene_change");
    CHECK(scene && scene->startField == 60);
    const SegmentEvent *noise = firstOfKind(a, "noise");
    CHECK(noise && noise->startField == 120 && noise->endFieldExclusive == 140);
    const SegmentEvent *blank = firstOfKind(a, "blank_video");
    CHECK(blank && blank->startField == 200 && blank->endFieldExclusive == 220);
    const SegmentEvent *burst = firstOfKind(a, "no_burst");
    CHECK(burst && burst->startField == 300 && burst->endFieldExclusive == 320);
}

// --- segments -----------------------------------------------------------------------------

void testDeriveSegments()
{
    TbcMetaData m;
    buildMetadata(m, 400);
    insertGap(m, 396, 4.0);   // a four-field tail section
    for (qint32 i = 0; i < 400; i++) {
        TbcMetaData::PictureMetrics p;
        p.lumaMeanIre = (i >= 200 && i < 260) ? 2.0 : 50.0;   // a long blank stretch
        p.fieldDiffIre = 0.3;
        p.noiseIre = (i >= 120 && i < 140) ? 25.0 : 0.4;    // a short noise burst
        p.blankingDevIre = 0.1;
        p.syncTipDevIre = -0.2;
        p.burstAmpIre = 40.0;
        m.updateFieldPictureMetrics(p, i + 1);
    }
    const FieldMetrics stored = FieldMetrics::fromMetadata(m);
    SegmentsThresholds t;
    t.minNonClipRunFields = 30;   // the 20-field noise burst is too short to split
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), t, 40e6, &stored);
    const QVector<TbcMetaData::Segment> segments = deriveSegments(m, a, t, QStringLiteral("testsegments"));

    // [0,200) clip, [200,260) blank, [260,396) clip, [396,400) unknown
    CHECK(segments.size() == 4);
    CHECK(segments[0].id == 1 && segments[0].startField == 0 && segments[0].endFieldExclusive == 200);
    CHECK(segments[0].kind == QLatin1String("clip") && segments[0].enabled);
    CHECK(segments[1].startField == 200 && segments[1].endFieldExclusive == 260);
    CHECK(segments[1].kind == QLatin1String("blank") && !segments[1].enabled);
    CHECK(segments[2].startField == 260 && segments[2].endFieldExclusive == 396 && segments[2].kind == QLatin1String("clip"));
    CHECK(segments[3].startField == 396 && segments[3].endFieldExclusive == 400 && segments[3].kind == QLatin1String("unknown"));
    for (const TbcMetaData::Segment &s : segments) {
        CHECK(s.source == QLatin1String("derived"));
        CHECK(s.createdBy == QStringLiteral("testsegments"));
        CHECK(!s.updatedAt.isEmpty());
        CHECK(QJsonDocument::fromJson(s.derivedFrom.toUtf8()).object().value("tool").toString() == QStringLiteral("testsegments"));
    }
    // Ids stay stable through the metadata
    m.setSegments(segments);
    CHECK(m.getSegments()[3].id == 4);

    // A lower run threshold splits the noise burst out of the first clip
    t.minNonClipRunFields = 10;
    const SegmentsAnalysis b = analyseSegments(m, fullRange(m), t, 40e6, &stored);
    const QVector<TbcMetaData::Segment> split = deriveSegments(m, b, t, QStringLiteral("testsegments"));
    CHECK(split.size() == 6);
    CHECK(split[1].startField == 120 && split[1].endFieldExclusive == 140 && split[1].kind == QLatin1String("noise"));
    CHECK(split[2].startField == 140 && split[2].endFieldExclusive == 200 && split[2].kind == QLatin1String("clip"));
}

void testPresets()
{
    const SegmentsThresholds low = SegmentsThresholds::preset(SegmentSensitivity::Low);
    const SegmentsThresholds normal = SegmentsThresholds::preset(SegmentSensitivity::Normal);
    const SegmentsThresholds high = SegmentsThresholds::preset(SegmentSensitivity::High);
    CHECK(low.syncConfThreshold < normal.syncConfThreshold && normal.syncConfThreshold < high.syncConfThreshold);
    CHECK(low.minRunFields > normal.minRunFields && normal.minRunFields > high.minRunFields);
    CHECK(low.minClipFields > normal.minClipFields && normal.minClipFields > high.minClipFields);
    CHECK(normal.gapTolerance == SegmentsThresholds().gapTolerance);
}

// --- frames ---------------------------------------------------------------------------------

void checkTiling(const TbcMetaData &m, const QVector<qint32> &boundaries)
{
    // Segments tiling [0, n) must produce frame ranges tiling [1, numberOfFrames]
    qint32 nextFrame = 1;
    for (qint32 k = 0; k + 1 < boundaries.size(); k++) {
        TbcMetaData::Segment s;
        s.startField = boundaries[k];
        s.endFieldExclusive = boundaries[k + 1];
        qint32 start = 0, length = 0;
        const bool ok = segmentFrameRange(m, s, &start, &length);
        if (!ok) continue;   // no whole frame: contributes nothing
        CHECK(start == nextFrame);
        CHECK(length >= 1);
        nextFrame = start + length;
    }
    CHECK(nextFrame == m.getNumberOfFrames() + 1);
}

void testFrameRanges()
{
    TbcMetaData m;
    buildMetadata(m, 400);   // first field first: frame F = fields (2F-2, 2F-1) 0-based
    CHECK(m.getNumberOfFrames() == 200);
    CHECK(frameContainingField(m, 0) == 1 && frameContainingField(m, 1) == 1);
    CHECK(frameContainingField(m, 100) == 51 && frameContainingField(m, 101) == 51);
    CHECK(frameContainingField(m, 399) == 200);
    CHECK(frameContainingField(m, 400) == -1 && frameContainingField(m, -1) == -1);

    // A seam on a second field: the mixed frame belongs to the earlier segment
    TbcMetaData::Segment a;
    a.startField = 0; a.endFieldExclusive = 101;
    qint32 start = 0, length = 0;
    CHECK(segmentFrameRange(m, a, &start, &length) && start == 1 && length == 51);
    TbcMetaData::Segment b;
    b.startField = 101; b.endFieldExclusive = 250;
    CHECK(segmentFrameRange(m, b, &start, &length) && start == 52 && length == 74);   // frames 52..125
    TbcMetaData::Segment c;
    c.startField = 250; c.endFieldExclusive = 400;
    CHECK(segmentFrameRange(m, c, &start, &length) && start == 126 && length == 75);  // frames 126..200
    checkTiling(m, {0, 101, 250, 400});
    checkTiling(m, {0, 1, 2, 3, 400});          // one-field segments hold no whole frame
    checkTiling(m, {0, 200, 201, 400});

    // A one-field segment on a first field owns its frame (the next segment
    // starts on the second field, so its range begins at the following frame);
    // a one-field segment on a second field holds no frame at all
    TbcMetaData::Segment one;
    one.startField = 200; one.endFieldExclusive = 201;
    CHECK(segmentFrameRange(m, one, &start, &length) && start == 101 && length == 1);
    TbcMetaData::Segment after;
    after.startField = 201; after.endFieldExclusive = 400;
    CHECK(segmentFrameRange(m, after, &start, &length) && start == 102 && length == 99);
    TbcMetaData::Segment oneSecond;
    oneSecond.startField = 201; oneSecond.endFieldExclusive = 202;
    CHECK(!segmentFrameRange(m, oneSecond, &start, &length));

    // Second field first: the library pairs frames from the first field it finds
    TbcMetaData inverted;
    buildMetadata(inverted, 400, 0, false);
    CHECK(inverted.getNumberOfFrames() == 199);
    CHECK(frameContainingField(inverted, 0) == -1);      // the leading orphan second field
    CHECK(frameContainingField(inverted, 1) == 1 && frameContainingField(inverted, 2) == 1);
    TbcMetaData::Segment lead;
    lead.startField = 0; lead.endFieldExclusive = 100;
    CHECK(segmentFrameRange(inverted, lead, &start, &length) && start == 1 && length == 50); // frames 1..50 = fields 1..100
    checkTiling(inverted, {0, 100, 251, 400});
}

void testReportShape()
{
    TbcMetaData m;
    buildMetadata(m, 40);
    const FieldRange r = fullRange(m);
    const SegmentsAnalysis a = analyseSegments(m, r, SegmentsThresholds(), 40e6, nullptr);
    const QVector<TbcMetaData::Segment> segments = deriveSegments(m, a, SegmentsThresholds(), QStringLiteral("testsegments"));
    const QJsonObject report = buildReport(m, "/x/y.tbc.db", "sqlite", 0, 0, r, SegmentsThresholds(), a, nullptr,
                                           QJsonObject(), true, segments, QStringLiteral("derived"));
    CHECK(report.value("schemaVersion").toInt() == 1);
    CHECK(report.value("video").toObject().value("numberOfFields").toInt() == 40);
    CHECK(report.value("range").toObject().value("startField").toInt() == 0);
    CHECK(report.value("counts").toObject().contains("gap"));
    CHECK(report.value("counts").toObject().value("decoderEvents").toInt() == 0);
    CHECK(report.value("perField").toObject().value("syncConf").toArray().size() == 40);
    CHECK(!report.value("fieldData").toObject().value("enabled").toBool());
    CHECK(report.value("segmentsSource").toString() == QLatin1String("derived"));
    const QJsonArray segs = report.value("segments").toArray();
    CHECK(segs.size() == 1);
    CHECK(segs.at(0).toObject().value("startFrame").toInt() == 1);
    CHECK(segs.at(0).toObject().value("lengthFrames").toInt() == 20);
    CHECK(segs.at(0).toObject().value("kind").toString() == QLatin1String("clip"));
    CHECK(!QJsonDocument(report).toJson(QJsonDocument::Compact).isEmpty());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testCleanCapture();
    testRateFromMetadata();
    testGapSplitsSections();
    testFaultsAndSyncLoss();
    testSameParityRepeatIsAParityBreak();
    testDropoutStorm();
    testRolloverThroughJson();
    testRangeResolution();
    testAudioAlignEquivalence();
    testDecoderEventsMerge();
    testStoredMetricsDriveFieldDataEvents();
    testDeriveSegments();
    testPresets();
    testFrameRanges();
    testReportShape();
    std::cout << "testsegments: all checks passed\n";
    return 0;
}
