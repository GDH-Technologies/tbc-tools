/******************************************************************************
 * headlessalign.cpp
 * tbc-audio-align - Headless (no GUI) audio alignment entry point
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "headlessalign.h"

#include <atomic>
#include <csignal>
#include <cstring>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include "audioalignmentutil.h"
#include "tbc/buildinfo.h"
#include "tbc/logging.h"

namespace {

std::atomic<bool> g_cancelRequested{false};

extern "C" void onTerminationSignal(int)
{
    g_cancelRequested.store(true);
}

void installSignalHandlers()
{
    std::signal(SIGINT, onTerminationSignal);
    std::signal(SIGTERM, onTerminationSignal);
}

void printError(const QString &message)
{
    QTextStream(stderr) << "Error: " << message << Qt::endl;
}

bool writeResult(const QString &target, const QJsonObject &result)
{
    const QByteArray payload = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (target == QStringLiteral("-")) {
        QTextStream(stdout) << payload;
        return true;
    }
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        printError(QStringLiteral("Unable to write result JSON: %1").arg(target));
        return false;
    }
    return file.write(payload) == payload.size();
}

} // namespace

namespace HeadlessAlign {

bool requested(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::strcmp(argv[i], "--headless") == 0) {
            return true;
        }
    }
    return false;
}

int run(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("tbc-audio-align"));
    QCoreApplication::setApplicationVersion(TbcBuildInfo::versionLine());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "tbc-audio-align --headless - align one audio stream to a TBC's field timing\n"
        "without opening the dialog. Intended for automation.\n"
        "\n"
        "Exit codes: 0 aligned, 1 alignment failed, 2 usage error,\n"
        "3 alignment runtime (VhsDecodeAutoAudioAlign / mono) unavailable."));
    parser.addHelpOption();
    parser.addVersionOption();
    addStandardDebugOptions(parser);

    parser.addOption(QCommandLineOption(QStringLiteral("headless"),
        QStringLiteral("Run without a GUI (required for this mode).")));
    const QCommandLineOption jsonOption(QStringLiteral("json"),
        QStringLiteral("Metadata JSON of the decode (.tbc.json)."), QStringLiteral("filename"));
    const QCommandLineOption inputOption(QStringLiteral("input-file"),
        QStringLiteral("Input audio stream (.wav/.flac)."), QStringLiteral("filename"));
    const QCommandLineOption outputOption(QStringLiteral("output-file"),
        QStringLiteral("Aligned output (.flac)."), QStringLiteral("filename"));
    const QCommandLineOption rfRateOption(QStringLiteral("rf-video-sample-rate-hz"),
        QStringLiteral("RF video capture sample rate in Hz (required; never inferred from the "
                       "decoded sample rate)."),
        QStringLiteral("hz"));
    const QCommandLineOption overwriteOption(QStringLiteral("overwrite"),
        QStringLiteral("Replace an existing output file."));
    const QCommandLineOption keepMonoOption(QStringLiteral("keep-mono"),
        QStringLiteral("Keep a mono input mono instead of duplicating it to stereo."));
    const QCommandLineOption resultJsonOption(QStringLiteral("result-json"),
        QStringLiteral("Write a JSON result summary to this path (\"-\" for stdout)."),
        QStringLiteral("filename"));
    parser.addOption(jsonOption);
    parser.addOption(inputOption);
    parser.addOption(outputOption);
    parser.addOption(rfRateOption);
    parser.addOption(overwriteOption);
    parser.addOption(keepMonoOption);
    parser.addOption(resultJsonOption);

    if (!parser.parse(QCoreApplication::arguments())) {
        printError(parser.errorText());
        return UsageError;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QTextStream(stdout) << parser.helpText();
        return Ok;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        QTextStream(stdout) << QCoreApplication::applicationName() << " "
                            << QCoreApplication::applicationVersion() << Qt::endl;
        return Ok;
    }
    processStandardDebugOptions(parser);

    QStringList missing;
    for (const QCommandLineOption *option : {&jsonOption, &inputOption, &outputOption, &rfRateOption}) {
        if (!parser.isSet(*option)) {
            missing << QStringLiteral("--") + option->names().constFirst();
        }
    }
    if (!missing.isEmpty()) {
        printError(QStringLiteral("missing required option(s): %1").arg(missing.join(QStringLiteral(", "))));
        return UsageError;
    }

    bool rateOk = false;
    const quint32 rfRateHz = parser.value(rfRateOption).toUInt(&rateOk);
    if (!rateOk || rfRateHz == 0) {
        printError(QStringLiteral("--rf-video-sample-rate-hz must be a positive integer, got \"%1\"")
                       .arg(parser.value(rfRateOption)));
        return UsageError;
    }

    const QString jsonPath = parser.value(jsonOption);
    const QString inputPath = parser.value(inputOption);
    const QString outputPath = parser.value(outputOption);
    const bool keepMono = parser.isSet(keepMonoOption);
    const QString resultTarget = parser.value(resultJsonOption);

    QJsonObject result{
        {QStringLiteral("ok"), false},
        {QStringLiteral("json"), QFileInfo(jsonPath).absoluteFilePath()},
        {QStringLiteral("input"), QFileInfo(inputPath).absoluteFilePath()},
        {QStringLiteral("output"), QFileInfo(outputPath).absoluteFilePath()},
        {QStringLiteral("rfVideoSampleRateHz"), static_cast<qint64>(rfRateHz)},
        {QStringLiteral("keepMono"), keepMono},
        {QStringLiteral("toolVersion"), QCoreApplication::applicationVersion()},
    };
    auto finish = [&](int code, const QString &error) {
        result.insert(QStringLiteral("exitCode"), code);
        if (!error.isEmpty()) {
            result.insert(QStringLiteral("error"), error);
            printError(error);
        }
        if (!resultTarget.isEmpty() && !writeResult(resultTarget, result) && code == Ok) {
            return static_cast<int>(AlignFailed);
        }
        return code;
    };

    QString runtimeError;
    if (AudioAlignmentUtil::audioAlignRunnerCommand(&runtimeError).isEmpty()) {
        return finish(RuntimeUnavailable, runtimeError.isEmpty()
                          ? QStringLiteral("VhsDecodeAutoAudioAlign runtime not available")
                          : runtimeError);
    }

    installSignalHandlers();
    QElapsedTimer timer;
    timer.start();
    int lastPercent = -1;
    const auto progress = [&lastPercent](int percent, const QString &message) {
        if (percent == lastPercent) {
            return;
        }
        lastPercent = percent;
        QTextStream(stderr) << "Progress: " << percent << "% " << message << Qt::endl;
    };
    const auto cancelled = []() { return g_cancelRequested.load(); };

    QString alignError;
    const bool ok = AudioAlignmentUtil::runStreamAlign(jsonPath, inputPath, outputPath, rfRateHz,
                                                       parser.isSet(overwriteOption), progress,
                                                       cancelled, &alignError, !keepMono);
    result.insert(QStringLiteral("elapsedMs"), static_cast<qint64>(timer.elapsed()));
    if (!ok) {
        return finish(AlignFailed, alignError.isEmpty() ? QStringLiteral("alignment failed") : alignError);
    }
    const QFileInfo outputInfo(outputPath);
    if (!outputInfo.isFile() || outputInfo.size() <= 0) {
        return finish(AlignFailed, QStringLiteral("aligner reported success but wrote no output: %1")
                                       .arg(outputInfo.absoluteFilePath()));
    }
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("outputBytes"), outputInfo.size());
    return finish(Ok, QString());
}

} // namespace HeadlessAlign
