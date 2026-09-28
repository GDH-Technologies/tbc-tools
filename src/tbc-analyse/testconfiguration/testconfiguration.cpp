/******************************************************************************
 * testconfiguration.cpp
 * tbc-analyse - Configuration unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Reece Dodge
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

#include "configuration.h"

// Release builds define NDEBUG, so a bare assert() would check nothing.
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "     \
                      << #condition << "\n";                                \
            std::exit(1);                                                   \
        }                                                                   \
    } while (false)

namespace {

const QString kVersion = QStringLiteral("1.0.1-gdh");
const QString kReleaseTag = QStringLiteral("local-test");
const QString kInstallPath = QStringLiteral("/tmp/tbc-tools-testconfiguration-plugin");

// The Plugin Manager records an install through its own short-lived
// Configuration, while MainWindow holds one for the whole session and writes
// it back on every settings change and in its destructor. That long-lived
// copy was loaded before the install, so it must not overwrite the record.
void testInstallSurvivesLongLivedWriter()
{
    std::cerr << "Testing that a CUDA plugin install survives a stale writer\n";

    Configuration mainWindow;
    CHECK(mainWindow.getCudaPluginInstalledVersion().isEmpty());

    {
        Configuration installer;
        installer.setCudaPluginInstalledVersion(kVersion);
        installer.setCudaPluginReleaseTag(kReleaseTag);
        installer.setCudaPluginEnabled(true);
        installer.setCudaPluginTrusted(true);
        installer.setCudaPluginInstallPath(kInstallPath);
        installer.writeConfiguration();
    }

    // MainWindow saving its own settings, as it does when tbc-analyse quits.
    mainWindow.setSourceDirectory(QDir::tempPath());
    mainWindow.writeConfiguration();

    Configuration reader;
    CHECK(reader.getCudaPluginInstalledVersion() == kVersion);
    CHECK(reader.getCudaPluginReleaseTag() == kReleaseTag);
    CHECK(reader.getCudaPluginEnabled());
    CHECK(reader.getCudaPluginTrusted());
    CHECK(reader.getCudaPluginInstallPath() == kInstallPath);
    // The long-lived writer's own change must still land.
    CHECK(reader.getSourceDirectory() == QDir::tempPath());
}

// The same race in reverse: a MainWindow loaded while the plugin was installed
// must not resurrect the record after the Plugin Manager removes it.
void testRemovalSurvivesLongLivedWriter()
{
    std::cerr << "Testing that a CUDA plugin removal survives a stale writer\n";

    Configuration mainWindow;
    CHECK(mainWindow.getCudaPluginInstalledVersion() == kVersion);

    {
        Configuration remover;
        remover.setCudaPluginInstalledVersion(QString());
        remover.setCudaPluginReleaseTag(QString());
        remover.setCudaPluginSha256(QString());
        remover.setCudaPluginEnabled(false);
        remover.setCudaPluginTrusted(false);
        remover.setCudaPluginInstallPath(QString());
        remover.writeConfiguration();
    }

    mainWindow.writeConfiguration();

    Configuration reader;
    CHECK(reader.getCudaPluginInstalledVersion().isEmpty());
    CHECK(reader.getCudaPluginReleaseTag().isEmpty());
    CHECK(!reader.getCudaPluginEnabled());
    CHECK(!reader.getCudaPluginTrusted());
    CHECK(reader.getCudaPluginInstallPath().isEmpty());
}

// A settings file from before stillMode existed has only the bestFrameSearch
// flag; "on" must carry over as the cleanest-frame mode, and the first write
// replaces the flag with stillMode.
void testFrameSnapshotStillModeMigration(const QString &configFile)
{
    std::cerr << "Testing that bestFrameSearch migrates to stillMode\n";

    {
        QSettings legacy(configFile, QSettings::IniFormat);
        legacy.setValue("frameSnapshot/bestFrameSearch", true);
        legacy.remove("frameSnapshot/stillMode");
        legacy.remove("frameSnapshot/marginBottom");
        legacy.sync();
    }

    Configuration reader;
    const FrameSnapshot::Options options = reader.getFrameSnapshotOptions();
    CHECK(options.stillMode == FrameSnapshot::StillMode::Cleanest);
    CHECK(options.marginBottom == 12);

    reader.writeConfiguration();
    QSettings written(configFile, QSettings::IniFormat);
    CHECK(!written.contains("frameSnapshot/bestFrameSearch"));
    CHECK(written.value("frameSnapshot/stillMode").toString() == QLatin1String("cleanest"));
}

// The Themes-menu choice: dark by default, a saved choice round-trips, and any
// other value falls back to dark. Adding the key must not have wiped the rest
// of an existing file (it is additive; SETTINGSVERSION is unchanged).
void testThemeChoice(const QString &configFile)
{
    std::cerr << "Testing the saved theme choice\n";

    {
        Configuration fresh;
        CHECK(fresh.getTheme() == QLatin1String("dark"));
        fresh.setTheme(QStringLiteral("light"));
        fresh.writeConfiguration();
    }
    {
        Configuration reader;
        CHECK(reader.getTheme() == QLatin1String("light"));
        CHECK(reader.getSourceDirectory() == QDir::tempPath());
    }
    for (const QString &other : {QStringLiteral("system"), QStringLiteral("purple")}) {
        {
            QSettings corrupt(configFile, QSettings::IniFormat);
            corrupt.setValue("viewOptions/theme", other);
            corrupt.sync();
        }
        Configuration reader;
        CHECK(reader.getTheme() == QLatin1String("dark"));
    }
}

// The field timing scope geometry and the waveform phosphor mode moved from a
// stray QSettings into Configuration: empty and off by default, round-tripped,
// and (being additive keys) no wipe of an existing file.
void testScopeSettings()
{
    std::cerr << "Testing the scope settings keys\n";

    const QByteArray geometry("field-timing-geometry");
    {
        Configuration fresh;
        CHECK(fresh.getFieldTimingDialogGeometry().isEmpty());
        CHECK(!fresh.getWaveformPhosphorMode());
        fresh.setFieldTimingDialogGeometry(geometry);
        fresh.setWaveformPhosphorMode(true);
        fresh.writeConfiguration();
    }
    {
        Configuration reader;
        CHECK(reader.getFieldTimingDialogGeometry() == geometry);
        CHECK(reader.getWaveformPhosphorMode());
        CHECK(reader.getSourceDirectory() == QDir::tempPath());
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    // Test mode points ConfigLocation at a throwaway directory, so this never
    // reads or writes the user's real tbc-analyse.ini.
    QStandardPaths::setTestModeEnabled(true);
    const QString configDirectory = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configFile = QDir(configDirectory).filePath(QStringLiteral("tbc-analyse.ini"));
    QDir().mkpath(configDirectory);
    QFile::remove(configFile);

    testInstallSurvivesLongLivedWriter();
    testRemovalSurvivesLongLivedWriter();
    testFrameSnapshotStillModeMigration(configFile);
    testThemeChoice(configFile);
    testScopeSettings();

    QFile::remove(configFile);
    std::cerr << "All configuration tests passed\n";
    return 0;
}
