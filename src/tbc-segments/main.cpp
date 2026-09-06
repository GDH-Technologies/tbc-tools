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
#include <QThread>
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

QString defaultChromaFor(const QString &lumaPath)
{
    // tbc-video-export's convention: <stem>_chroma.tbc beside <stem>.tbc.
    if (!lumaPath.endsWith(QStringLiteral(".tbc"), Qt::CaseInsensitive)) return QString();
    const QString candidate = lumaPath.left(lumaPath.length() - 4) + QStringLiteral("_chroma.tbc");
    return QFileInfo::exists(candidate) ? candidate : QString();
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
        "Reads .tbc.db (or legacy .tbc.json) metadata and reports gapless sections and\n"
        "boundary events (tape gaps, sync loss, parity breaks, skipped fields, dropout\n"
        "storms). With --tbc it also walks the raw fields for luma, noise, blanking,\n"
        "sync-tip and burst metrics and the scene changes / noise / blank / no-burst runs\n"
        "they reveal. Output is JSON (schema 1); fields are 0-based, seconds count from\n"
        "field 0.\n"
        "\n"
        "(c)2026 GDH-Technologies LLC\n"
        "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);

    QCommandLineOption jsonOption("json", QCoreApplication::translate("main", "Write the report to <file> ('-' = stdout, the default)"), "file", "-");
    QCommandLineOption startOption("start", QCoreApplication::translate("main", "First frame to analyse (1-based)"), "frame");
    QCommandLineOption lengthOption("length", QCoreApplication::translate("main", "Number of frames to analyse"), "frames");
    QCommandLineOption rfRateOption("rf-sample-rate-hz", QCoreApplication::translate("main", "RF capture sample rate; unset = self-calibrate from the median field delta"), "Hz");
    QCommandLineOption gapToleranceOption("gap-tolerance", QCoreApplication::translate("main", "Field-length deviation (fraction of nominal) that starts a new section (default 0.333)"), "fraction");
    QCommandLineOption syncConfOption("sync-conf-threshold", QCoreApplication::translate("main", "sync loss below this percentage of the file's median syncConf (default 50)"), "0-100");
    QCommandLineOption minRunOption("min-run-fields", QCoreApplication::translate("main", "Minimum run length for sync_loss / dropout_storm / field-data runs (default 2)"), "n");
    QCommandLineOption dropoutStormOption("dropout-storm-threshold", QCoreApplication::translate("main", "Active-area dropout coverage that counts as a storm (default 0.25)"), "fraction");
    QCommandLineOption tbcOption("tbc", QCoreApplication::translate("main", "Also walk the raw fields of this luma TBC"), "file");
    QCommandLineOption chromaOption("chroma-tbc", QCoreApplication::translate("main", "Chroma TBC for burst amplitude (default: <luma>_chroma.tbc when present)"), "file");
    QCommandLineOption threadsOption(QStringList() << "t" << "threads", QCoreApplication::translate("main", "Worker threads for the field walk (default: logical CPUs)"), "n");
    QCommandLineOption sceneOption("scene-threshold-ire", QCoreApplication::translate("main", "Same-parity field difference marking a scene change (default 12)"), "IRE");
    QCommandLineOption noiseOption("noise-threshold-ire", QCoreApplication::translate("main", "Back-porch noise above which a field is snow (default 6)"), "IRE");
    QCommandLineOption blankOption("blank-luma-ire", QCoreApplication::translate("main", "Active luma at or below which a quiet field is blank (default 5)"), "IRE");
    QCommandLineOption perFieldOption("per-field", QCoreApplication::translate("main", "Include per-field arrays (syncConf, decodeFaults, dropout coverage and, with --tbc, the field metrics)"));
    QCommandLineOption summaryOption("summary", QCoreApplication::translate("main", "Print a human summary (to stderr, so --json - stays pure)"));
    for (const QCommandLineOption *opt : {&jsonOption, &startOption, &lengthOption, &rfRateOption, &gapToleranceOption,
                                          &syncConfOption, &minRunOption, &dropoutStormOption, &tbcOption, &chromaOption,
                                          &threadsOption, &sceneOption, &noiseOption, &blankOption, &perFieldOption, &summaryOption}) {
        parser.addOption(*opt);
    }
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Metadata file (.tbc.db or .tbc.json)"));
    parser.process(a);
    processStandardDebugOptions(parser);

    const QStringList positional = parser.positionalArguments();
    if (positional.count() != 1) {
        qCritical("You must specify exactly one input metadata file (.tbc.db or .tbc.json)");
        return 1;
    }
    const QString inputFilename = positional.constFirst();

    SegmentsThresholds thresholds;
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
        || !parseDouble(parser, blankOption, &thresholds.blankLumaIre, -50.0, 200.0, &error)) {
        qCritical().noquote() << error;
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
    const QString inputKind = inputFilename.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
                                  ? QStringLiteral("json") : QStringLiteral("sqlite");

    FieldRange range;
    if (!resolveFieldRange(metaData, startFrame, lengthFrames, &range)) {
        qCritical() << "Invalid --start/--length for this metadata";
        return 1;
    }

    FieldMetrics metrics;
    QJsonObject fieldDataInfo;
    if (parser.isSet(tbcOption)) {
        const QString luma = parser.value(tbcOption);
        const QString chroma = parser.isSet(chromaOption) ? parser.value(chromaOption) : defaultChromaFor(luma);
        FieldGeometry geometry = geometryFromParameters(metaData.getVideoParameters());
        FieldWalkPool pool(luma, chroma, threads, metaData, geometry);
        if (!pool.process(metrics)) return 1;
        fieldDataInfo.insert("lumaTbc", luma);
        fieldDataInfo.insert("chromaTbc", chroma.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(chroma));
        fieldDataInfo.insert("threads", threads);
    }

    const SegmentsAnalysis analysis = analyseSegments(metaData, range, thresholds, rfRate, metrics.enabled ? &metrics : nullptr);
    const QJsonObject report = buildReport(metaData, inputFilename, inputKind, startFrame, lengthFrames, range, thresholds,
                                           analysis, metrics.enabled ? &metrics : nullptr, fieldDataInfo, parser.isSet(perFieldOption));
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
        qInfo().noquote() << summariseAnalysis(analysis, range);
    }
    return 0;
}
