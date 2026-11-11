/******************************************************************************
 * main.cpp
 * tbc-audio-align - Audio alignment tool for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

#include <memory>

#include "audioalignmentdialog.h"
#include "headlessalign.h"
#include "tbc/logging.h"
#include "tbc/uistyle.h"
#include "tbc/wheelguard.h"
#include "tbc/buildinfo.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Headless automation path: decided before any QApplication exists so it
    // never needs a display.
    if (HeadlessAlign::requested(argc, argv)) {
        return HeadlessAlign::run(argc, argv);
    }

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    // --help and --version need no display, so they get a QCoreApplication
    // (parser.process() answers them and exits); anything else a QApplication
    bool informationOnly = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        informationOnly = informationOnly || argument == "-h" || argument == "-?" || argument == "--help"
                          || argument == "--help-all" || argument == "-v" || argument == "--version";
    }
    // The GUI gets the stock-theme application; its environment must be set
    // before construction.
    if (!informationOnly) {
        tbc::ui::prepareStockThemeEnvironment();
    }
    std::unique_ptr<QCoreApplication> app(informationOnly ? new QCoreApplication(argc, argv)
                                                          : new tbc::ui::ThemedApplication(argc, argv));

    // Set application name and version
    QCoreApplication::setApplicationName("tbc-audio-align");
    QCoreApplication::setApplicationVersion(TbcBuildInfo::versionLine());
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription(
                "tbc-audio-align - Audio alignment tool for ld-decode\n"
                "\n"
                "(c)2026 Simon Inns\n"
                "GPLv3 Open-Source - github: https://github.com/happycube/ld-decode");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    QCommandLineOption sourceDirectoryOption(QStringList() << "source-dir",
                                             QCoreApplication::translate("main", "Initial source directory used by browse dialogs"),
                                             QCoreApplication::translate("main", "path"));
    parser.addOption(sourceDirectoryOption);

    QCommandLineOption jsonOption(QStringList() << "json",
                                  QCoreApplication::translate("main", "Prefill metadata JSON input"),
                                  QCoreApplication::translate("main", "filename"));
    parser.addOption(jsonOption);

    QCommandLineOption inputFileOption(QStringList() << "input-file",
                                       QCoreApplication::translate("main", "Prefill input audio stream"),
                                       QCoreApplication::translate("main", "filename"));
    parser.addOption(inputFileOption);

    QCommandLineOption outputFileOption(QStringList() << "output-file",
                                        QCoreApplication::translate("main", "Prefill output audio stream"),
                                        QCoreApplication::translate("main", "filename"));
    parser.addOption(outputFileOption);

    QCommandLineOption rfVideoSampleRateOption(QStringList() << "rf-video-sample-rate-hz",
                                               QCoreApplication::translate("main", "Prefill RF video sample rate in Hz"),
                                               QCoreApplication::translate("main", "hz"));
    parser.addOption(rfVideoSampleRateOption);

    QCommandLineOption exportTrackFileOption(QStringList() << "export-track-file",
                                             QCoreApplication::translate("main", "Write aligned track details for tbc-analyse export auto-load"),
                                             QCoreApplication::translate("main", "filename"));
    parser.addOption(exportTrackFileOption);
    parser.addOption(QCommandLineOption("headless",
                                        QCoreApplication::translate("main", "Align without the dialog (see --headless --help)")));
    parser.addOption(QCommandLineOption("force-dark-theme",
                                        QCoreApplication::translate("main", "Pin the dark Fusion theme instead of following the system light/dark setting")));
    parser.addOption(QCommandLineOption("light-theme",
                                        QCoreApplication::translate("main", "Pin the light Fusion theme instead of following the system light/dark setting")));

    parser.process(*app);
    processStandardDebugOptions(parser);

    // Apply the stock theme: follow the OS light/dark colour scheme by
    // default (dark when unknown), or pin light/dark via the explicit flags.
    // The --help/--version QCoreApplication is not a ThemedApplication, and the
    // *ToApp helpers leave it alone.
    if (parser.isSet(QStringLiteral("light-theme"))) {
        tbc::ui::applyStockLightThemeToApp();
    } else if (parser.isSet(QStringLiteral("force-dark-theme"))) {
        tbc::ui::applyStockDarkThemeToApp();
    } else {
        tbc::ui::applyStockThemeFollowingSystemToApp();
    }
    // The wheel changes a field only once it is clicked or tabbed into
    tbc::ui::installWheelGuard();

    AudioAlignmentDialog dialog;
    if (parser.isSet(sourceDirectoryOption)) {
        dialog.setSourceDirectory(parser.value(sourceDirectoryOption));
    }
    if (parser.isSet(jsonOption)) {
        dialog.setDefaultJson(parser.value(jsonOption));
    }
    if (parser.isSet(inputFileOption)) {
        dialog.setDefaultInputFile(parser.value(inputFileOption));
    }
    if (parser.isSet(outputFileOption)) {
        dialog.setDefaultOutputFile(parser.value(outputFileOption));
    }
    if (parser.isSet(exportTrackFileOption)) {
        dialog.setExportTrackOutputFile(parser.value(exportTrackFileOption));
    }

    bool ok = false;
    if (parser.isSet(rfVideoSampleRateOption)) {
        const quint32 rfSampleRate = parser.value(rfVideoSampleRateOption).toUInt(&ok);
        if (ok && rfSampleRate > 0) {
            dialog.setDefaultRfVideoSampleRate(rfSampleRate);
        }
    }

    dialog.exec();
    return 0;
}
