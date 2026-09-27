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
#include <QStyleFactory>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QtConcurrent/QtConcurrent>
#include <cstdio>

#include "framesnapshot.h"
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

    // Built-in defaults plus flags; tbc-analyse.ini is not read, so a script
    // gets the same output wherever it runs.
    FrameSnapshot::Options options;
    const QString crop = parser.value(QStringLiteral("crop")).trimmed().toLower();
    if (crop == QLatin1String("full")) {
        options.framing = FrameSnapshot::Framing::Full;
    } else if (crop == QLatin1String("active") || crop.isEmpty()) {
        options.framing = FrameSnapshot::Framing::Active;
    } else {
        qint32 rect[4];
        if (!parseFourInts(crop, rect)) return fail(QStringLiteral("--crop takes full, active or x,y,w,h"));
        options.framing = FrameSnapshot::Framing::Custom;
        options.customRect = QRect(rect[0], rect[1], rect[2], rect[3]);
    }
    if (parser.isSet(QStringLiteral("margins"))) {
        qint32 margins[4];
        if (!parseFourInts(parser.value(QStringLiteral("margins")), margins)) {
            return fail(QStringLiteral("--margins takes left,top,right,bottom"));
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
        if (options.aspectMode == invalid) return fail(QStringLiteral("--aspect takes exact or viewer"));
    }
    if (parser.isSet(QStringLiteral("best-of")) && parser.isSet(QStringLiteral("average-of"))) {
        return fail(QStringLiteral("--best-of and --average-of are alternatives; give one"));
    }
    for (const QString &flag : {QStringLiteral("best-of"), QStringLiteral("average-of")}) {
        if (!parser.isSet(flag)) continue;
        options.searchRadius = parser.value(flag).toInt(&ok);
        if (!ok || options.searchRadius < 0) return fail(QStringLiteral("--%1 takes a frame count (0 = off)").arg(flag));
        if (options.searchRadius > 0) {
            options.stillMode = flag == QLatin1String("best-of") ? FrameSnapshot::StillMode::Cleanest
                                                                 : FrameSnapshot::StillMode::Average;
        }
    }
    if (parser.isSet(QStringLiteral("upscale"))) {
        options.upscaleFactor = parser.value(QStringLiteral("upscale")).toInt(&ok);
        if (!ok || options.upscaleFactor < 1 || options.upscaleFactor > 4) {
            return fail(QStringLiteral("--upscale takes 1, 2, 3 or 4"));
        }
    }
    if (parser.isSet(QStringLiteral("upscale-method"))) {
        options.upscaleMethod = parser.value(QStringLiteral("upscale-method"));
        if (!FrameSnapshot::isUpscaleMethodAvailable(options.upscaleMethod)) {
            QStringList names;
            for (const FrameSnapshot::UpscaleMethod &method : FrameSnapshot::upscaleMethods()) names << method.name;
            return fail(QStringLiteral("--upscale-method takes one of: %1").arg(names.join(QStringLiteral(", "))));
        }
    }

    TbcSource tbcSource;
    bool loaded = false;
    QEventLoop loop;
    QObject::connect(&tbcSource, &TbcSource::finishedLoading, &loop, [&](bool success) {
        loaded = success;
        loop.quit();
    });
    tbcSource.loadSource(inputFileName);
    loop.exec();
    if (!loaded || !tbcSource.getIsSourceLoaded()) {
        return fail(QStringLiteral("Could not load %1: %2").arg(inputFileName, tbcSource.getLastIOError()));
    }
    if (frameNumber > tbcSource.getNumberOfFrames()) {
        return fail(QStringLiteral("Frame %1 is past the end (%2 frames)").arg(frameNumber).arg(tbcSource.getNumberOfFrames()));
    }

    // MainWindow configures the chroma decoder by handing the loaded source's
    // configuration back through the Chroma Decoder dialog; do the same here.
    tbcSource.setChromaConfiguration(tbcSource.getPalConfiguration(), tbcSource.getNtscConfiguration());

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
    if (msg.contains("Wayland does not support QWindow::requestActivate()") ||
        msg.contains("QSocketNotifier: Can only be used with threads started with QThread")) {
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
    // QT_SCALE_FACTOR themselves keeps control of it.
    {
        Configuration startupConfiguration;
        const double uiScaleFactor = startupConfiguration.getUiScaleFactor();
        if (uiScaleFactor != 0.0 && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
            qputenv("QT_SCALE_FACTOR", QByteArray::number(uiScaleFactor));
        }
    }

    // Qt 6 already defaults to PassThrough; stating it pins fractional desktop
    // scales (125%, 150%) against a future change of the platform default.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    tbc::ui::prepareStockThemeEnvironment();

    // --save-frame never opens a window, so it must not need a display
    for (int i = 1; i < argc; i++) {
        if (QByteArray(argv[i]).startsWith("--save-frame") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
        }
    }

    tbc::ui::ThemedApplication a(argc, argv);

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

    // Theme options. Stock default is Fusion + dark (absolute; re-asserted on
    // macOS appearance switchover by ThemedApplication). --light-theme opts
    // into the light palette; --force-dark-theme is kept as a back-compat
    // no-op (dark is now the default).
    parser.addOption(QCommandLineOption("force-dark-theme", "Force dark theme regardless of system settings (default; no-op)"));
    parser.addOption(QCommandLineOption("light-theme", "Use the light Fusion theme instead of the stock dark theme"));
    parser.addOption(QCommandLineOption("metadata-only", "Load metadata (.db or .json) without TBC data"));

    // Headless "Save frame as PNG"
    parser.addOption(QCommandLineOption("save-frame", "Save frame <N> as a PNG and exit, without opening a window", "N"));
    parser.addOption(QCommandLineOption({"o", "output"}, "PNG file written by --save-frame", "file"));
    parser.addOption(QCommandLineOption("crop", "--save-frame framing: full, active (default) or x,y,w,h", "framing"));
    parser.addOption(QCommandLineOption("margins", "--save-frame trims from the framing: left,top,right,bottom (default 0,0,0,12)", "l,t,r,b"));
    parser.addOption(QCommandLineOption("aspect", "--save-frame aspect: exact (square pixels, default) or viewer (tbc-analyse's DAR stretch)", "mode"));
    parser.addOption(QCommandLineOption("best-of", "--save-frame: save the cleanest frame within +/-N that shows the same picture (0 = off)", "N"));
    parser.addOption(QCommandLineOption("average-of", "--save-frame: save the average of the usable frames within +/-N that show the same picture (0 = off)", "N"));
    parser.addOption(QCommandLineOption("upscale", "--save-frame: upscale factor 1-4 (default 1)", "factor"));
    parser.addOption(QCommandLineOption("upscale-method", "--save-frame: resampling for the upscale and aspect correction "
                                                          "(default lanczos4; an invalid name lists this build's methods)", "method"));
    parser.addOption(QCommandLineOption("score-report", "--save-frame: write the still-picture search's per-frame figures as CSV", "file"));

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", QCoreApplication::translate("main", "Specify input TBC or metadata file"));

    // Process the command line arguments given by the user
    parser.process(a);

    // Standard logging options
    processStandardDebugOptions(parser);

    // Apply the stock theme (dark by default, light via --light-theme). This
    // sets the Fusion palette, the isDarkTheme app property, the Qt 6.8 color
    // scheme override, and the input-widget contrast guard. ThemedApplication
    // re-asserts it on any ApplicationPaletteChange (e.g. macOS switchover).
    if (parser.isSet("light-theme")) {
        a.applyStockLightTheme();
    } else {
        a.applyStockDarkTheme();
    }

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

    const bool metadataOnly = parser.isSet("metadata-only");

    // Start the GUI application
    MainWindow w(inputFileName, metadataOnly);
    w.show();

    return a.exec();
}
