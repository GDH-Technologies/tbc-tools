/******************************************************************************
 * processvbi-main.cpp
 * tbc-process-vbi - standalone VBI/VITS processing GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Harry Munday
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>

#include "processvbidialog.h"
#include "tbc/logging.h"
#include "tbc/uistyle.h"

int main(int argc, char *argv[])
{
    // Set 'binary mode' for stdin and stdout on Windows
    setBinaryMode();

    // Install the local debug message handler
    setDebug(true);
    qInstallMessageHandler(debugOutputHandler);

    tbc::ui::prepareStockThemeEnvironment();

    tbc::ui::ThemedApplication app(argc, argv);

    QCoreApplication::setApplicationName("tbc-process-vbi");
    QCoreApplication::setApplicationVersion(
        QString("tbc-tools - Branch: %1 / Commit: %2").arg(APP_BRANCH, APP_COMMIT));
    QCoreApplication::setOrganizationDomain("github.com");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "tbc-process-vbi - standalone GUI for ld-process-vbi (VBI + VITS processing)\n"
        "\n"
        "(c)2026 Harry Munday\n"
        "GPLv3 Open-Source - github: https://github.com/harrypm/tbc-tools");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add the standard debug options --debug and --quiet
    addStandardDebugOptions(parser);

    QCommandLineOption sourceDirectoryOption(QStringList() << "source-dir",
                                             QCoreApplication::translate(
                                                 "main", "Initial source directory used by browse dialogs"),
                                             QCoreApplication::translate("main", "path"));
    parser.addOption(sourceDirectoryOption);

    QCommandLineOption inputTbcOption(QStringList() << "input-tbc",
                                      QCoreApplication::translate(
                                          "main", "Prefill the input TBC file"),
                                      QCoreApplication::translate("main", "filename"));
    parser.addOption(inputTbcOption);

    QCommandLineOption outputMetadataOption(QStringList() << "output-metadata",
                                            QCoreApplication::translate(
                                                "main", "Prefill the output metadata file (defaults to the input's auto-detected sidecar)"),
                                            QCoreApplication::translate("main", "filename"));
    parser.addOption(outputMetadataOption);

    parser.addOption(QCommandLineOption("force-dark-theme",
                                        QCoreApplication::translate("main", "Force dark theme regardless of system settings (default; no-op)")));
    parser.addOption(QCommandLineOption("light-theme",
                                        QCoreApplication::translate("main", "Use the light Fusion theme instead of the stock dark theme")));

    parser.addPositionalArgument(
        "input",
        QCoreApplication::translate("main",
                                    "Optional input TBC file to preload"),
        "[input]");

    parser.process(app);
    processStandardDebugOptions(parser);

    // Apply the stock theme (dark by default, light via --light-theme).
    if (parser.isSet(QStringLiteral("light-theme"))) {
        app.applyStockLightTheme();
    } else {
        app.applyStockDarkTheme();
    }

    ProcessVbiDialog dialog;

    if (parser.isSet(sourceDirectoryOption)) {
        dialog.setSourceDirectory(parser.value(sourceDirectoryOption));
    }

    if (parser.isSet(inputTbcOption)) {
        dialog.setDefaultInputTbc(parser.value(inputTbcOption));
    }

    if (parser.isSet(outputMetadataOption)) {
        dialog.setDefaultOutputMetadata(parser.value(outputMetadataOption));
    }

    const QStringList positionalInputs = parser.positionalArguments();
    for (const QString &positionalInput : positionalInputs) {
        // Only preload the first positional argument as the TBC input.
        dialog.setDefaultInputTbc(positionalInput);
        break;
    }

    dialog.exec();
    return 0;
}
