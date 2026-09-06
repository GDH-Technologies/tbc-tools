/************************************************************************

    testsegments.cpp (field-walk tests; the metadata-tier tests live in src/library/tbc/testsegments)

    Unit tests for tbc-segments (metadata tier and field-data tier)
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

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QVector>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "fieldmetrics.h"
#include "processingpool.h"
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
void buildMetadata(TbcMetaData &m, qint32 fields, qint64 firstLoc = 0)
{
    m.clear();
    m.setVideoParameters(ntscParameters(fields));
    m.setIsFirstFieldFirst(true);
    for (qint32 i = 0; i < fields; i++) {
        TbcMetaData::Field f;
        f.seqNo = i + 1;
        f.isFirstField = (i % 2 == 0);
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

// --- field-data tier -------------------------------------------------------------

quint16 ire(double v)
{
    const double raw = kBlack + v * (kWhite - kBlack) / 100.0;
    return static_cast<quint16>(std::max(0.0, std::min(65535.0, raw)));
}

void writeSyntheticTbc(const QString &lumaPath, const QString &chromaPath, qint32 fields)
{
    QFile luma(lumaPath), chroma(chromaPath);
    CHECK(luma.open(QIODevice::WriteOnly) && chroma.open(QIODevice::WriteOnly));
    QRandomGenerator rng(1234);
    QVector<quint16> lumaField(kWidth * kHeight), chromaField(kWidth * kHeight);
    for (qint32 n = 0; n < fields; n++) {
        const bool noisy = n >= 120 && n < 140;
        const bool blank = n >= 200 && n < 220;
        const double picture = blank ? 2.0 : (n >= 60 ? 80.0 : 50.0);
        const bool burstOff = n >= 300 && n < 320;
        for (qint32 line = 0; line < kHeight; line++) {
            for (qint32 x = 0; x < kWidth; x++) {
                double level;
                if (x < 38) level = -40.0;                       // sync tip
                else if (x < 135) level = 0.0;                   // porch (burst lives in the chroma file)
                else level = picture;
                if (noisy) level = -40.0 + 140.0 * rng.generateDouble();
                lumaField[line * kWidth + x] = ire(level);
                double c = 0.0;
                if (x >= 76 && x < 112 && !burstOff) c = 20.0 * std::sin(x * 2.0 * M_PI / 4.0);
                chromaField[line * kWidth + x] = ire(c);
            }
        }
        luma.write(reinterpret_cast<const char *>(lumaField.constData()), lumaField.size() * 2);
        chroma.write(reinterpret_cast<const char *>(chromaField.constData()), chromaField.size() * 2);
    }
}

void testFieldWalk()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString luma = dir.filePath("synthetic.tbc");
    const QString chroma = dir.filePath("synthetic_chroma.tbc");
    const qint32 fields = 400;
    writeSyntheticTbc(luma, chroma, fields);

    TbcMetaData m;
    buildMetadata(m, fields);
    const FieldGeometry g = geometryFromParameters(m.getVideoParameters());
    CHECK(g.valid());

    FieldMetrics one, four;
    {
        FieldWalkPool pool(luma, chroma, 1, m, g);
        CHECK(pool.process(one));
    }
    {
        FieldWalkPool pool(luma, chroma, 4, m, g);
        CHECK(pool.process(four));
    }
    CHECK(one.enabled && one.hasBurst && one.lumaMeanIre.size() == fields);
    for (qint32 i = 0; i < fields; i++) {
        CHECK((std::isnan(one.fieldDiffIre[i]) && std::isnan(four.fieldDiffIre[i])) || one.fieldDiffIre[i] == four.fieldDiffIre[i]);
        CHECK(one.lumaMeanIre[i] == four.lumaMeanIre[i]);
    }
    CHECK(std::abs(one.lumaMeanIre[10] - 50.0) < 1.0);
    CHECK(std::abs(one.lumaMeanIre[70] - 80.0) < 1.0);
    CHECK(std::abs(one.fieldDiffIre[60] - 30.0) < 1.0);
    CHECK(one.fieldDiffIre[10] < 0.5 && std::isnan(one.fieldDiffIre[0]));
    CHECK(one.noiseIre[10] < 0.5 && one.noiseIre[130] > 10.0);
    CHECK(std::abs(one.syncTipDevIre[10]) < 1.0 && std::abs(one.blankingDevIre[10]) < 1.0);
    CHECK(one.burstAmpIre[10] > 30.0 && one.burstAmpIre[310] < 1.0);

    SegmentsThresholds t;
    const SegmentsAnalysis a = analyseSegments(m, fullRange(m), t, 40e6, &one);
    const SegmentEvent *scene = firstOfKind(a, "scene_change");
    CHECK(scene && scene->startField == 60);
    for (const SegmentEvent &e : a.events) {
        if (e.kind == QLatin1String("scene_change")) CHECK(!(e.startField > 121 && e.startField < 138));
    }
    const SegmentEvent *noise = firstOfKind(a, "noise");
    CHECK(noise && noise->startField == 120 && noise->endFieldExclusive == 140);
    const SegmentEvent *blank = firstOfKind(a, "blank_video");
    CHECK(blank && blank->startField == 200 && blank->endFieldExclusive == 220);
    const SegmentEvent *burst = firstOfKind(a, "no_burst");
    CHECK(burst && burst->startField == 300 && burst->endFieldExclusive == 320);

    const QJsonObject report = buildReport(m, "/x.tbc.db", "sqlite", 0, 0, fullRange(m), t, a, &one, QJsonObject(), true);
    CHECK(report.value("fieldData").toObject().value("enabled").toBool());
    CHECK(report.value("fieldMetrics").toObject().value("fieldDiffIre").toArray().size() == fields);
    CHECK(report.value("fieldMetrics").toObject().value("fieldDiffIre").toArray().at(0).isNull());

    // A TBC with a trailing surplus (the decoder flushed its metadata before the
    // last fields) is walked up to the metadata's count; a TBC holding fewer
    // fields than the metadata describes is refused.
    TbcMetaData shorterMeta;
    buildMetadata(shorterMeta, fields - 1);
    FieldMetrics prefix;
    FieldWalkPool surplus(luma, QString(), 1, shorterMeta, geometryFromParameters(shorterMeta.getVideoParameters()));
    CHECK(surplus.process(prefix));
    CHECK(prefix.enabled && prefix.lumaMeanIre.size() == fields - 1);
    CHECK(std::abs(prefix.lumaMeanIre[10] - 50.0) < 1.0);

    TbcMetaData longerMeta;
    buildMetadata(longerMeta, fields + 1);
    FieldMetrics unused;
    FieldWalkPool bad(luma, QString(), 1, longerMeta, geometryFromParameters(longerMeta.getVideoParameters()));
    CHECK(!bad.process(unused));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testFieldWalk();
    std::cout << "testsegmentswalk: all checks passed\n";
    return 0;
}
