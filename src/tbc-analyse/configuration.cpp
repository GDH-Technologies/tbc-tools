/******************************************************************************
 * configuration.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "configuration.h"
#include "tbc/logging.h"

#include <QFile>
#include <QRect>
#include <algorithm>

// This define should be incremented if the settings file format changes.
// Purely additive keys must NOT bump it: a bump runs setDefault() and discards
// every existing user setting. New keys are read with a default instead.
static const qint32 SETTINGSVERSION = 5;

// Every per-purpose last-used-directory slot, so the read, write and default
// paths iterate one list instead of repeating each key three times.
static const char *DIRECTORY_PURPOSES[] = {
    DirectoryPurpose::Export,   DirectoryPurpose::AudioTrack, DirectoryPurpose::Metadata,
    DirectoryPurpose::Profile,  DirectoryPurpose::Efm,        DirectoryPurpose::Teletext,
    DirectoryPurpose::Plugin,
};

// Bounds for the manual UI scale factor. 0 means "follow the OS scale".
static const double UI_SCALE_MINIMUM = 0.5;
static const double UI_SCALE_MAXIMUM = 3.0;

// Frame snapshot options in the current group. Both "Save frame as PNG" and
// "Extract slideshow stills" keep a set.
static void writeSnapshotOptions(QSettings *configuration, const FrameSnapshot::Options &options)
{
    configuration->setValue("framing", FrameSnapshot::framingName(options.framing));
    configuration->setValue("marginLeft", options.marginLeft);
    configuration->setValue("marginTop", options.marginTop);
    configuration->setValue("marginRight", options.marginRight);
    configuration->setValue("marginBottom", options.marginBottom);
    configuration->setValue("customRect", options.customRect);
    configuration->setValue("aspectMode", FrameSnapshot::aspectModeName(options.aspectMode));
    configuration->setValue("stillMode", FrameSnapshot::stillModeName(options.stillMode));
    configuration->setValue("searchRadius", options.searchRadius);
    configuration->setValue("upscaleFactor", options.upscaleFactor);
    configuration->setValue("upscaleMethod", options.upscaleMethod);
}

// Additive keys: a missing key takes its value from defaults
static FrameSnapshot::Options readSnapshotOptions(QSettings *configuration, const FrameSnapshot::Options &defaults)
{
    FrameSnapshot::Options options;
    options.framing = FrameSnapshot::framingFromName(configuration->value("framing").toString(), defaults.framing);
    options.marginLeft = std::max(0, configuration->value("marginLeft", defaults.marginLeft).toInt());
    options.marginTop = std::max(0, configuration->value("marginTop", defaults.marginTop).toInt());
    options.marginRight = std::max(0, configuration->value("marginRight", defaults.marginRight).toInt());
    options.marginBottom = std::max(0, configuration->value("marginBottom", defaults.marginBottom).toInt());
    options.customRect = configuration->value("customRect", defaults.customRect).toRect();
    options.aspectMode = FrameSnapshot::aspectModeFromName(configuration->value("aspectMode").toString(), defaults.aspectMode);
    options.stillMode = FrameSnapshot::stillModeFromName(configuration->value("stillMode").toString(), defaults.stillMode);
    options.searchRadius = std::clamp(configuration->value("searchRadius", defaults.searchRadius).toInt(), 1, 600);
    options.upscaleFactor = std::clamp(configuration->value("upscaleFactor", defaults.upscaleFactor).toInt(), 1, 4);
    // Not checked against this build's methods; FrameSnapshot::process falls back
    options.upscaleMethod = configuration->value("upscaleMethod", defaults.upscaleMethod).toString();
    return options;
}

Configuration::Configuration(QObject *parent) : QObject(parent)
{
    // Open the application's configuration file
    QString configurationPath;
    QString configurationFileName;

    configurationPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) ;
    configurationFileName = "tbc-analyse.ini" ;

    // Migrate settings from the pre-rename "ld-analyse.ini" if the new settings
    // file does not exist yet (the old file is left in place as a backup)
    const QString newIniPath = configurationPath + "/" + configurationFileName;
    const QString legacyIniPath = configurationPath + "/ld-analyse.ini";
    if (!QFile::exists(newIniPath) && QFile::exists(legacyIniPath)) {
        QFile::copy(legacyIniPath, newIniPath);
    }

    configuration = new QSettings(newIniPath, QSettings::IniFormat);

    // Read the configuration
    readConfiguration();

    // Are the configuration settings valid?
    if (settings.version != SETTINGSVERSION) {
        tbcDebugStream() << "Configuration::Configuration(): Configuration invalid or wrong version ("
                         << settings.version << "!=" << SETTINGSVERSION << "). Setting to default values";

        // Set default configuration
        setDefault();
    }
}

Configuration::~Configuration()
{
    delete configuration;
}

void Configuration::writeConfiguration(void)
{
    // Write the valid configuration flag
    configuration->setValue("version", settings.version);

    // Directories
    configuration->beginGroup("directories");
    configuration->setValue("sourceDirectory", settings.directories.sourceDirectory);
    configuration->setValue("pngDirectory", settings.directories.pngDirectory);
    for (const char *purpose : DIRECTORY_PURPOSES) {
        configuration->setValue(purpose, settings.directories.lastUsed.value(QString::fromLatin1(purpose)));
    }
    configuration->endGroup();

    // Windows
    configuration->beginGroup("windows");
    configuration->setValue("mainWindowGeometry", settings.windows.mainWindowGeometry);
    configuration->setValue("mainWindowScaleFactor", settings.windows.mainWindowScaleFactor);
    configuration->setValue("vbiDialogGeometry", settings.windows.vbiDialogGeometry);
    configuration->setValue("oscilloscopeDialogGeometry", settings.windows.oscilloscopeDialogGeometry);
    configuration->setValue("vectorscopeDialogGeometry", settings.windows.vectorscopeDialogGeometry);
    configuration->setValue("waveformMonitorDialogGeometry", settings.windows.waveformMonitorDialogGeometry);
    configuration->setValue("dropoutAnalysisDialogGeometry", settings.windows.dropoutAnalysisDialogGeometry);
    configuration->setValue("visibleDropoutAnalysisDialogGeometry", settings.windows.visibleDropoutAnalysisDialogGeometry);
    configuration->setValue("blackSnrAnalysisDialogGeometry", settings.windows.blackSnrAnalysisDialogGeometry);
    configuration->setValue("whiteSnrAnalysisDialogGeometry", settings.windows.whiteSnrAnalysisDialogGeometry);
    configuration->setValue("closedCaptionDialogGeometry", settings.windows.closedCaptionDialogGeometry);
    configuration->setValue("videoParametersDialogGeometry", settings.windows.videoParametersDialogGeometry);
    configuration->setValue("chromaDecoderConfigDialogGeometry", settings.windows.chromaDecoderConfigDialogGeometry);
    configuration->setValue("fieldTimingDialogGeometry", settings.windows.fieldTimingDialogGeometry);
    configuration->endGroup();

    // View options
    configuration->beginGroup("viewOptions");
    configuration->setValue("toggleChromaDuringSeek", settings.viewOptions.toggleChromaDuringSeek);
    configuration->setValue("skipBySegments", settings.viewOptions.skipBySegments);
    configuration->setValue("waveformPhosphorMode", settings.viewOptions.waveformPhosphorMode);
    configuration->setValue("theme", settings.viewOptions.theme);
    configuration->setValue("generateProxyEnabled", settings.viewOptions.generateProxyEnabled);
    configuration->setValue("exportProfileConfigEnabled", settings.viewOptions.exportProfileConfigEnabled);
    configuration->setValue("exportProfileConfigPath", settings.viewOptions.exportProfileConfigPath);
    configuration->setValue("resizeFrameWithWindow", settings.viewOptions.resizeFrameWithWindow);
    configuration->setValue("showExportBoundary", settings.viewOptions.showExportBoundary);
    configuration->setValue("exportBoundaryThickness", settings.viewOptions.exportBoundaryThickness);
    configuration->setValue("uiScaleFactor", settings.viewOptions.uiScaleFactor);
    configuration->setValue("exportBoundaryThicknessUserSet", settings.viewOptions.exportBoundaryThicknessUserSet);
    configuration->endGroup();

    // Update checker
    configuration->beginGroup("updateCheck");
    configuration->setValue("enabled", settings.updateCheck.enabled);
    configuration->setValue("lastCheckTimestamp", settings.updateCheck.lastCheckTimestamp);
    configuration->setValue("skippedVersion", settings.updateCheck.skippedVersion);
    configuration->endGroup();

    // The CUDA plugin registry is not written here. Its setters write through
    // (see writeCudaPluginKey), so only the Configuration that changed a key
    // ever writes it. MainWindow's long-lived instance is loaded at startup and
    // written on every settings change and at exit; writing the group here let
    // its stale copy erase an install or removal the Plugin Manager recorded.

    // VBI processing options
    configuration->beginGroup("vbiProcessing");
    configuration->setValue("vbiCore", settings.vbiProcessing.vbiCore);
    configuration->setValue("ntsc", settings.vbiProcessing.ntsc);
    configuration->setValue("vitc", settings.vbiProcessing.vitc);
    configuration->setValue("closedCaptions", settings.vbiProcessing.closedCaptions);
    configuration->setValue("teletext", settings.vbiProcessing.teletext);
    configuration->setValue("vits", settings.vbiProcessing.vits);
    configuration->setValue("teletextHtmlDir", settings.vbiProcessing.teletextHtmlDir);
    configuration->setValue("teletextTapeFormat", settings.vbiProcessing.teletextTapeFormat);
    configuration->setValue("teletextMinDuplicates", settings.vbiProcessing.teletextMinDuplicates);
    configuration->endGroup();

    // "Save frame as PNG" options
    configuration->beginGroup("frameSnapshot");
    writeSnapshotOptions(configuration, settings.frameSnapshot);
    configuration->remove("bestFrameSearch"); // replaced by stillMode
    configuration->endGroup();

    // "Extract slideshow stills" options
    configuration->beginGroup("slideshowExtract");
    writeSnapshotOptions(configuration, settings.slideshowExtract.snapshot);
    configuration->setValue("minHoldSeconds", settings.slideshowExtract.minHoldSeconds);
    configuration->setValue("outputDirectory", settings.slideshowExtract.outputDirectory);
    configuration->setValue("dialogGeometry", settings.slideshowExtract.dialogGeometry);
    configuration->setValue("dialogColumns", settings.slideshowExtract.dialogColumns);
    configuration->setValue("dialogRows", settings.slideshowExtract.dialogRows);
    configuration->endGroup();

    // Sync the settings with disk
    tbcDebugStream() << "Configuration::writeConfiguration(): Writing configuration to disk";
    configuration->sync();
}

void Configuration::readConfiguration(void)
{
    tbcDebugStream() << "Configuration::readConfiguration(): Reading configuration from" << configuration->fileName();

    // Read the valid configuration flag
    settings.version = configuration->value("version").toInt();

    // Directories
    configuration->beginGroup("directories");
    settings.directories.sourceDirectory = configuration->value("sourceDirectory").toString();
    settings.directories.pngDirectory = configuration->value("pngDirectory").toString();
    // Additive keys - older config files fall back to an empty (unremembered) slot
    settings.directories.lastUsed.clear();
    for (const char *purpose : DIRECTORY_PURPOSES) {
        settings.directories.lastUsed.insert(QString::fromLatin1(purpose),
                                             configuration->value(purpose, QString()).toString());
    }
    configuration->endGroup();

    // Windows
    configuration->beginGroup("windows");
    settings.windows.mainWindowGeometry = configuration->value("mainWindowGeometry").toByteArray();
    settings.windows.mainWindowScaleFactor = configuration->value("mainWindowScaleFactor").toReal();
    settings.windows.vbiDialogGeometry = configuration->value("vbiDialogGeometry").toByteArray();
    settings.windows.oscilloscopeDialogGeometry = configuration->value("oscilloscopeDialogGeometry").toByteArray();
    settings.windows.vectorscopeDialogGeometry = configuration->value("vectorscopeDialogGeometry").toByteArray();
    settings.windows.waveformMonitorDialogGeometry = configuration->value("waveformMonitorDialogGeometry").toByteArray();
    settings.windows.dropoutAnalysisDialogGeometry = configuration->value("dropoutAnalysisDialogGeometry").toByteArray();
    settings.windows.visibleDropoutAnalysisDialogGeometry = configuration->value("visibleDropoutAnalysisDialogGeometry").toByteArray();
    settings.windows.blackSnrAnalysisDialogGeometry = configuration->value("blackSnrAnalysisDialogGeometry").toByteArray();
    settings.windows.whiteSnrAnalysisDialogGeometry = configuration->value("whiteSnrAnalysisDialogGeometry").toByteArray();
    settings.windows.closedCaptionDialogGeometry = configuration->value("closedCaptionDialogGeometry").toByteArray();
    settings.windows.videoParametersDialogGeometry = configuration->value("videoParametersDialogGeometry").toByteArray();
    settings.windows.chromaDecoderConfigDialogGeometry = configuration->value("chromaDecoderConfigDialogGeometry").toByteArray();
    settings.windows.fieldTimingDialogGeometry = configuration->value("fieldTimingDialogGeometry").toByteArray();
    configuration->endGroup();

    // View options
    configuration->beginGroup("viewOptions");
    settings.viewOptions.toggleChromaDuringSeek = configuration->value("toggleChromaDuringSeek", false).toBool();
    settings.viewOptions.skipBySegments = configuration->value("skipBySegments", true).toBool();
    settings.viewOptions.waveformPhosphorMode = configuration->value("waveformPhosphorMode", false).toBool();
    settings.viewOptions.theme = configuration->value("theme", QStringLiteral("dark")).toString();
    if (settings.viewOptions.theme != QLatin1String("light")) {
        settings.viewOptions.theme = QStringLiteral("dark");
    }
    settings.viewOptions.generateProxyEnabled = configuration->value("generateProxyEnabled", false).toBool();
    settings.viewOptions.exportProfileConfigEnabled = configuration->value("exportProfileConfigEnabled", false).toBool();
    settings.viewOptions.exportProfileConfigPath = configuration->value("exportProfileConfigPath", QString()).toString();
    settings.viewOptions.resizeFrameWithWindow = configuration->value("resizeFrameWithWindow", true).toBool();
    settings.viewOptions.showExportBoundary = configuration->value("showExportBoundary", true).toBool();
    settings.viewOptions.exportBoundaryThickness = configuration->value("exportBoundaryThickness", 2).toInt();
    settings.viewOptions.exportBoundaryThicknessUserSet = configuration->value("exportBoundaryThicknessUserSet", false).toBool();
    if (settings.viewOptions.exportBoundaryThickness < 1) settings.viewOptions.exportBoundaryThickness = 1;
    if (settings.viewOptions.exportBoundaryThickness > 8) settings.viewOptions.exportBoundaryThickness = 8;
    // Additive key. 0 means "follow the OS scale"; anything outside the usable
    // range is treated as 0 rather than launching the UI at an unusable size.
    settings.viewOptions.uiScaleFactor = configuration->value("uiScaleFactor", 0.0).toDouble();
    if (settings.viewOptions.uiScaleFactor != 0.0
        && (settings.viewOptions.uiScaleFactor < UI_SCALE_MINIMUM
            || settings.viewOptions.uiScaleFactor > UI_SCALE_MAXIMUM)) {
        settings.viewOptions.uiScaleFactor = 0.0;
    }
    // Migrate configurations written before the stock default changed from 4 px to
    // 2 px: a value of 4 with no user-set flag is the old stock default, so adopt
    // the new stock value. Manually chosen values are left untouched.
    if (!settings.viewOptions.exportBoundaryThicknessUserSet
        && settings.viewOptions.exportBoundaryThickness == 4) {
        settings.viewOptions.exportBoundaryThickness = 2;
    }
    configuration->endGroup();

    // Update checker (additive keys - older config files fall back to defaults)
    configuration->beginGroup("updateCheck");
    settings.updateCheck.enabled = configuration->value("enabled", true).toBool();
    settings.updateCheck.lastCheckTimestamp = configuration->value("lastCheckTimestamp", QString()).toString();
    settings.updateCheck.skippedVersion = configuration->value("skippedVersion", QString()).toString();
    configuration->endGroup();

    // CUDA plugin registry (additive keys - older config files fall back to defaults)
    configuration->beginGroup("cudaPlugin");
    settings.cudaPlugin.installedVersion = configuration->value("installedVersion", QString()).toString();
    settings.cudaPlugin.releaseTag = configuration->value("releaseTag", QString()).toString();
    settings.cudaPlugin.sha256 = configuration->value("sha256", QString()).toString();
    settings.cudaPlugin.enabled = configuration->value("enabled", false).toBool();
    settings.cudaPlugin.trusted = configuration->value("trusted", false).toBool();
    settings.cudaPlugin.installPath = configuration->value("installPath", QString()).toString();
    configuration->endGroup();

    // VBI processing options (additive keys - older config files fall back to defaults)
    configuration->beginGroup("vbiProcessing");
    settings.vbiProcessing.vbiCore = configuration->value("vbiCore", true).toBool();
    settings.vbiProcessing.ntsc = configuration->value("ntsc", true).toBool();
    settings.vbiProcessing.vitc = configuration->value("vitc", true).toBool();
    settings.vbiProcessing.closedCaptions = configuration->value("closedCaptions", true).toBool();
    settings.vbiProcessing.teletext = configuration->value("teletext", false).toBool();
    settings.vbiProcessing.vits = configuration->value("vits", false).toBool();
    settings.vbiProcessing.teletextHtmlDir = configuration->value("teletextHtmlDir", QString()).toString();
    settings.vbiProcessing.teletextTapeFormat = configuration->value("teletextTapeFormat", QStringLiteral("vhs")).toString();
    settings.vbiProcessing.teletextMinDuplicates = configuration->value("teletextMinDuplicates", 1).toInt();
    if (settings.vbiProcessing.teletextMinDuplicates < 1) settings.vbiProcessing.teletextMinDuplicates = 1;
    configuration->endGroup();

    // "Save frame as PNG" options (additive keys - older config files fall back to defaults)
    configuration->beginGroup("frameSnapshot");
    // stillMode replaced a bestFrameSearch on/off flag
    FrameSnapshot::Options snapshotDefaults;
    if (configuration->value("bestFrameSearch", false).toBool()) snapshotDefaults.stillMode = FrameSnapshot::StillMode::Cleanest;
    settings.frameSnapshot = readSnapshotOptions(configuration, snapshotDefaults);
    configuration->endGroup();

    // "Extract slideshow stills" options. Until first saved they follow the
    // "Save frame as PNG" options, averaging each photo.
    FrameSnapshot::Options slideshowDefaults = settings.frameSnapshot;
    slideshowDefaults.stillMode = FrameSnapshot::StillMode::Average;
    configuration->beginGroup("slideshowExtract");
    settings.slideshowExtract.snapshot = readSnapshotOptions(configuration, slideshowDefaults);
    settings.slideshowExtract.minHoldSeconds = std::clamp(configuration->value("minHoldSeconds", 1.0).toDouble(), 0.1, 60.0);
    settings.slideshowExtract.outputDirectory = configuration->value("outputDirectory", QString()).toString();
    settings.slideshowExtract.dialogGeometry = configuration->value("dialogGeometry").toByteArray();
    settings.slideshowExtract.dialogColumns = configuration->value("dialogColumns").toByteArray();
    settings.slideshowExtract.dialogRows = configuration->value("dialogRows").toByteArray();
    configuration->endGroup();
}

void Configuration::setDefault(void)
{
    // Set up the default values
    settings.version = SETTINGSVERSION;

    // Directories
    settings.directories.sourceDirectory = QDir::homePath();
    settings.directories.pngDirectory = QDir::homePath();
    settings.directories.lastUsed.clear();

    // Windows
    settings.windows.mainWindowGeometry = QByteArray();
    settings.windows.mainWindowScaleFactor = 1.0;
    settings.windows.vbiDialogGeometry = QByteArray();
    settings.windows.oscilloscopeDialogGeometry = QByteArray();
    settings.windows.vectorscopeDialogGeometry = QByteArray();
    settings.windows.waveformMonitorDialogGeometry = QByteArray();
    settings.windows.dropoutAnalysisDialogGeometry = QByteArray();
    settings.windows.visibleDropoutAnalysisDialogGeometry = QByteArray();
    settings.windows.blackSnrAnalysisDialogGeometry = QByteArray();
    settings.windows.whiteSnrAnalysisDialogGeometry = QByteArray();
    settings.windows.closedCaptionDialogGeometry = QByteArray();
    settings.windows.videoParametersDialogGeometry = QByteArray();
    settings.windows.chromaDecoderConfigDialogGeometry = QByteArray();
    settings.windows.fieldTimingDialogGeometry = QByteArray();

    // View options
    settings.viewOptions.toggleChromaDuringSeek = false;
    settings.viewOptions.skipBySegments = true;
    settings.viewOptions.waveformPhosphorMode = false;
    settings.viewOptions.theme = QStringLiteral("dark");
    settings.viewOptions.generateProxyEnabled = false;
    settings.viewOptions.exportProfileConfigEnabled = false;
    settings.viewOptions.exportProfileConfigPath = QString();
    settings.viewOptions.resizeFrameWithWindow = true;
    settings.viewOptions.showExportBoundary = true;
    settings.viewOptions.exportBoundaryThickness = 2;
    settings.viewOptions.exportBoundaryThicknessUserSet = false;
    settings.viewOptions.uiScaleFactor = 0.0;

    // Update checker
    settings.updateCheck.enabled = true;
    settings.updateCheck.lastCheckTimestamp = QString();
    settings.updateCheck.skippedVersion = QString();

    // CUDA plugin registry: deliberately not reset. It records plugin files
    // that are still installed on disk, writeConfiguration() no longer writes
    // it, and the constructor has just read it, so a settings-format reset
    // keeps the plugin known instead of orphaning its files.

    // VBI processing options (defaults match tbc-process-vbi CLI defaults)
    settings.vbiProcessing.vbiCore = true;
    settings.vbiProcessing.ntsc = true;
    settings.vbiProcessing.vitc = true;
    settings.vbiProcessing.closedCaptions = true;
    settings.vbiProcessing.teletext = false;
    settings.vbiProcessing.vits = false;
    settings.vbiProcessing.teletextHtmlDir = QString();
    settings.vbiProcessing.teletextTapeFormat = QStringLiteral("vhs");
    settings.vbiProcessing.teletextMinDuplicates = 1;

    // "Save frame as PNG" and "Extract slideshow stills" options
    settings.frameSnapshot = FrameSnapshot::Options();
    settings.slideshowExtract = SlideshowExtractOptions();

    // Write the configuration
    writeConfiguration();
}

// Functions to get and set configuration values ----------------------------------------------------------------------

// Directories
void Configuration::setSourceDirectory(QString sourceDirectory)
{
    settings.directories.sourceDirectory = sourceDirectory;
}

QString Configuration::getSourceDirectory(void)
{
    return settings.directories.sourceDirectory;
}

void Configuration::setPngDirectory(QString pngDirectory)
{
    settings.directories.pngDirectory = pngDirectory;
}

QString Configuration::getPngDirectory(void)
{
    return settings.directories.pngDirectory;
}

QString Configuration::getLastDirectory(const QString &purpose, const QString &fallback)
{
    // A remembered directory wins, but only while it still exists - a path on
    // a since-unmounted drive would otherwise open the dialog on nothing.
    const QString remembered = settings.directories.lastUsed.value(purpose);
    if (!remembered.isEmpty() && QDir(remembered).exists()) return remembered;

    // Then the caller's own contextual default (the loaded file's directory,
    // a suggested output path), which is the better guess on a fresh install.
    if (!fallback.isEmpty() && QDir(fallback).exists()) return fallback;

    if (!settings.directories.sourceDirectory.isEmpty()
        && QDir(settings.directories.sourceDirectory).exists()) {
        return settings.directories.sourceDirectory;
    }
    return QDir::homePath();
}

void Configuration::setLastDirectory(const QString &purpose, const QString &path)
{
    if (purpose.isEmpty() || path.isEmpty()) return;
    settings.directories.lastUsed.insert(purpose, path);
}

// Windows
void Configuration::setMainWindowGeometry(QByteArray mainWindowGeometry)
{
    settings.windows.mainWindowGeometry = mainWindowGeometry;
}

QByteArray Configuration::getMainWindowGeometry(void)
{
    return settings.windows.mainWindowGeometry;
}

void Configuration::setMainWindowScaleFactor(double mainWindowScaleFactor)
{
    settings.windows.mainWindowScaleFactor = mainWindowScaleFactor;
}

double Configuration::getMainWindowScaleFactor(void)
{
    return settings.windows.mainWindowScaleFactor;
}

void Configuration::setVbiDialogGeometry(QByteArray vbiDialogGeometry)
{
    settings.windows.vbiDialogGeometry = vbiDialogGeometry;
}

QByteArray Configuration::getVbiDialogGeometry(void)
{
    return settings.windows.vbiDialogGeometry;
}

void Configuration::setOscilloscopeDialogGeometry(QByteArray oscilloscopeDialogGeometry)
{
    settings.windows.oscilloscopeDialogGeometry = oscilloscopeDialogGeometry;
}

QByteArray Configuration::getOscilloscopeDialogGeometry(void)
{
    return settings.windows.oscilloscopeDialogGeometry;
}

void Configuration::setVectorscopeDialogGeometry(QByteArray vectorscopeDialogGeometry)
{
    settings.windows.vectorscopeDialogGeometry = vectorscopeDialogGeometry;
}

QByteArray Configuration::getVectorscopeDialogGeometry(void)
{
    return settings.windows.vectorscopeDialogGeometry;
}

void Configuration::setWaveformMonitorDialogGeometry(QByteArray waveformMonitorDialogGeometry)
{
    settings.windows.waveformMonitorDialogGeometry = waveformMonitorDialogGeometry;
}

QByteArray Configuration::getWaveformMonitorDialogGeometry(void)
{
    return settings.windows.waveformMonitorDialogGeometry;
}

void Configuration::setDropoutAnalysisDialogGeometry(QByteArray dropoutAnalysisDialogGeometry)
{
    settings.windows.dropoutAnalysisDialogGeometry = dropoutAnalysisDialogGeometry;
}

QByteArray Configuration::getDropoutAnalysisDialogGeometry(void)
{
    return settings.windows.dropoutAnalysisDialogGeometry;
}

void Configuration::setVisibleDropoutAnalysisDialogGeometry(QByteArray visibleDropoutAnalysisDialogGeometry)
{
    settings.windows.visibleDropoutAnalysisDialogGeometry = visibleDropoutAnalysisDialogGeometry;
}

QByteArray Configuration::getVisibleDropoutAnalysisDialogGeometry(void)
{
    return settings.windows.visibleDropoutAnalysisDialogGeometry;
}

void Configuration::setBlackSnrAnalysisDialogGeometry(QByteArray blackSnrAnalysisDialogGeometry)
{
    settings.windows.blackSnrAnalysisDialogGeometry = blackSnrAnalysisDialogGeometry;
}

QByteArray Configuration::getBlackSnrAnalysisDialogGeometry(void)
{
    return settings.windows.blackSnrAnalysisDialogGeometry;
}

void Configuration::setWhiteSnrAnalysisDialogGeometry(QByteArray whiteSnrAnalysisDialogGeometry)
{
    settings.windows.whiteSnrAnalysisDialogGeometry = whiteSnrAnalysisDialogGeometry;
}

QByteArray Configuration::getWhiteSnrAnalysisDialogGeometry(void)
{
    return settings.windows.whiteSnrAnalysisDialogGeometry;
}

void Configuration::setClosedCaptionDialogGeometry(QByteArray closedCaptionDialogGeometry)
{
    settings.windows.closedCaptionDialogGeometry = closedCaptionDialogGeometry;
}

QByteArray Configuration::getClosedCaptionDialogGeometry(void)
{
    return settings.windows.closedCaptionDialogGeometry;
}

void Configuration::setVideoParametersDialogGeometry(QByteArray videoParametersDialogGeometry)
{
    settings.windows.videoParametersDialogGeometry = videoParametersDialogGeometry;
}

QByteArray Configuration::getVideoParametersDialogGeometry(void)
{
    return settings.windows.videoParametersDialogGeometry;
}

void Configuration::setChromaDecoderConfigDialogGeometry(QByteArray chromaDecoderConfigDialogGeometry)
{
    settings.windows.chromaDecoderConfigDialogGeometry = chromaDecoderConfigDialogGeometry;
}

QByteArray Configuration::getChromaDecoderConfigDialogGeometry(void)
{
    return settings.windows.chromaDecoderConfigDialogGeometry;
}

void Configuration::setFieldTimingDialogGeometry(QByteArray fieldTimingDialogGeometry)
{
    settings.windows.fieldTimingDialogGeometry = fieldTimingDialogGeometry;
}

QByteArray Configuration::getFieldTimingDialogGeometry(void)
{
    return settings.windows.fieldTimingDialogGeometry;
}

// View options
void Configuration::setToggleChromaDuringSeek(bool toggleChromaDuringSeek)
{
    settings.viewOptions.toggleChromaDuringSeek = toggleChromaDuringSeek;
}

bool Configuration::getToggleChromaDuringSeek(void)
{
    return settings.viewOptions.toggleChromaDuringSeek;
}

void Configuration::setSkipBySegments(bool skipBySegments)
{
    settings.viewOptions.skipBySegments = skipBySegments;
}

bool Configuration::getSkipBySegments(void)
{
    return settings.viewOptions.skipBySegments;
}

void Configuration::setWaveformPhosphorMode(bool waveformPhosphorMode)
{
    settings.viewOptions.waveformPhosphorMode = waveformPhosphorMode;
}

bool Configuration::getWaveformPhosphorMode(void)
{
    return settings.viewOptions.waveformPhosphorMode;
}

void Configuration::setTheme(QString theme)
{
    settings.viewOptions.theme = theme;
}

QString Configuration::getTheme(void)
{
    return settings.viewOptions.theme;
}

void Configuration::setGenerateProxyEnabled(bool generateProxyEnabled)
{
    settings.viewOptions.generateProxyEnabled = generateProxyEnabled;
}

bool Configuration::getGenerateProxyEnabled(void)
{
    return settings.viewOptions.generateProxyEnabled;
}
void Configuration::setExportProfileConfigEnabled(bool exportProfileConfigEnabled)
{
    settings.viewOptions.exportProfileConfigEnabled = exportProfileConfigEnabled;
}

bool Configuration::getExportProfileConfigEnabled(void)
{
    return settings.viewOptions.exportProfileConfigEnabled;
}

void Configuration::setExportProfileConfigPath(QString exportProfileConfigPath)
{
    settings.viewOptions.exportProfileConfigPath = exportProfileConfigPath;
}

QString Configuration::getExportProfileConfigPath(void)
{
    return settings.viewOptions.exportProfileConfigPath;
}

void Configuration::setResizeFrameWithWindow(bool resizeFrameWithWindow)
{
    settings.viewOptions.resizeFrameWithWindow = resizeFrameWithWindow;
}

bool Configuration::getResizeFrameWithWindow(void)
{
    return settings.viewOptions.resizeFrameWithWindow;
}

void Configuration::setShowExportBoundary(bool showExportBoundary)
{
    settings.viewOptions.showExportBoundary = showExportBoundary;
}

bool Configuration::getShowExportBoundary(void)
{
    return settings.viewOptions.showExportBoundary;
}

void Configuration::setExportBoundaryThickness(qint32 exportBoundaryThickness)
{
    settings.viewOptions.exportBoundaryThickness = exportBoundaryThickness;
    // Only reached from the video parameters dialogue on a manual change, so mark
    // the value as user-set so future stock-default changes never override it
    settings.viewOptions.exportBoundaryThicknessUserSet = true;
}

qint32 Configuration::getExportBoundaryThickness(void)
{
    return settings.viewOptions.exportBoundaryThickness;
}

void Configuration::setUiScaleFactor(double uiScaleFactor)
{
    if (uiScaleFactor != 0.0 && (uiScaleFactor < UI_SCALE_MINIMUM || uiScaleFactor > UI_SCALE_MAXIMUM)) {
        uiScaleFactor = 0.0;
    }
    settings.viewOptions.uiScaleFactor = uiScaleFactor;
}

double Configuration::getUiScaleFactor(void)
{
    return settings.viewOptions.uiScaleFactor;
}

// Update checker
void Configuration::setUpdateCheckEnabled(bool updateCheckEnabled)
{
    settings.updateCheck.enabled = updateCheckEnabled;
}

bool Configuration::getUpdateCheckEnabled(void)
{
    return settings.updateCheck.enabled;
}

void Configuration::setLastUpdateCheckTimestamp(QString lastUpdateCheckTimestamp)
{
    settings.updateCheck.lastCheckTimestamp = lastUpdateCheckTimestamp;
}

QString Configuration::getLastUpdateCheckTimestamp(void)
{
    return settings.updateCheck.lastCheckTimestamp;
}

void Configuration::setSkippedUpdateVersion(QString skippedUpdateVersion)
{
    settings.updateCheck.skippedVersion = skippedUpdateVersion;
}

QString Configuration::getSkippedUpdateVersion(void)
{
    return settings.updateCheck.skippedVersion;
}

// CUDA plugin registry. Each setter writes its key straight to the settings
// file, and writeConfiguration() leaves the group alone; see the note there.
static void writeCudaPluginKey(QSettings *configuration, const char *key, const QVariant &value)
{
    configuration->beginGroup("cudaPlugin");
    configuration->setValue(key, value);
    configuration->endGroup();
}

void Configuration::setCudaPluginInstalledVersion(QString version)
{
    settings.cudaPlugin.installedVersion = version;
    writeCudaPluginKey(configuration, "installedVersion", version);
}

QString Configuration::getCudaPluginInstalledVersion(void)
{
    return settings.cudaPlugin.installedVersion;
}

void Configuration::setCudaPluginReleaseTag(QString tag)
{
    settings.cudaPlugin.releaseTag = tag;
    writeCudaPluginKey(configuration, "releaseTag", tag);
}

QString Configuration::getCudaPluginReleaseTag(void)
{
    return settings.cudaPlugin.releaseTag;
}

void Configuration::setCudaPluginSha256(QString sha256)
{
    settings.cudaPlugin.sha256 = sha256;
    writeCudaPluginKey(configuration, "sha256", sha256);
}

QString Configuration::getCudaPluginSha256(void)
{
    return settings.cudaPlugin.sha256;
}

void Configuration::setCudaPluginEnabled(bool enabled)
{
    settings.cudaPlugin.enabled = enabled;
    writeCudaPluginKey(configuration, "enabled", enabled);
}

bool Configuration::getCudaPluginEnabled(void)
{
    return settings.cudaPlugin.enabled;
}

void Configuration::setCudaPluginTrusted(bool trusted)
{
    settings.cudaPlugin.trusted = trusted;
    writeCudaPluginKey(configuration, "trusted", trusted);
}

bool Configuration::getCudaPluginTrusted(void)
{
    return settings.cudaPlugin.trusted;
}

void Configuration::setCudaPluginInstallPath(QString path)
{
    settings.cudaPlugin.installPath = path;
    writeCudaPluginKey(configuration, "installPath", path);
}

QString Configuration::getCudaPluginInstallPath(void)
{
    return settings.cudaPlugin.installPath;
}

// VBI processing options
void Configuration::setVbiProcessingOptions(const VbiProcessingOptions &options)
{
    settings.vbiProcessing = options;
}

VbiProcessingOptions Configuration::getVbiProcessingOptions(void)
{
    return settings.vbiProcessing;
}

// "Save frame as PNG" options
void Configuration::setFrameSnapshotOptions(const FrameSnapshot::Options &options)
{
    settings.frameSnapshot = options;
}

FrameSnapshot::Options Configuration::getFrameSnapshotOptions(void)
{
    return settings.frameSnapshot;
}

// "Extract slideshow stills" options
void Configuration::setSlideshowExtractOptions(const SlideshowExtractOptions &options)
{
    settings.slideshowExtract = options;
}

SlideshowExtractOptions Configuration::getSlideshowExtractOptions(void)
{
    return settings.slideshowExtract;
}
