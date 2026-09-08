/************************************************************************

    main.cpp

    tbc-segments - Find recording boundaries in TBC metadata and fields
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

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>

#include "fieldmetrics.h"
#include "processingpool.h"
#include "segments.h"
#include "tbc/logging.h"
#include "tbcmetadata.h"

namespace {

bool parseDouble(const QCommandLineParser &parser, const QCommandLineOption &option, double *out,
                 double lo, double hi, QString *error)
{
    if (!parser.isSet(option)) return true;
    bool ok = false;
    const double v = parser.value(option).toDouble(&ok);
    if (!ok || v < lo || v > hi) {
        *error = QStringLiteral("Invalid --%1 value: %2").arg(option.names().constFirst(), parser.value(option));
        return false;
    }
    *out = v;
    return true;
}

bool parseInt(const QCommandLineParser &parser, const QCommandLineOption &option, qint32 *out,
              qint32 lo, qint32 hi, QString *error)
{
    if (!parser.isSet(option)) return true;
    bool ok = false;
    const qint32 v = parser.value(option).toInt(&ok);
    if (!ok || v < lo || v > hi) {
        *error = QStringLiteral("Invalid --%1 value: %2").arg(option.names().constFirst(), parser.value(option));
        return false;
    }
    *out = v;
    return true;
}

// The chroma TBC beside a luma one, paired the way ld-analyse pairs them when
// it opens a luma file (TbcSource::startBackgroundLoad, src/ld-analyse/
// tbcsource.cpp): a .tbcy/.ytbc luma takes the matching .tbcc/.ctbc, and a
// plain .tbc luma takes tbc-video-export's <stem>_chroma.tbc, vhs-decode's
// chroma_<stem>.tbc, or an alternate-extension sibling, in that order.
//
// Getting this wrong is quiet rather than loud: with no chroma found, the burst
// is measured from the luma TBC instead, which on a colour-under decode (VHS,
// Video8, S-Video) carries no burst and yields a plausible but meaningless
// number. Use --no-burst to skip the measurement deliberately.
QString defaultChromaFor(const QString &lumaPath)
{
    QStringList candidates;
    if (lumaPath.endsWith(QStringLiteral(".tbcy"), Qt::CaseInsensitive)) {
        candidates << lumaPath.left(lumaPath.length() - 5) + QStringLiteral(".tbcc");
    } else if (lumaPath.endsWith(QStringLiteral(".ytbc"), Qt::CaseInsensitive)) {
        candidates << lumaPath.left(lumaPath.length() - 5) + QStringLiteral(".ctbc");
    } else if (lumaPath.endsWith(QStringLiteral(".tbc"), Qt::CaseInsensitive)) {
        const QString stem = lumaPath.left(lumaPath.length() - 4);
        const QFileInfo info(lumaPath);
        const QString dir = info.path();
        const QString prefixDir = (dir.isEmpty() || dir == QStringLiteral(".")) ? QString()
                                                                               : dir + QLatin1Char('/');
        candidates << stem + QStringLiteral("_chroma.tbc")
                   << prefixDir + QStringLiteral("chroma_") + info.fileName()
                   << stem + QStringLiteral(".ctbc")
                   << stem + QStringLiteral(".tbcc");
    }
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return QString();
}

// Store a walk's metrics on the metadata's fields
void storeMetrics(TbcMetaData &metaData, const FieldMetrics &metrics)
{
    const qint32 n = std::min<qint32>(metaData.getNumberOfFields(), metrics.lumaMeanIre.size());
    for (qint32 i = 0; i < n; i++) {
        TbcMetaData::PictureMetrics p;
        p.lumaMeanIre = metrics.lumaMeanIre[i];
        p.fieldDiffIre = metrics.fieldDiffIre[i];
        p.blankingDevIre = metrics.blankingDevIre[i];
        p.syncTipDevIre = metrics.syncTipDevIre[i];
        p.noiseIre = metrics.noiseIre[i];
        p.burstAmpIre = metrics.hasBurst ? metrics.burstAmpIre[i] : std::numeric_limits<double>::quiet_NaN();
        metaData.updateFieldPictureMetrics(p, i + 1);
    }
}

// Compare walked metrics with the stored ones: max |stored - walked| per metric
// over the fields where both are finite. Returns the largest of them.
double verifyStored(const FieldMetrics &stored, const FieldMetrics &walked, QString *report)
{
    struct Column { const char *name; const QVector<double> *a; const QVector<double> *b; };
    const Column columns[] = {
        {"lumaMeanIre", &stored.lumaMeanIre, &walked.lumaMeanIre},
        {"fieldDiffIre", &stored.fieldDiffIre, &walked.fieldDiffIre},
        {"blankingDevIre", &stored.blankingDevIre, &walked.blankingDevIre},
        {"syncTipDevIre", &stored.syncTipDevIre, &walked.syncTipDevIre},
        {"noiseIre", &stored.noiseIre, &walked.noiseIre},
        {"burstAmpIre", &stored.burstAmpIre, &walked.burstAmpIre},
    };
    double worst = 0.0;
    for (const Column &c : columns) {
        double maxDiff = 0.0;
        qint32 compared = 0, storedOnly = 0, walkedOnly = 0;
        const qint32 n = std::min(c.a->size(), c.b->size());
        for (qint32 i = 0; i < n; i++) {
            const bool fa = std::isfinite((*c.a)[i]);
            const bool fb = std::isfinite((*c.b)[i]);
            if (fa && fb) {
                maxDiff = std::max(maxDiff, std::abs((*c.a)[i] - (*c.b)[i]));
                compared++;
            } else if (fa) {
                storedOnly++;
            } else if (fb) {
                walkedOnly++;
            }
        }
        worst = std::max(worst, maxDiff);
        *report += QStringLiteral("  %1: max |stored - walked| %2 IRE over %3 fields (stored only %4, walked only %5)\n")
                       .arg(QString::fromLatin1(c.name)).arg(maxDiff, 0, 'f', 3).arg(compared).arg(storedOnly).arg(walkedOnly);
    }
    return worst;
}

// Two decoder events are the same record if everything but their provenance
// matches. detailJson carries the build's APP_COMMIT, which changes between
// builds (and gains a "-dirty" suffix on a dirty tree), so comparing the text
// would report a difference on every rebuild and force a needless rewrite.
bool sameEvent(const TbcMetaData::DecoderEvent &a, const TbcMetaData::DecoderEvent &b)
{
    if (a.kind != b.kind || a.field != b.field || a.fileLoc != b.fileLoc
        || a.source != b.source || a.hasRfDeltaSamples != b.hasRfDeltaSamples
        || a.rfDeltaSamples != b.rfDeltaSamples) {
        return false;
    }
    // rfDeltaFields defaults to NaN, and NaN compares unequal to itself
    const bool aNan = std::isnan(a.rfDeltaFields), bNan = std::isnan(b.rfDeltaFields);
    if (aNan != bNan) return false;
    if (!aNan && !qFuzzyCompare(1.0 + a.rfDeltaFields, 1.0 + b.rfDeltaFields)) return false;

    QJsonObject aDetail = QJsonDocument::fromJson(a.detailJson.toUtf8()).object();
    QJsonObject bDetail = QJsonDocument::fromJson(b.detailJson.toUtf8()).object();
    aDetail.remove(QStringLiteral("commit"));
    bDetail.remove(QStringLiteral("commit"));
    return aDetail == bDetail;
}

// Reconstruct decoder events from the field records when the decoder stored
// none: gaps (with direction), skipped and duplicated fields. Rows from an
// earlier reconstruction are replaced; rows the decoder wrote are never touched.
// Returns whether the metadata was actually changed: an unchanged return keeps
// --write from rewriting the whole file to store rows it already holds.
bool reconstructEvents(TbcMetaData &metaData, const SegmentsAnalysis &analysis)
{
    QVector<TbcMetaData::DecoderEvent> kept;
    for (const TbcMetaData::DecoderEvent &event : metaData.getDecoderEvents()) {
        if (event.source == QLatin1String("decoder")) return false; // the decoder's own record stands
        if (event.source != QLatin1String("tbc-segments")) kept.append(event);
    }
    const double nominal = analysis.nominalSamplesPerField;
    const QString detailTail = QStringLiteral(",\"reconstructed\":true,\"tool\":\"tbc-segments\",\"commit\":\"%1\"}").arg(QStringLiteral(APP_COMMIT));
    for (const SegmentEvent &ev : analysis.events) {
        TbcMetaData::DecoderEvent de;
        de.source = QStringLiteral("tbc-segments");
        de.field = ev.startField;
        if (ev.kind == QLatin1String("gap")) {
            de.kind = QStringLiteral("gap");
            if (ev.detail.contains("deltaSamples")) {
                de.hasRfDeltaSamples = true;
                de.rfDeltaSamples = static_cast<qint64>(std::llround(ev.detail.value("deltaSamples").toDouble()));
                if (nominal > 0) de.rfDeltaFields = de.rfDeltaSamples / nominal;
            }
            const QString direction = ev.detail.value("direction").toString();
            de.detailJson = QStringLiteral("{\"direction\":\"%1\"").arg(direction.isEmpty() ? QStringLiteral("forward") : direction) + detailTail;
        } else if (ev.kind == QLatin1String("skipped_field")) {
            const TbcMetaData::Field &field = metaData.getField(ev.startField + 1);
            de.kind = (field.hasIsDuplicateField && field.isDuplicateField) ? QStringLiteral("duplicate_field")
                                                                              : QStringLiteral("skipped_field");
            de.fileLoc = field.fileLoc;
            de.detailJson = QStringLiteral("{\"fields\":%1").arg(ev.endFieldExclusive - ev.startField) + detailTail;
        } else {
            continue;
        }
        kept.append(de);
    }

    const QVector<TbcMetaData::DecoderEvent> &existing = metaData.getDecoderEvents();
    if (existing.size() == kept.size()
        && std::equal(existing.cbegin(), existing.cend(), kept.cbegin(), sameEvent)) {
        return false; // the stored rows already say this; leave the file alone
    }

    metaData.setDecoderEvents(kept);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    setBinaryMode();
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    QCoreApplication a(argc, argv);
    QCoreApplication::setApplicationName("tbc-segments");
    QCoreApplication::setApplicationVersion(QString("tbc-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("domesday86.com");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "tbc-segments - Find recording boundaries in TBC metadata and fields\n"
        "\n"
        "Reads .tbc.db (or legacy .tbc.json) metadata and reports gapless sections,\n"
        "boundary events (tape gaps, sync loss, parity breaks, skipped fields, dropout\n"
        "storms, the decoder's own seam events) and the recording segments derived from\n"
        "them. Per-field picture metrics stored by the decoder are used when present;\n"
        "--tbc walks the raw fields for them otherwise (luma, noise, blanking, sync-tip\n"
        "and burst metrics and the scene changes / noise / blank / no-burst runs they\n"
        "reveal). --write stores walked metrics, reconstructed events and (with\n"
        "--write-segments) the derived segments back into the metadata, SQLite first.\n"
        "Output is JSON (schema 1); fields are 0-based, seconds count from field 0.\n"
        "\n"
        "(c)2026 GDH-Technologies LLC\n"
        "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);

    QCommandLineOption jsonOption("json", QCoreApplication::translate("main", "Write the report to <file> ('-' = stdout, the default)"), "file", "-");
    QCommandLineOption startOption("start", QCoreApplication::translate("main", "First frame to analyse (1-based)"), "frame");
    QCommandLineOption lengthOption("length", QCoreApplication::translate("main", "Number of frames to analyse"), "frames");
    QCommandLineOption rfRateOption("rf-sample-rate-hz", QCoreApplication::translate("main", "RF capture sample rate; overrides the rate stored in the metadata (unset with none stored = self-calibrate from the median field delta)"), "Hz");
    QCommandLineOption sensitivityOption("sensitivity", QCoreApplication::translate("main", "Threshold preset: low, normal or high (default normal); individual options override it"), "preset", "normal");
    QCommandLineOption gapToleranceOption("gap-tolerance", QCoreApplication::translate("main", "Field-length deviation (fraction of nominal) that starts a new section (default 1/3, tbc-audio-align's rule)"), "fraction");
    QCommandLineOption syncConfOption("sync-conf-threshold", QCoreApplication::translate("main", "sync loss below this percentage of the file's median syncConf (default 50)"), "0-100");
    QCommandLineOption minRunOption("min-run-fields", QCoreApplication::translate("main", "Minimum run length for sync_loss / dropout_storm / field-data runs (default 2)"), "n");
    QCommandLineOption dropoutStormOption("dropout-storm-threshold", QCoreApplication::translate("main", "Active-area dropout coverage that counts as a storm (default 0.25)"), "fraction");
    QCommandLineOption tbcOption("tbc", QCoreApplication::translate("main", "Luma TBC to walk for the field metrics when the metadata holds none"), "file");
    QCommandLineOption chromaOption("chroma-tbc", QCoreApplication::translate("main", "Chroma TBC for burst amplitude (default: <luma>_chroma.tbc when present)"), "file");
    QCommandLineOption threadsOption(QStringList() << "t" << "threads", QCoreApplication::translate("main", "Worker threads for the field walk (default: logical CPUs)"), "n");
    QCommandLineOption sceneOption("scene-threshold-ire", QCoreApplication::translate("main", "Same-parity field difference marking a scene change (default 12)"), "IRE");
    QCommandLineOption noiseOption("noise-threshold-ire", QCoreApplication::translate("main", "Back-porch noise above which a field is snow (default 6)"), "IRE");
    QCommandLineOption blankOption("blank-luma-ire", QCoreApplication::translate("main", "Active luma at or below which a quiet field is blank (default 5)"), "IRE");
    QCommandLineOption minClipOption("min-clip-fields", QCoreApplication::translate("main", "A section shorter than this is 'unknown', never a clip (default 10)"), "n");
    QCommandLineOption minNonClipRunOption("min-non-clip-run-fields", QCoreApplication::translate("main", "A noise/blank run at least this long splits a section (default 50)"), "n");
    QCommandLineOption noBurstOption("no-burst", QCoreApplication::translate("main", "Do not measure burst amplitude, and do not read the chroma TBC for it (halves the walk's I/O; no no_burst events)"));
    QCommandLineOption forceWalkOption("force-walk", QCoreApplication::translate("main", "Walk the TBC even when the metadata already holds picture metrics"));
    QCommandLineOption verifyStoredOption("verify-stored", QCoreApplication::translate("main", "Walk the TBC and compare with the stored metrics; exit 1 above 0.05 IRE"));
    QCommandLineOption writeOption("write", QCoreApplication::translate("main", "Store walked metrics and reconstructed events in the metadata (the .tbc.db; a JSON-only decode gets one first and the JSON is refreshed)"));
    QCommandLineOption writeSegmentsOption("write-segments", QCoreApplication::translate("main", "With --write: also store the derived segments when the metadata holds none"));
    QCommandLineOption forceOption("force", QCoreApplication::translate("main", "With --write-segments: replace stored derived segments (user segments are always kept)"));
    QCommandLineOption perFieldOption("per-field", QCoreApplication::translate("main", "Include per-field arrays (syncConf, decodeFaults, dropout coverage and the field metrics)"));
    QCommandLineOption summaryOption("summary", QCoreApplication::translate("main", "Print a human summary (to stderr, so --json - stays pure)"));
    for (const QCommandLineOption *opt : {&jsonOption, &startOption, &lengthOption, &rfRateOption, &sensitivityOption,
                                          &gapToleranceOption, &syncConfOption, &minRunOption, &dropoutStormOption,
                                          &tbcOption, &chromaOption, &threadsOption, &sceneOption, &noiseOption, &blankOption,
                                          &minClipOption, &minNonClipRunOption, &noBurstOption, &forceWalkOption, &verifyStoredOption,
                                          &writeOption, &writeSegmentsOption, &forceOption, &perFieldOption, &summaryOption}) {
        parser.addOption(*opt);
    }
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Metadata file (.tbc.db or .tbc.json; a .tbc.json with a .tbc.db sibling opens the database)"));
    parser.process(a);
    processStandardDebugOptions(parser);

    const QStringList positional = parser.positionalArguments();
    if (positional.count() != 1) {
        qCritical("You must specify exactly one input metadata file (.tbc.db or .tbc.json)");
        return 1;
    }
    const QString requestedFilename = positional.constFirst();
    const QString inputFilename = TbcMetaData::resolveMetadataPath(requestedFilename);
    if (inputFilename != requestedFilename) {
        qInfo().noquote() << "Opening the SQLite metadata beside" << requestedFilename;
    }

    SegmentsThresholds thresholds;
    {
        const QString preset = parser.value(sensitivityOption).trimmed().toLower();
        if (preset == QLatin1String("low")) thresholds = SegmentsThresholds::preset(SegmentSensitivity::Low);
        else if (preset == QLatin1String("high")) thresholds = SegmentsThresholds::preset(SegmentSensitivity::High);
        else if (preset == QLatin1String("normal")) thresholds = SegmentsThresholds::preset(SegmentSensitivity::Normal);
        else {
            qCritical().noquote() << "Invalid --sensitivity value:" << preset << "(low, normal or high)";
            return 1;
        }
    }
    qint32 startFrame = 0, lengthFrames = 0, threads = QThread::idealThreadCount();
    double rfRate = 0.0;
    QString error;
    if (!parseInt(parser, startOption, &startFrame, 1, INT32_MAX, &error)
        || !parseInt(parser, lengthOption, &lengthFrames, 1, INT32_MAX, &error)
        || !parseDouble(parser, rfRateOption, &rfRate, 1.0, 1e12, &error)
        || !parseDouble(parser, gapToleranceOption, &thresholds.gapTolerance, 0.001, 0.999, &error)
        || !parseInt(parser, syncConfOption, &thresholds.syncConfThreshold, 0, 100, &error)
        || !parseInt(parser, minRunOption, &thresholds.minRunFields, 1, 100000, &error)
        || !parseDouble(parser, dropoutStormOption, &thresholds.dropoutStormThreshold, 0.0, 1.0, &error)
        || !parseInt(parser, threadsOption, &threads, 1, 1024, &error)
        || !parseDouble(parser, sceneOption, &thresholds.sceneThresholdIre, 0.0, 200.0, &error)
        || !parseDouble(parser, noiseOption, &thresholds.noiseThresholdIre, 0.0, 200.0, &error)
        || !parseDouble(parser, blankOption, &thresholds.blankLumaIre, -50.0, 200.0, &error)
        || !parseInt(parser, minClipOption, &thresholds.minClipFields, 1, 1000000, &error)
        || !parseInt(parser, minNonClipRunOption, &thresholds.minNonClipRunFields, 1, 1000000, &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    const bool wantWrite = parser.isSet(writeOption);
    const bool wantWriteSegments = parser.isSet(writeSegmentsOption);
    const bool wantVerify = parser.isSet(verifyStoredOption);
    if ((wantWriteSegments || parser.isSet(forceOption)) && !wantWrite) {
        qCritical("--write-segments and --force need --write");
        return 1;
    }
    if (wantVerify && !parser.isSet(tbcOption)) {
        qCritical("--verify-stored needs --tbc");
        return 1;
    }

    TbcMetaData metaData;
    if (!metaData.read(inputFilename)) {
        qCritical() << "Unable to read the metadata file";
        return 1;
    }
    if (metaData.getNumberOfFields() < 1) {
        qCritical() << "The metadata describes no fields";
        return 1;
    }
    const QString inputKind = TbcMetaData::isJsonMetadataFilename(inputFilename)
                                  ? QStringLiteral("json") : QStringLiteral("sqlite");

    FieldRange range;
    if (!resolveFieldRange(metaData, startFrame, lengthFrames, &range)) {
        qCritical() << "Invalid --start/--length for this metadata";
        return 1;
    }

    // Field metrics: stored with the metadata unless a walk is asked for
    FieldMetrics metrics = FieldMetrics::fromMetadata(metaData);
    const bool storedComplete = metrics.enabled && FieldMetrics::metadataIsComplete(metaData);
    QJsonObject fieldDataInfo;
    bool walked = false;
    if (metrics.enabled) {
        fieldDataInfo.insert("source", QStringLiteral("stored"));
        fieldDataInfo.insert("storedComplete", storedComplete);
    } else {
        fieldDataInfo.insert("source", QStringLiteral("none"));
    }

    const bool needWalk = parser.isSet(tbcOption)
                          && (wantVerify || parser.isSet(forceWalkOption) || !storedComplete);
    if (needWalk) {
        const bool noBurst = parser.isSet(noBurstOption);
        const QString luma = parser.value(tbcOption);
        // With --no-burst there is nothing the chroma TBC is needed for, so it
        // is never named and never opened.
        const QString chroma = noBurst ? QString()
                                       : (parser.isSet(chromaOption) ? parser.value(chromaOption) : defaultChromaFor(luma));
        if (noBurst && parser.isSet(chromaOption)) {
            qWarning() << "tbc-segments: --no-burst ignores --chroma-tbc";
        }
        FieldGeometry geometry = geometryFromParameters(metaData.getVideoParameters());
        geometry.skipBurst = noBurst;
        FieldWalkPool pool(luma, chroma, threads, metaData, geometry);
        FieldMetrics walkedMetrics;
        if (!pool.process(walkedMetrics)) return 1;
        fieldDataInfo.insert("lumaTbc", luma);
        fieldDataInfo.insert("chromaTbc", chroma.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(chroma));
        fieldDataInfo.insert("threads", threads);

        if (wantVerify && metrics.enabled) {
            QString report;
            const double worst = verifyStored(metrics, walkedMetrics, &report);
            qInfo().noquote() << QStringLiteral("Stored vs walked metrics:\n%1  worst: %2 IRE").arg(report).arg(worst, 0, 'f', 3);
            if (worst > 0.05) {
                qCritical().noquote() << QStringLiteral("Stored metrics disagree with the walk by %1 IRE").arg(worst, 0, 'f', 3);
                return 1;
            }
        } else if (wantVerify) {
            qCritical("--verify-stored: the metadata holds no picture metrics to compare");
            return 1;
        }
        metrics = walkedMetrics;
        walked = true;
        fieldDataInfo.insert("source", QStringLiteral("walked"));
    } else if (parser.isSet(tbcOption) && storedComplete) {
        qInfo() << "Metadata already holds picture metrics for every field; not walking the TBC (--force-walk overrides)";
    }

    const SegmentsAnalysis analysis = analyseSegments(metaData, range, thresholds, rfRate, metrics.enabled ? &metrics : nullptr);

    // Segments: the stored layer wins; derive otherwise
    QVector<TbcMetaData::Segment> segments = metaData.getSegments();
    QString segmentsSource = segments.isEmpty() ? QStringLiteral("derived") : QStringLiteral("stored");
    QVector<TbcMetaData::Segment> derived;
    if (segments.isEmpty() || (wantWriteSegments && parser.isSet(forceOption))) {
        derived = deriveSegments(metaData, analysis, thresholds, QStringLiteral("tbc-segments"));
        if (segments.isEmpty()) segments = derived;
    }

    if (wantWrite) {
        // Track what actually changed so the write can be narrowed to it. A
        // backfill never touches the field records, and rewriting 475,000 of
        // them to store the metrics is what makes --write cost hours on a
        // large capture.
        bool metricsDirty = false, captureDirty = false, eventsDirty = false, segmentsDirty = false;
        if (walked) {
            storeMetrics(metaData, metrics);
            metricsDirty = true;
        }
        if (rfRate > 0 && !(metaData.getVideoParameters().rfSourceSampleRateHz > 0)) {
            TbcMetaData::VideoParameters vp = metaData.getVideoParameters();
            vp.rfSourceSampleRateHz = rfRate;
            metaData.setVideoParameters(vp);
            captureDirty = true;
        }
        if (reconstructEvents(metaData, analysis)) eventsDirty = true;
        if (wantWriteSegments) {
            const bool hasStored = !metaData.getSegments().isEmpty();
            if (!hasStored) {
                metaData.setSegments(derived);
                segmentsSource = QStringLiteral("stored");
                segmentsDirty = true;
            } else if (parser.isSet(forceOption)) {
                // Replace derived segments, keep every user one
                QVector<TbcMetaData::Segment> merged;
                for (const TbcMetaData::Segment &s : metaData.getSegments()) {
                    if (s.source == QLatin1String("user")) merged.append(s);
                }
                for (const TbcMetaData::Segment &d : derived) {
                    bool overlapsUser = false;
                    for (const TbcMetaData::Segment &u : merged) {
                        if (d.startField < u.endFieldExclusive && u.startField < d.endFieldExclusive) { overlapsUser = true; break; }
                    }
                    if (!overlapsUser) {
                        TbcMetaData::Segment copy = d;
                        copy.id = -1; // setSegments assigns the next free id
                        merged.append(copy);
                    }
                }
                std::stable_sort(merged.begin(), merged.end(), [](const TbcMetaData::Segment &x, const TbcMetaData::Segment &y) {
                    return x.startField < y.startField;
                });
                metaData.setSegments(merged);
                segments = metaData.getSegments();
                segmentsDirty = true;
            } else {
                qInfo() << "Metadata already holds segments; not replacing them (--force overrides)";
            }
        }
        const bool changed = metricsDirty || captureDirty || eventsDirty || segmentsDirty;
        if (changed) {
            // The capture row is always written on an update: it is one row, and
            // it is what carries the schema migrations. The field records are
            // never ours to change, so leave them alone unless we are creating
            // the database (a JSON-only decode getting its first .tbc.db), which
            // writeSqlite detects for itself and writes in full.
            SqliteWriteScope scope;
            scope.fields = false;
            scope.pictureMetrics = metricsDirty;
            scope.decoderEvents = eventsDirty;
            scope.segments = segmentsDirty;

            QString canonical;
            if (!metaData.writeWithProjection(inputFilename, &canonical, scope)) {
                qCritical() << "Unable to write the metadata";
                return 1;
            }
            qInfo().noquote() << "Metadata written:" << canonical;
        } else {
            qInfo() << "Nothing to write";
        }
    }

    const QJsonObject report = buildReport(metaData, inputFilename, inputKind, startFrame, lengthFrames, range, thresholds,
                                           analysis, metrics.enabled ? &metrics : nullptr, fieldDataInfo,
                                           parser.isSet(perFieldOption), segments, segmentsSource);
    const QByteArray payload = QJsonDocument(report).toJson(QJsonDocument::Compact) + "\n";

    const QString jsonTarget = parser.value(jsonOption);
    if (jsonTarget == QLatin1String("-")) {
        std::cout.write(payload.constData(), payload.size());
        std::cout.flush();
    } else {
        QFile out(jsonTarget);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qCritical() << "Cannot write" << jsonTarget;
            return 1;
        }
        out.write(payload);
        out.close();
    }

    if (parser.isSet(summaryOption)) {
        qInfo().noquote() << summariseAnalysis(analysis, range)
                          << QStringLiteral("Segments: %1 (%2)").arg(segments.size()).arg(segmentsSource);
    }
    return 0;
}
