/******************************************************************************
 * main.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "mainwindow.h"
#include "configuration.h"
#include <QApplication>
#include <QDebug>
#include <QtGlobal>
#include <QCommandLineParser>
#include <QLoggingCategory>
#include <QPixmap>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QtConcurrent/QtConcurrent>
#include <cstdio>

#include "framesnapshot.h"
#include "slideshowextract.h"
#include "tbcsource.h"

#include "tbc/logging.h"
#include "tbc/uistyle.h"
#include "tbc/buildinfo.h"
namespace {
// Parse "a,b,c,d" into four integers
bool parseFourInts(const QString &text, qint32 values[4])
{
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 4) return false;
    for (int i = 0; i < 4; i++) {
        bool ok = false;
        values[i] = parts[i].trimmed().toInt(&ok);
        if (!ok) return false;
    }
    return true;
}

// Framing, aspect, still-picture and upscale flags, shared by --save-frame and
// --extract-stills. Built-in defaults plus flags; tbc-analyse.ini is not read,
// so a script gets the same output wherever it runs.
bool parseSnapshotOptions(const QCommandLineParser &parser, FrameSnapshot::Options &options, QString *error)
{
    bool ok = false;
    const QString crop = parser.value(QStringLiteral("crop")).trimmed().toLower();
    if (crop == QLatin1String("full")) {
        options.framing = FrameSnapshot::Framing::Full;
    } else if (crop == QLatin1String("active") || crop.isEmpty()) {
        options.framing = FrameSnapshot::Framing::Active;
    } else {
        qint32 rect[4];
        if (!parseFourInts(crop, rect)) {
            *error = QStringLiteral("--crop takes full, active or x,y,w,h");
            return false;
        }
        options.framing = FrameSnapshot::Framing::Custom;
        options.customRect = QRect(rect[0], rect[1], rect[2], rect[3]);
    }
    if (parser.isSet(QStringLiteral("margins"))) {
        qint32 margins[4];
        if (!parseFourInts(parser.value(QStringLiteral("margins")), margins)) {
            *error = QStringLiteral("--margins takes left,top,right,bottom");
            return false;
        }
        options.marginLeft = margins[0];
        options.marginTop = margins[1];
        options.marginRight = margins[2];
        options.marginBottom = margins[3];
    }
    if (parser.isSet(QStringLiteral("aspect"))) {
        const QString aspect = parser.value(QStringLiteral("aspect"));
        const FrameSnapshot::AspectMode invalid = static_cast<FrameSnapshot::AspectMode>(-1);
        options.aspectMode = FrameSnapshot::aspectModeFromName(aspect, invalid);
        if (options.aspectMode == invalid) {
            *error = QStringLiteral("--aspect takes exact or viewer");
            return false;
        }
    }
    if (parser.isSet(QStringLiteral("best-of")) && parser.isSet(QStringLiteral("average-of"))) {
        *error = QStringLiteral("--best-of and --average-of are alternatives; give one");
        return false;
    }
    for (const QString &flag : {QStringLiteral("best-of"), QStringLiteral("average-of")}) {
        if (!parser.isSet(flag)) continue;
        options.searchRadius = parser.value(flag).toInt(&ok);
        if (!ok || options.searchRadius < 0) {
            *error = QStringLiteral("--%1 takes a frame count (0 = off)").arg(flag);
            return false;
        }
        options.stillMode = options.searchRadius == 0 ? FrameSnapshot::StillMode::Off
                            : flag == QLatin1String("best-of") ? FrameSnapshot::StillMode::Cleanest
                                                               : FrameSnapshot::StillMode::Average;
    }
    if (parser.isSet(QStringLiteral("upscale"))) {
        options.upscaleFactor = parser.value(QStringLiteral("upscale")).toInt(&ok);
        if (!ok || options.upscaleFactor < 1 || options.upscaleFactor > 4) {
            *error = QStringLiteral("--upscale takes 1, 2, 3 or 4");
            return false;
        }
    }
    if (parser.isSet(QStringLiteral("upscale-method"))) {
        options.upscaleMethod = parser.value(QStringLiteral("upscale-method"));
        if (!FrameSnapshot::isUpscaleMethodAvailable(options.upscaleMethod)) {
            QStringList names;
            for (const FrameSnapshot::UpscaleMethod &method : FrameSnapshot::upscaleMethods()) names << method.name;
            *error = QStringLiteral("--upscale-method takes one of: %1").arg(names.join(QStringLiteral(", ")));
            return false;
        }
    }
    return true;
}

// Loads a source for a headless run and configures its chroma decoder
bool loadHeadless(TbcSource &tbcSource, const QString &inputFileName, QString *error)
{
    bool loaded = false;
    QEventLoop loop;
    QObject::connect(&tbcSource, &TbcSource::finishedLoading, &loop, [&](bool success) {
        loaded = success;
        loop.quit();
    });
    tbcSource.loadSource(inputFileName);
    loop.exec();
    if (!loaded || !tbcSource.getIsSourceLoaded()) {
        *error = QStringLiteral("Could not load %1: %2").arg(inputFileName, tbcSource.getLastIOError());
        return false;
    }
    // MainWindow configures the chroma decoder by handing the loaded source's
    // configuration back through the Chroma Decoder dialog; do the same here.
    tbcSource.setChromaConfiguration(tbcSource.getPalConfiguration(), tbcSource.getNtscConfiguration());
    return true;
}

// Headless "Save frame as PNG": tbc-analyse --save-frame N -o out.png input.tbc
int runSaveFrame(const QCommandLineParser &parser, const QString &inputFileName)
{
    auto fail = [](const QString &message) {
        fprintf(stderr, "tbc-analyse: %s\n", qPrintable(message));
        return 1;
    };

    if (inputFileName.isEmpty()) return fail(QStringLiteral("--save-frame needs an input TBC file"));
    const QString outputFileName = parser.value(QStringLiteral("output"));
    if (outputFileName.isEmpty()) return fail(QStringLiteral("--save-frame needs -o/--output <file.png>"));

    bool ok = false;
    qint32 frameNumber = parser.value(QStringLiteral("save-frame")).toInt(&ok);
    if (!ok || frameNumber < 1) return fail(QStringLiteral("--save-frame needs a frame number (from 1)"));

    FrameSnapshot::Options options;
    QString optionsError;
    if (!parseSnapshotOptions(parser, options, &optionsError)) return fail(optionsError);

    TbcSource tbcSource;
    QString loadError;
    if (!loadHeadless(tbcSource, inputFileName, &loadError)) return fail(loadError);
    if (frameNumber > tbcSource.getNumberOfFrames()) {
        return fail(QStringLiteral("Frame %1 is past the end (%2 frames)").arg(frameNumber).arg(tbcSource.getNumberOfFrames()));
    }

    const TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    const QSize frameSize(tbcSource.getFrameWidth(), tbcSource.getFrameHeight());

    const bool search = options.stillMode != FrameSnapshot::StillMode::Off;
    QImage frameImage;
    if (search || parser.isSet(QStringLiteral("score-report"))) {
        FrameSnapshot::SearchInput input;
        input.tbcFilename = tbcSource.getCurrentSourceFilename();
        input.videoParameters = videoParameters;
        input.anchorFrame = frameNumber;
        input.radius = search ? options.searchRadius : FrameSnapshot::Options().searchRadius;
        input.cropRect = FrameSnapshot::outputRect(options, videoParameters, frameSize);
        input.firstFrame = qMax(1, frameNumber - input.radius);
        const qint32 lastFrame = qMin(tbcSource.getNumberOfFrames(), frameNumber + input.radius);
        const QVector<double> visibleDropouts = tbcSource.getVisibleDropOutGraphData();
        for (qint32 frame = input.firstFrame; frame <= lastFrame; frame++) {
            input.fieldNumbers.append(tbcSource.getFieldNumbersForFrame(frame));
            input.visibleDropouts.append(frame - 1 < visibleDropouts.size() ? visibleDropouts[frame - 1] : 0.0);
        }

        const FrameSnapshot::SearchResult result = FrameSnapshot::findStillFrames(input);
        if (!result.errorMessage.isEmpty()) return fail(result.errorMessage);

        if (parser.isSet(QStringLiteral("score-report"))) {
            QString errorMessage;
            if (!FrameSnapshot::writeScoreReport(parser.value(QStringLiteral("score-report")), result, &errorMessage)) {
                return fail(errorMessage);
            }
        }
        qint32 runLength = 0;
        for (const FrameSnapshot::FrameScore &score : result.scores) {
            if (score.inRun) runLength++;
        }
        printf("cleanest frame: %d (%lld of %d frames showing the same picture around frame %d are usable)\n",
               result.bestFrame, static_cast<long long>(result.eligibleFrames.size()), runLength, frameNumber);

        if (options.stillMode == FrameSnapshot::StillMode::Average) {
            frameImage = FrameSnapshot::averageFrames(result.eligibleFrames, [&tbcSource](qint32 frame) {
                tbcSource.load(frame, frame * 2 - 1);
                return tbcSource.getImage();
            }, nullptr, nullptr, result.alignments);
            qint32 alignedFrames = 0;
            for (const FrameSnapshot::FrameAlignment &alignment : result.alignments) {
                if (alignment.apply) alignedFrames++;
            }
            printf("averaged %lld frames (%d realigned)\n", static_cast<long long>(result.eligibleFrames.size()), alignedFrames);
        }
        if (search) frameNumber = result.bestFrame;
    }

    if (frameImage.isNull()) {
        tbcSource.load(frameNumber, frameNumber * 2 - 1);
        frameImage = tbcSource.getImage();
    }
    QString errorMessage;
    const QImage image = FrameSnapshot::process(frameImage, options, videoParameters, &errorMessage);
    if (image.isNull()) return fail(errorMessage);
    if (!image.save(outputFileName)) return fail(QStringLiteral("Could not write %1").arg(outputFileName));

    printf("saved frame %d as %s (%dx%d)\n", frameNumber, qPrintable(outputFileName), image.width(), image.height());
    return 0;
}

// Headless "Extract slideshow stills": tbc-analyse --extract-stills all -o DIR input.tbc
int runExtractStills(const QCommandLineParser &parser, const QString &inputFileName)
{
    auto fail = [](const QString &message) {
        fprintf(stderr, "tbc-analyse: %s\n", qPrintable(message));
        return 1;
    };

    if (inputFileName.isEmpty()) return fail(QStringLiteral("--extract-stills needs an input TBC file"));
    const QString outputDirectory = parser.value(QStringLiteral("output"));
    // With only --scan-report, the scan is all it does
    const bool scanOnly = outputDirectory.isEmpty() && parser.isSet(QStringLiteral("scan-report"));
    if (outputDirectory.isEmpty() && !scanOnly) {
        return fail(QStringLiteral("--extract-stills needs -o/--output <folder> (or just --scan-report <file>)"));
    }

    // Each photo is averaged by default
    FrameSnapshot::Options options;
    options.stillMode = FrameSnapshot::StillMode::Average;
    QString optionsError;
    if (!parseSnapshotOptions(parser, options, &optionsError)) return fail(optionsError);

    bool ok = true;
    double minHoldSeconds = 1.0;
    if (parser.isSet(QStringLiteral("min-hold"))) minHoldSeconds = parser.value(QStringLiteral("min-hold")).toDouble(&ok);
    if (!ok || minHoldSeconds <= 0.0) return fail(QStringLiteral("--min-hold takes a time in seconds"));

    TbcSource tbcSource;
    QString loadError;
    if (!loadHeadless(tbcSource, inputFileName, &loadError)) return fail(loadError);
    const qint32 frames = tbcSource.getNumberOfFrames();

    qint32 firstFrame = 1;
    qint32 lastFrame = frames;
    const QString range = parser.value(QStringLiteral("extract-stills")).trimmed().toLower();
    if (range != QLatin1String("all")) {
        const QStringList ends = range.split(QLatin1Char('-'));
        bool firstOk = false;
        bool lastOk = false;
        if (ends.size() == 2) {
            firstFrame = ends[0].toInt(&firstOk);
            lastFrame = ends[1].toInt(&lastOk);
        }
        if (!firstOk || !lastOk || firstFrame < 1 || lastFrame < firstFrame) {
            return fail(QStringLiteral("--extract-stills takes all or FIRST-LAST (frame numbers from 1)"));
        }
        if (lastFrame > frames) return fail(QStringLiteral("Frame %1 is past the end (%2 frames)").arg(lastFrame).arg(frames));
    }

    const QDir directory(outputDirectory);
    const QString stem = SlideshowExtract::fileStem(inputFileName);
    const QString manifestName = directory.filePath(stem + QStringLiteral("_stills.csv"));
    const bool existing = !scanOnly && (QFileInfo::exists(manifestName)
                          || !directory.entryList({stem + QStringLiteral("_still_*.png")}, QDir::Files).isEmpty());
    if (existing && !parser.isSet(QStringLiteral("overwrite"))) {
        return fail(QStringLiteral("%1 already holds stills from this tape; give --overwrite to replace them").arg(outputDirectory));
    }

    const TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    const QSize frameSize(tbcSource.getFrameWidth(), tbcSource.getFrameHeight());
    SlideshowExtract::CaptureInput input;
    input.options = options;
    input.scan.tbcFilename = tbcSource.getCurrentSourceFilename();
    input.scan.videoParameters = videoParameters;
    input.scan.cropRect = FrameSnapshot::outputRect(options, videoParameters, frameSize);
    input.scan.firstFrame = firstFrame;
    const QVector<double> visibleDropouts = tbcSource.getVisibleDropOutGraphData();
    for (qint32 frame = firstFrame; frame <= lastFrame; frame++) {
        input.scan.fieldNumbers.append(tbcSource.getFieldNumbersForFrame(frame));
        input.visibleDropouts.append(frame - 1 < visibleDropouts.size() ? visibleDropouts[frame - 1] : 0.0);
    }

    QElapsedTimer timer;
    timer.start();
    const SlideshowExtract::ScanResult scan = SlideshowExtract::scan(input.scan);
    if (!scan.errorMessage.isEmpty()) return fail(scan.errorMessage);
    const qint32 minHoldFrames = qMax(1, qRound(minHoldSeconds * SlideshowExtract::frameRate(videoParameters.system)));
    const QVector<SlideshowExtract::Hold> holds = SlideshowExtract::findHolds(scan.samples, firstFrame, minHoldFrames);
    qint32 photos = 0;
    for (const SlideshowExtract::Hold &hold : holds) {
        if (hold.kind == SlideshowExtract::HoldKind::Photo) photos++;
    }
    printf("scanned frames %d-%d in %.1f s: %d photos, %lld moving or unsteady\n", firstFrame, lastFrame,
           timer.elapsed() / 1000.0, photos, static_cast<long long>(holds.size() - photos));
    fflush(stdout);
    if (parser.isSet(QStringLiteral("scan-report"))) {
        QString reportError;
        if (!SlideshowExtract::writeScanReport(parser.value(QStringLiteral("scan-report")), scan.samples, firstFrame,
                                               holds, &reportError)) {
            return fail(reportError);
        }
    }

    if (scanOnly) return 0;
    if (!directory.mkpath(QStringLiteral("."))) return fail(QStringLiteral("Could not create %1").arg(outputDirectory));

    const bool includeMoving = parser.isSet(QStringLiteral("include-moving"));
    auto render = [&tbcSource](qint32 frame) {
        tbcSource.load(frame, frame * 2 - 1);
        return tbcSource.getImage();
    };
    QVector<SlideshowExtract::Still> stills;
    timer.restart();
    for (const SlideshowExtract::Hold &hold : holds) {
        if (hold.kind != SlideshowExtract::HoldKind::Photo && !includeMoving) continue;
        const SlideshowExtract::Capture capture = SlideshowExtract::captureHold(input, hold, render);
        if (capture.image.isNull()) return fail(capture.errorMessage);

        SlideshowExtract::Still still;
        still.index = stills.size() + 1;
        still.fileName = SlideshowExtract::stillFileName(stem, still.index, capture, options);
        still.hold = hold;
        still.captureFrame = capture.frame;
        still.framesAveraged = capture.framesAveraged;
        still.startTimecode = SlideshowExtract::frameTimecode(hold.first, videoParameters.system);
        still.durationSeconds = hold.length() / SlideshowExtract::frameRate(videoParameters.system);
        if (!capture.image.save(directory.filePath(still.fileName))) {
            return fail(QStringLiteral("Could not write %1").arg(directory.filePath(still.fileName)));
        }
        printf("%s  frames %d-%d  %s\n", qPrintable(still.fileName), hold.first, hold.last,
               qPrintable(SlideshowExtract::holdKindName(hold.kind)));
        fflush(stdout);
        stills.append(still);
    }
    QString manifestError;
    if (!SlideshowExtract::writeManifest(manifestName, stills, &manifestError)) return fail(manifestError);
    printf("saved %lld stills to %s in %.1f s\n", static_cast<long long>(stills.size()), qPrintable(outputDirectory),
           timer.elapsed() / 1000.0);
    return 0;
}

QIcon bundledApplicationIcon()
{
    QIcon icon;
    const QStringList iconResources = {
        QStringLiteral(":/icons/Graphics/16-analyse.png"),
        QStringLiteral(":/icons/Graphics/32-analyse.png"),
        QStringLiteral(":/icons/Graphics/64-analyse.png"),
        QStringLiteral(":/icons/Graphics/128-analyse.png"),
        QStringLiteral(":/icons/Graphics/256-analyse.png")
    };

    for (const QString &resource : iconResources) {
        QPixmap pixmap(resource);
        if (!pixmap.isNull()) {
            icon.addPixmap(pixmap);
        }
    }

    return icon;
}

QString resolvedExecutableDirectory(const char *argv0)
{
    if (!argv0 || argv0[0] == '\0') {
        return QDir::currentPath();
    }

    const QString rawPath = QString::fromLocal8Bit(argv0);
    const QFileInfo info(rawPath);
    if (info.isAbsolute()) {
        return info.absolutePath();
    }

    return QFileInfo(QDir::current().absoluteFilePath(rawPath)).absolutePath();
}

void prependEnvSearchPath(const char *envKey, const QStringList &paths)
{
    if (!envKey || paths.isEmpty()) {
        return;
    }

    QStringList normalizedPaths;
    for (const QString &path : paths) {
        const QString cleanPath = QDir::cleanPath(path.trimmed());
        if (!cleanPath.isEmpty() && !normalizedPaths.contains(cleanPath)) {
            normalizedPaths << cleanPath;
        }
    }
    if (normalizedPaths.isEmpty()) {
        return;
    }

    QStringList envEntries =
        QString::fromLocal8Bit(qgetenv(envKey)).split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (int i = normalizedPaths.size() - 1; i >= 0; --i) {
        if (!envEntries.contains(normalizedPaths.at(i))) {
            envEntries.prepend(normalizedPaths.at(i));
        }
    }

    qputenv(envKey, envEntries.join(QDir::listSeparator()).toLocal8Bit());
}

void configureBundledQtPluginPaths(int argc, char *argv[])
{
#if defined(Q_OS_LINUX)
    const QString exeDir = resolvedExecutableDirectory((argv && argc > 0) ? argv[0] : nullptr);
    QStringList pluginRoots;

    auto appendPluginRoot = [&pluginRoots](const QString &path) {
        if (path.isEmpty()) {
            return;
        }
        const QString cleanPath = QDir::cleanPath(path);
        if (QDir(cleanPath).exists() && !pluginRoots.contains(cleanPath)) {
            pluginRoots << cleanPath;
        }
    };

    appendPluginRoot(QDir(exeDir).filePath(QStringLiteral("../plugins")));
    appendPluginRoot(QDir(exeDir).filePath(QStringLiteral("plugins")));

    if (qEnvironmentVariableIsSet("APPDIR")) {
        const QString appDirRoot = qEnvironmentVariable("APPDIR");
        appendPluginRoot(QDir(appDirRoot).filePath(QStringLiteral("usr/plugins")));
        appendPluginRoot(QDir(appDirRoot).filePath(QStringLiteral("usr/lib/qt6/plugins")));
        appendPluginRoot(QDir(appDirRoot).filePath(QStringLiteral("usr/lib/plugins")));
    }

    if (pluginRoots.isEmpty()) {
        return;
    }

    QStringList platformPluginPaths;
    for (const QString &pluginRoot : pluginRoots) {
        const QString platformsDir = QDir(pluginRoot).filePath(QStringLiteral("platforms"));
        if (QDir(platformsDir).exists() && !platformPluginPaths.contains(platformsDir)) {
            platformPluginPaths << platformsDir;
        }
    }

    if (qEnvironmentVariable("QT_QPA_PLATFORM_PLUGIN_PATH").trimmed().isEmpty()
        && !platformPluginPaths.isEmpty()) {
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", platformPluginPaths.constFirst().toLocal8Bit());
    }

    prependEnvSearchPath("QT_PLUGIN_PATH", pluginRoots);
#else
    Q_UNUSED(argc)
    Q_UNUSED(argv)
#endif
}
} // namespace

// Custom message handler that filters out harmless Qt system warnings
void filteredDebugOutputHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Filter out harmless Qt system warnings that don't affect functionality
    if (msg.contains("QSocketNotifier: Can only be used with threads started with QThread")) {
        return; // Don't output these warnings
    }
    
    // Call the original handler for all other messages
    debugOutputHandler(type, context, msg);
}

int main(int argc, char *argv[])
{
    // Install the local debug message handler with Qt system warning filtering
    qInstallMessageHandler(filteredDebugOutputHandler);
#ifdef Q_OS_WIN
    // Prefer the native Schannel TLS backend for HTTPS requests in local
    // Windows builds. This avoids OpenSSL backend dependency issues from
    // blocking update checks when OpenSSL runtime DLLs are not staged.
    if (qEnvironmentVariableIsEmpty("QT_TLS_BACKEND")) {
        qputenv("QT_TLS_BACKEND", QByteArrayLiteral("schannel"));
    }
#endif
    configureBundledQtPluginPaths(argc, argv);

    // Set application name and version BEFORE anything reads the configuration
    // file or constructs the application. QStandardPaths::ConfigLocation - which
    // Configuration uses to locate tbc-analyse.ini - includes the application
    // name on Windows, so reading the UI scale below with the name still unset
    // would silently look in the wrong directory there.
    QCoreApplication::setApplicationName("tbc-analyse");
    QCoreApplication::setApplicationVersion(TbcBuildInfo::version());
    QCoreApplication::setOrganizationDomain("github.com");

    // Apply the saved UI scale. Qt has no runtime API for a manual global scale
    // factor, so this has to be in the environment before the application is
    // constructed. 0 means "follow the OS scale", and an operator who has set
    // QT_SCALE_FACTOR themselves keeps control of it. The saved theme is read
    // here too and applied once the command line is parsed.
    QString themeChoice;
    {
        Configuration startupConfiguration;
        const double uiScaleFactor = startupConfiguration.getUiScaleFactor();
        if (uiScaleFactor != 0.0 && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
            qputenv("QT_SCALE_FACTOR", QByteArray::number(uiScaleFactor));
        }
        themeChoice = startupConfiguration.getTheme();
    }

    // Qt 6 already defaults to PassThrough; stating it pins fractional desktop
    // scales (125%, 150%) against a future change of the platform default.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // --save-frame and --extract-stills never open a window, so they must not need a display
    for (int i = 1; i < argc; i++) {
        const QByteArray argument(argv[i]);
        if ((argument.startsWith("--save-frame") || argument.startsWith("--extract-stills"))
            && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
        }
    }

    QApplication a(argc, argv);

    // Set desktop file name for proper GNOME integration
    // This must match the installed .desktop file name (without .desktop extension)
    QGuiApplication::setDesktopFileName("tbc-analyse");
    
    // Set application icon (for window decorations and taskbar/dock).
    // QIcon::fromTheme works on desktops with an installed theme; otherwise use bundled assets.
    QIcon appIcon = QIcon::fromTheme(QStringLiteral("tbc-analyse"));
    if (appIcon.isNull()) {
        appIcon = bundledApplicationIcon();
    }
    if (!appIcon.isNull()) {
        a.setWindowIcon(appIcon);
    }

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "tbc-analyse - analysis & adjustment tool for the decode projects 4fsc TBC format\n"
        "\n"
        "(c)2018-2025 Simon Inns\n"
        "(c)2020-2022 Adam Sampson\n"
        "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    // Theme options. They override the Themes-menu choice for this session
    // only and are never saved.
    parser.addOption(QCommandLineOption("force-dark-theme", "Use the dark theme for this session"));
    parser.addOption(QCommandLineOption("light-theme", "Use the light theme for this session"));
    parser.addOption(QCommandLineOption("metadata-only", "Load metadata (.db or .json) without TBC data"));

    // Headless "Save frame as PNG"
    parser.addOption(QCommandLineOption("save-frame", "Save frame <N> as a PNG and exit, without opening a window", "N"));
    parser.addOption(QCommandLineOption({"o", "output"}, "PNG file written by --save-frame, or the folder for --extract-stills", "path"));
    parser.addOption(QCommandLineOption("crop", "--save-frame framing: full, active (default) or x,y,w,h", "framing"));
    parser.addOption(QCommandLineOption("margins", "--save-frame trims from the framing: left,top,right,bottom (default 0,0,0,12)", "l,t,r,b"));
    parser.addOption(QCommandLineOption("aspect", "--save-frame aspect: exact (square pixels, default) or viewer (tbc-analyse's DAR stretch)", "mode"));
    parser.addOption(QCommandLineOption("best-of", "--save-frame: save the cleanest frame within +/-N that shows the same picture (0 = off)", "N"));
    parser.addOption(QCommandLineOption("average-of", "--save-frame: save the average of the usable frames within +/-N that show the same picture (0 = off)", "N"));
    parser.addOption(QCommandLineOption("upscale", "--save-frame: upscale factor 1-4 (default 1)", "factor"));
    parser.addOption(QCommandLineOption("upscale-method", "--save-frame: resampling for the upscale and aspect correction "
                                                          "(default lanczos4; an invalid name lists this build's methods)", "method"));
    parser.addOption(QCommandLineOption("score-report", "--save-frame: write the still-picture search's per-frame figures as CSV", "file"));

    // Headless "Extract slideshow stills"; takes the --save-frame framing,
    // aspect, --best-of/--average-of (default --average-of 60) and upscale flags
    parser.addOption(QCommandLineOption("extract-stills", "Save every photo held on the tape (all, or frames FIRST-LAST) "
                                                          "to the -o folder with a stills.csv manifest, and exit", "range"));
    parser.addOption(QCommandLineOption("min-hold", "--extract-stills: shortest hold that counts as a photo, in seconds (default 1)", "seconds"));
    parser.addOption(QCommandLineOption("include-moving", "--extract-stills: also save pans, zooms and unsteady holds (their cleanest frame)"));
    parser.addOption(QCommandLineOption("overwrite", "--extract-stills: replace stills already in the -o folder"));
    parser.addOption(QCommandLineOption("scan-report", "--extract-stills: write the scan's per-frame figures and holds as CSV (without -o, only scan)", "file"));

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC or metadata file"));

    // Process the command line arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    // Qt's own Fusion theme. The saved Themes-menu choice (dark by default)
    // unless a command-line option overrides it for this session.
    if (parser.isSet("light-theme")) {
        themeChoice = QStringLiteral("light");
    } else if (parser.isSet("force-dark-theme")) {
        themeChoice = QStringLiteral("dark");
    }
    tbc::ui::applyFusionTheme(themeChoice == QLatin1String("light") ? Qt::ColorScheme::Light
                                                                    : Qt::ColorScheme::Dark);

    // Get the arguments from the parser
    QString inputFileName;
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 1) {
        inputFileName = positionalArguments.at(0);
    } else {
        inputFileName.clear();
    }
    if (parser.isSet("save-frame")) {
        return runSaveFrame(parser, inputFileName);
    }
    if (parser.isSet("extract-stills")) {
        return runExtractStills(parser, inputFileName);
    }

    const bool metadataOnly = parser.isSet("metadata-only");

    // Start the GUI application
    MainWindow w(inputFileName, metadataOnly, themeChoice);
    w.show();

    return a.exec();
}
