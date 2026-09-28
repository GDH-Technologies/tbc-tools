/******************************************************************************
 * mainwindow.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2022 Adam Sampson
 * SPDX-FileCopyrightText: 2026 Harry Munday
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "mainwindow.h"

#include <cmath>
#include "ui_mainwindow.h"
#include "gui/processprogressrunner.h"
#include "tbc/logging.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QProcess>
#include <QDialog>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStringList>
#include <QTextStream>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileOpenEvent>
#include <QMimeData>
#include <QScreen>
#include <QtMath>
#include <QUrl>
#include <QClipboard>
#include <QWheelEvent>
#include <QWindow>
#include <QHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QAbstractSpinBox>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPushButton>
#include <QCheckBox>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <optional>
#include <utility>
#if defined(Q_OS_UNIX)
#include <signal.h>
#endif
#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#include "metadataconverterutil.h"
#include "notesviewerdialog.h"
#include "segmentsviewerdialog.h"
#include "teletextviewerdialog.h"
#include "timelinemarkerslider.h"
#include "efmhandlerdialog.h"
#include "../audio-align/audioalignmentdialog.h"
#include "../tbc-export-metadata/metadataexportdialog.h"
#include "tbc/uistyle.h"
#include "tbc/buildinfo.h"
namespace {
QString chromaDecoderNameFromConfig(VideoSystem system,
                                    const PalColour::Configuration &palConfig,
                                    const Comb::Configuration &ntscConfig)
{
    const bool isPal = (system == PAL || system == PAL_M);
    const bool isSecam = (system == SECAM || system == MESECAM);
    if (isPal) {
        switch (palConfig.chromaFilter) {
        case PalColour::palColourFilter:
            return QStringLiteral("pal2d");
        case PalColour::transform2DFilter:
            return QStringLiteral("transform2d");
        case PalColour::transform3DFilter:
            return QStringLiteral("transform3d");
        case PalColour::mono:
            return QStringLiteral("mono");
        default:
            break;
        }
    } else if (isSecam) {
        // SECAM is its own system: only SECAM decoders (and mono) are named here.
        switch (palConfig.chromaFilter) {
        case PalColour::secam:
            return QStringLiteral("secam");
        case PalColour::secamPredemod:
            return QStringLiteral("secam-predemod");
        case PalColour::mono:
            return QStringLiteral("mono");
        default:
            break;
        }
    }

    if (system == NTSC) {
        if (ntscConfig.dimensions <= 0) {
            return QStringLiteral("mono");
        }
        switch (ntscConfig.dimensions) {
        case 1:
            return QStringLiteral("ntsc1d");
        case 2:
            return QStringLiteral("ntsc2d");
        case 3:
            return ntscConfig.nnTransform3D ? QStringLiteral("nntransform3d")
                                            : QStringLiteral("ntsc3d");
        default:
            break;
        }
    }

    return QString();
}


QString sanitizedFileToken(const QString &value)
{
    QString token = value.trimmed().toLower();
    token.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    token.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    if (token.isEmpty()) {
        token = QStringLiteral("state");
    }
    return token;
}

// Wait for background work behind a progress dialog, keeping the window
// painted. Without a cancel flag, user input is held off so nothing can touch
// TbcSource while a worker thread is using it.
template <typename T>
T waitWithProgress(QWidget *parent, QFuture<T> future, const QString &label,
                   std::atomic<bool> *cancel = nullptr, std::atomic<qint32> *progress = nullptr,
                   qint32 maximum = 0)
{
    if (future.isFinished()) {
        return future.result();
    }

    QProgressDialog dialog(label, QObject::tr("Cancel"), 0, maximum, parent);
    if (!cancel) {
        dialog.setCancelButton(nullptr);
    }
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(300);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);

    QEventLoop loop;
    QFutureWatcher<T> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &dialog, [&]() {
        if (progress && maximum > 0) {
            dialog.setValue(qMin(progress->load(), maximum));
        }
        if (cancel && dialog.wasCanceled()) {
            cancel->store(true);
        }
    });
    watcher.setFuture(future);
    poll.start(100);
    if (!future.isFinished()) {
        loop.exec(cancel ? QEventLoop::AllEvents : QEventLoop::ExcludeUserInputEvents);
    }
    return future.result();
}

struct EfmAutoloadCandidates {
    QStringList efmInputs;
    QStringList pcmInputs;
    QString ac3Input;
};

EfmAutoloadCandidates discoverEfmAutoloadCandidates(const QString &directoryPath,
                                                    const QString &sourceBaseName)
{
    EfmAutoloadCandidates candidates;
    const QString normalizedDirectory = QDir::cleanPath(directoryPath.trimmed());
    if (normalizedDirectory.isEmpty()) {
        return candidates;
    }

    const QDir directory(normalizedDirectory);
    if (!directory.exists()) {
        return candidates;
    }

    const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::NoSymLinks,
                                                           QDir::Name | QDir::IgnoreCase);
    const QString sourceBaseToken = sourceBaseName.trimmed().toLower();
    QString bestAc3Candidate;
    int bestAc3Score = -1;

    for (const QFileInfo &entry : entries) {
        const QString suffix = entry.suffix().toLower();
        const QString fileNameLower = entry.fileName().toLower();
        const QString absolutePath = entry.absoluteFilePath();

        if (suffix == QStringLiteral("efm")) {
            candidates.efmInputs << absolutePath;
        }
        if (suffix == QStringLiteral("pcm")) {
            candidates.pcmInputs << absolutePath;
        }

        if (!fileNameLower.contains(QStringLiteral("ac3"))) {
            continue;
        }

        int score = 0;
        if (!sourceBaseToken.isEmpty() && fileNameLower.contains(sourceBaseToken)) {
            score += 2;
        }
        if (suffix == QStringLiteral("bin")
            || suffix == QStringLiteral("dat")
            || suffix == QStringLiteral("txt")
            || suffix == QStringLiteral("sym")
            || suffix == QStringLiteral("symbols")
            || suffix == QStringLiteral("raw")) {
            score += 4;
        } else if (suffix == QStringLiteral("ac3")) {
            score += 1;
        }
        if (fileNameLower.contains(QStringLiteral("symbol"))
            || fileNameLower.contains(QStringLiteral("sym"))) {
            score += 3;
        }

        if (score > bestAc3Score) {
            bestAc3Score = score;
            bestAc3Candidate = absolutePath;
        }
    }

    candidates.ac3Input = bestAc3Candidate;
    return candidates;
}


QString formatOptionalString(const QString &value)
{
    return value.isEmpty() ? QStringLiteral("—") : value;
}

QString formatOptionalDouble(double value, int precision = 3)
{
    if (value < 0.0) {
        return QStringLiteral("—");
    }
    return QString::number(value, 'f', precision);
}


QString formatOptionalBoolFromInt(qint32 value)
{
    if (value < 0) {
        return QStringLiteral("—");
    }
    return value != 0 ? QStringLiteral("Yes") : QStringLiteral("No");
}

QString backupFilenameWithTimestampFallback(const QString &inputMetadataFilename,
                                            const QString &backupSuffix)
{
    const QString defaultBackupFilename = inputMetadataFilename + backupSuffix;
    if (!QFileInfo::exists(defaultBackupFilename)) {
        return defaultBackupFilename;
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString backupFilename = inputMetadataFilename + QStringLiteral(".") + timestamp + backupSuffix;
    qint32 collisionCounter = 1;
    while (QFileInfo::exists(backupFilename)) {
        backupFilename = inputMetadataFilename + QStringLiteral(".") + timestamp
                         + QStringLiteral("_") + QString::number(collisionCounter) + backupSuffix;
        collisionCounter++;
    }
    return backupFilename;
}

bool toolHelpListsOption(const QString &toolPath, const QString &optionName)
{
    if (toolPath.isEmpty() || optionName.isEmpty()) {
        return false;
    }

    const QString cacheKey = QDir::cleanPath(toolPath) + QLatin1Char('|') + optionName;
    static QHash<QString, bool> supportCache;
    if (supportCache.contains(cacheKey)) {
        return supportCache.value(cacheKey);
    }

    // Run through ProcessProgressRunner so the GUI keeps its event loop: a
    // probe slower than half a second gets a progress dialog with Cancel
    // instead of a frozen window
    QString helpOutput;
    ProcessProgressRunner::Options options;
    options.onLine = [&helpOutput](const QString &line, int *, QString *) {
        helpOutput += line + QLatin1Char('\n');
    };
    const ProcessProgressRunner::Result result = ProcessProgressRunner::run(
        toolPath, {QStringLiteral("--help")}, QApplication::activeWindow(),
        QCoreApplication::translate("MainWindow", "Checking %1...").arg(QFileInfo(toolPath).fileName()),
        options);
    const bool supportsOption = result.status == ProcessProgressRunner::Result::Finished
                                && helpOutput.contains(optionName);

    supportCache.insert(cacheKey, supportsOption);
    return supportsOption;
}

QString metadataInputOptionForTool(const QString &toolPath)
{
    if (toolHelpListsOption(toolPath, QStringLiteral("--input-metadata"))) {
        return QStringLiteral("--input-metadata");
    }
    if (toolHelpListsOption(toolPath, QStringLiteral("--input-json"))) {
        return QStringLiteral("--input-json");
    }
    return QStringLiteral("--input-metadata");
}

QString noBackupOptionForTool(const QString &toolPath)
{
    if (toolHelpListsOption(toolPath, QStringLiteral("--nobackup"))) {
        return QStringLiteral("--nobackup");
    }
    if (toolHelpListsOption(toolPath, QStringLiteral("-n"))) {
        return QStringLiteral("-n");
    }
    return QString();
}

bool createTimestampedMetadataBackup(const QString &metadataFilename,
                                     const QString &backupSuffix,
                                     QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (metadataFilename.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Metadata filename is empty.");
        }
        return false;
    }
    if (!QFileInfo::exists(metadataFilename)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Metadata file does not exist:\n%1").arg(metadataFilename);
        }
        return false;
    }

    const QString backupFilename = backupFilenameWithTimestampFallback(metadataFilename, backupSuffix);
    if (!QFile::copy(metadataFilename, backupFilename)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Unable to create backup file:\n%1").arg(backupFilename);
        }
        return false;
    }
    return true;
}

void appendUniqueCandidate(QStringList &candidates, const QString &candidate)
{
    if (candidate.isEmpty()) {
        return;
    }

    for (const QString &existing : candidates) {
        if (existing.compare(candidate, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    candidates.append(candidate);
}

QString normalizedTeletextBaseName(const QFileInfo &pathInfo)
{
    QString baseName = pathInfo.completeBaseName();
    QString baseNameLower = baseName.toLower();
    const QStringList suffixesToStrip = {
        QStringLiteral(".tbc"),
        QStringLiteral(".ytbc"),
        QStringLiteral(".ctbc"),
        QStringLiteral(".tbcy"),
        QStringLiteral(".tbcc")
    };
    for (const QString &suffix : suffixesToStrip) {
        if (!baseNameLower.endsWith(suffix)) {
            continue;
        }
        baseName.chop(suffix.size());
        break;
    }
    return baseName;
}

QString defaultTeletextHtmlDirectoryForInput(const QString &inputPath)
{
    const QFileInfo inputInfo(inputPath);
    if (inputInfo.absolutePath().isEmpty()) {
        return QString();
    }

    QString baseName = normalizedTeletextBaseName(inputInfo);
    if (baseName.isEmpty()) {
        baseName = inputInfo.completeBaseName();
    }
    if (baseName.isEmpty()) {
        return QString();
    }
    return QDir(inputInfo.absolutePath()).filePath(baseName + QStringLiteral("_teletext_html"));
}

QString resolveTeletextHtmlDirectoryFromHint(const QString &pathHint)
{
    const QFileInfo pathInfo(pathHint);
    if (!pathInfo.exists()) {
        return QString();
    }

    if (pathInfo.isFile() && pathInfo.suffix().compare(QStringLiteral("html"), Qt::CaseInsensitive) == 0) {
        const QDir htmlDirectory(pathInfo.absolutePath());
        const QStringList htmlPages = htmlDirectory.entryList(
            QStringList() << QStringLiteral("*.html"),
            QDir::Files,
            QDir::Name | QDir::IgnoreCase
        );
        if (!htmlPages.isEmpty()) {
            return htmlDirectory.absolutePath();
        }
    }

    QDir baseDirectory;
    if (pathInfo.isDir()) {
        baseDirectory = QDir(pathInfo.absoluteFilePath());
    } else {
        baseDirectory = pathInfo.absoluteDir();
    }

    QStringList candidateDirectories;
    QStringList baseNames;
    if (pathInfo.isDir()) {
        appendUniqueCandidate(baseNames, pathInfo.fileName());
    } else {
        appendUniqueCandidate(baseNames, normalizedTeletextBaseName(pathInfo));
        appendUniqueCandidate(baseNames, pathInfo.completeBaseName());
    }

    for (const QString &baseName : baseNames) {
        if (baseName.isEmpty()) {
            continue;
        }
        appendUniqueCandidate(candidateDirectories, baseDirectory.filePath(baseName + QStringLiteral("_teletext_html")));
        appendUniqueCandidate(candidateDirectories, baseDirectory.filePath(baseName + QStringLiteral(".teletext_html")));
        appendUniqueCandidate(candidateDirectories, baseDirectory.filePath(baseName + QStringLiteral("_teletext")));
    }
    appendUniqueCandidate(candidateDirectories, baseDirectory.filePath(QStringLiteral("teletext_html")));
    appendUniqueCandidate(candidateDirectories, baseDirectory.filePath(QStringLiteral("teletext")));

    for (const QString &candidate : candidateDirectories) {
        const QDir candidateDirectory(candidate);
        if (!candidateDirectory.exists()) {
            continue;
        }
        const QStringList htmlPages = candidateDirectory.entryList(
            QStringList() << QStringLiteral("*.html"),
            QDir::Files,
            QDir::Name | QDir::IgnoreCase
        );
        if (!htmlPages.isEmpty()) {
            return candidateDirectory.absolutePath();
        }
    }

    return QString();
}

QString resolveTeletextHtmlDirectoryFromHints(const QStringList &pathHints)
{
    for (const QString &pathHint : pathHints) {
        const QString resolvedDirectory = resolveTeletextHtmlDirectoryFromHint(pathHint);
        if (!resolvedDirectory.isEmpty()) {
            return resolvedDirectory;
        }
    }
    return QString();
}
QString resolveSourceFilenameForMetadata(const QString &metadataFilename)
{
    const QFileInfo metadataInfo(metadataFilename);
    if (!metadataInfo.exists()) {
        return QString();
    }

    const QString metadataStem = metadataInfo.completeBaseName();
    const QString metadataStemLower = metadataStem.toLower();
    const QDir metadataDir(metadataInfo.absolutePath());

    QStringList sourceCandidates;
    const auto appendFileNameCandidate = [&metadataDir, &sourceCandidates](const QString &fileName) {
        appendUniqueCandidate(sourceCandidates, metadataDir.filePath(fileName));
    };

    auto replaceSuffix = [](const QString &fileName, const QString &suffix, const QString &replacement) {
        QString output = fileName;
        output.chop(suffix.size());
        output.append(replacement);
        return output;
    };

    if (metadataStemLower.endsWith(QStringLiteral(".tbc"))
        || metadataStemLower.endsWith(QStringLiteral(".ytbc"))
        || metadataStemLower.endsWith(QStringLiteral(".ctbc"))
        || metadataStemLower.endsWith(QStringLiteral(".tbcy"))
        || metadataStemLower.endsWith(QStringLiteral(".tbcc"))) {
        appendFileNameCandidate(metadataStem);

        if (metadataStemLower.endsWith(QStringLiteral(".ctbc"))) {
            appendFileNameCandidate(replaceSuffix(metadataStem, QStringLiteral(".ctbc"), QStringLiteral(".ytbc")));
        } else if (metadataStemLower.endsWith(QStringLiteral(".tbcc"))) {
            appendFileNameCandidate(replaceSuffix(metadataStem, QStringLiteral(".tbcc"), QStringLiteral(".tbcy")));
        } else if (metadataStemLower.endsWith(QStringLiteral(".ytbc"))) {
            appendFileNameCandidate(replaceSuffix(metadataStem, QStringLiteral(".ytbc"), QStringLiteral(".ctbc")));
        } else if (metadataStemLower.endsWith(QStringLiteral(".tbcy"))) {
            appendFileNameCandidate(replaceSuffix(metadataStem, QStringLiteral(".tbcy"), QStringLiteral(".tbcc")));
        } else if (metadataStemLower.endsWith(QStringLiteral("_chroma.tbc"))) {
            appendFileNameCandidate(metadataStem.left(metadataStem.size() - QStringLiteral("_chroma.tbc").size()) + QStringLiteral(".tbc"));
        } else if (metadataStemLower.startsWith(QStringLiteral("chroma_"))
                   && metadataStemLower.endsWith(QStringLiteral(".tbc"))) {
            appendFileNameCandidate(metadataStem.mid(QStringLiteral("chroma_").size()));
        }
    } else {
        appendFileNameCandidate(metadataStem + QStringLiteral(".tbc"));
        appendFileNameCandidate(metadataStem + QStringLiteral(".ytbc"));
        appendFileNameCandidate(metadataStem + QStringLiteral(".tbcy"));
        appendFileNameCandidate(metadataStem + QStringLiteral(".ctbc"));
        appendFileNameCandidate(metadataStem + QStringLiteral(".tbcc"));
    }

    for (const QString &candidate : sourceCandidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return QString();
}
QString resolveMetadataFilenameForSource(const QString &sourceFilename,
                                         const QString &preferredMetadataFilename = QString())
{
    if (!preferredMetadataFilename.isEmpty() && QFileInfo::exists(preferredMetadataFilename)) {
        return preferredMetadataFilename;
    }

    const QFileInfo sourceInfo(sourceFilename);
    if (!sourceInfo.exists()) {
        return QString();
    }

    QStringList metadataCandidates;
    const auto appendMetadataCandidate = [&metadataCandidates](const QString &candidate) {
        appendUniqueCandidate(metadataCandidates, candidate);
    };

    const auto appendCandidatesForFile = [&appendMetadataCandidate](const QFileInfo &info) {
        appendMetadataCandidate(info.filePath() + QStringLiteral(".json"));
        appendMetadataCandidate(info.filePath() + QStringLiteral(".db"));

        const QString basePath = QDir(info.absolutePath()).filePath(info.completeBaseName());
        appendMetadataCandidate(basePath + QStringLiteral(".json"));
        appendMetadataCandidate(basePath + QStringLiteral(".db"));

        const QString suffix = info.suffix().toLower();
        if (suffix == QStringLiteral("ytbc")
            || suffix == QStringLiteral("ctbc")
            || suffix == QStringLiteral("tbcy")
            || suffix == QStringLiteral("tbcc")) {
            appendMetadataCandidate(QDir(info.absolutePath())
                                        .filePath(info.completeBaseName() + QStringLiteral(".tbc.json")));
            appendMetadataCandidate(QDir(info.absolutePath())
                                        .filePath(info.completeBaseName() + QStringLiteral(".tbc.db")));
        }
    };

    appendCandidatesForFile(sourceInfo);

    const QString sourceLowerFileName = sourceInfo.fileName().toLower();
    QString lumaSourceCandidate;
    if (sourceLowerFileName.endsWith(QStringLiteral("_chroma.tbc"))) {
        lumaSourceCandidate = QDir(sourceInfo.absolutePath())
                                  .filePath(sourceInfo.fileName().left(sourceInfo.fileName().size()
                                          - QStringLiteral("_chroma.tbc").size()) + QStringLiteral(".tbc"));
    } else if (sourceLowerFileName.startsWith(QStringLiteral("chroma_"))
               && sourceLowerFileName.endsWith(QStringLiteral(".tbc"))) {
        lumaSourceCandidate = QDir(sourceInfo.absolutePath())
                                  .filePath(sourceInfo.fileName().mid(QStringLiteral("chroma_").size()));
    } else if (sourceLowerFileName.endsWith(QStringLiteral(".ctbc"))) {
        lumaSourceCandidate = QDir(sourceInfo.absolutePath())
                                  .filePath(sourceInfo.completeBaseName() + QStringLiteral(".ytbc"));
    } else if (sourceLowerFileName.endsWith(QStringLiteral(".tbcc"))) {
        lumaSourceCandidate = QDir(sourceInfo.absolutePath())
                                  .filePath(sourceInfo.completeBaseName() + QStringLiteral(".tbcy"));
    }

    if (!lumaSourceCandidate.isEmpty()) {
        appendCandidatesForFile(QFileInfo(lumaSourceCandidate));
    }

    for (const QString &candidate : metadataCandidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return QString();
}

double frameRateForSystem(VideoSystem system)
{
    switch (system) {
    case PAL:
        return 25.0;
    case PAL_M:
    case NTSC:
    default:
        return 30000.0 / 1001.0;
    }
}

int nominalFrameRateForSystem(VideoSystem system)
{
    switch (system) {
    case PAL:
        return 25;
    case PAL_M:
    case NTSC:
    default:
        return 30;
    }
}

qint32 minActiveFrameLineForSystem(VideoSystem system)
{
    switch (system) {
    case PAL:
        return 2;
    case PAL_M:
    case NTSC:
    default:
        return 1;
    }
}

QString resolveExternalExecutable(const QStringList &toolNames)
{
    if (toolNames.isEmpty()) {
        return QString();
    }
    const auto isRunnableFile = [](const QString &candidatePath) {
        const QFileInfo candidateInfo(candidatePath);
#if defined(Q_OS_WIN)
        return candidateInfo.exists() && candidateInfo.isFile();
#else
        return candidateInfo.exists() && candidateInfo.isFile() && candidateInfo.isExecutable();
#endif
    };

    const QStringList candidateToolNames = [&toolNames]() {
        QStringList names;
        for (const QString &toolName : toolNames) {
            appendUniqueCandidate(names, toolName);
#if defined(Q_OS_WIN)
            if (!toolName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
                appendUniqueCandidate(names, toolName + QStringLiteral(".exe"));
            }
#endif
        }
        return names;
    }();

    QStringList searchRoots;
    const auto appendRoot = [&searchRoots](const QString &rootPath) {
        appendUniqueCandidate(searchRoots, QDir::cleanPath(rootPath));
    };

    const QString appDir = QCoreApplication::applicationDirPath();
    appendRoot(appDir);
    const QStringList relativeRoots = {
        QStringLiteral("."),
        QStringLiteral(".."),
        QStringLiteral("../bin"),
        QStringLiteral("../../bin"),
        QStringLiteral("../../../"),
        QStringLiteral("../../../bin"),
        QStringLiteral("../../../../bin"),
        QStringLiteral("../Resources"),
        QStringLiteral("../Resources/bin"),
        QStringLiteral("../../Resources/bin"),
        QStringLiteral("../../../Resources/bin"),
        QStringLiteral("../libexec"),
        QStringLiteral("../../libexec"),
        QStringLiteral("../../../libexec")
    };
    for (const QString &root : relativeRoots) {
        appendRoot(QDir(appDir).filePath(root));
    }

    const QString currentDir = QDir::currentPath();
    appendRoot(currentDir);
    appendRoot(QDir(currentDir).filePath(QStringLiteral("bin")));
    appendRoot(QDir(currentDir).filePath(QStringLiteral("build/bin")));
    appendRoot(QDir(currentDir).filePath(QStringLiteral("../build/bin")));

#if defined(Q_OS_WIN)
    appendRoot(QDir(appDir).filePath(QStringLiteral("tools")));
    appendRoot(QDir(appDir).filePath(QStringLiteral("../tools")));
#endif

    if (qEnvironmentVariableIsSet("APPDIR")) {
        const QString appImageRoot = qEnvironmentVariable("APPDIR");
        appendRoot(QDir(appImageRoot).filePath(QStringLiteral("usr/bin")));
        appendRoot(QDir(appImageRoot).filePath(QStringLiteral("usr/libexec")));
    }

    if (qEnvironmentVariableIsSet("APPIMAGE")) {
        const QString appImageFile = qEnvironmentVariable("APPIMAGE");
        const QString appImageDir = QFileInfo(appImageFile).absolutePath();
        appendRoot(appImageDir);
        appendRoot(QDir(appImageDir).filePath(QStringLiteral("bin")));
    }

    for (const QString &dir : searchRoots) {
        const QDir searchDir(dir);
        for (const QString &name : candidateToolNames) {
            const QString localCandidate = searchDir.filePath(name);
            if (isRunnableFile(localCandidate)) {
                return localCandidate;
            }
        }
    }

    for (const QString &name : candidateToolNames) {
        const QString toolPath = QStandardPaths::findExecutable(name);
        if (!toolPath.isEmpty() && isRunnableFile(toolPath)) {
            return toolPath;
        }
    }

    return QString();
}

bool isSupportedInputExtension(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    return suffix == QStringLiteral("tbc")
        || suffix == QStringLiteral("ytbc")
        || suffix == QStringLiteral("ctbc")
        || suffix == QStringLiteral("tbcy")
        || suffix == QStringLiteral("tbcc")
        || suffix == QStringLiteral("db")
        || suffix == QStringLiteral("json");
}

bool isTeletextStreamInputExtension(const QString &filePath)
{
    static const QRegularExpression teletextSuffixPattern(
        QStringLiteral("^t\\d\\d$"),
        QRegularExpression::CaseInsensitiveOption
    );
    const QString suffix = QFileInfo(filePath).suffix();
    return teletextSuffixPattern.match(suffix).hasMatch();
}

QString firstSupportedDroppedFile(const QMimeData *mimeData)
{
    if (!mimeData || !mimeData->hasUrls()) {
        return QString();
    }

    const QList<QUrl> urls = mimeData->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile();
        if (localPath.isEmpty()) {
            continue;
        }
        if (isSupportedInputExtension(localPath) || isTeletextStreamInputExtension(localPath)) {
            return localPath;
        }
    }

    return QString();
}

enum class ExternalToolStage {
    Starting,
    LoadingMetadata,
    AnalysingMetadata,
    WritingMetadata,
    TeletextDependencyCheck,
    TeletextBackendProbe,
    TeletextDeconvolution,
    TeletextSquashing,
    TeletextHtmlGeneration,
    Finishing
};
bool externalToolStageIsTeletext(ExternalToolStage stage)
{
    return stage == ExternalToolStage::TeletextDependencyCheck
        || stage == ExternalToolStage::TeletextBackendProbe
        || stage == ExternalToolStage::TeletextDeconvolution
        || stage == ExternalToolStage::TeletextSquashing
        || stage == ExternalToolStage::TeletextHtmlGeneration;
}

QString externalToolStageLabel(const QString &toolDisplayName, ExternalToolStage stage)
{
    switch (stage) {
    case ExternalToolStage::Starting:
        return QObject::tr("%1: Preparing...").arg(toolDisplayName);
    case ExternalToolStage::LoadingMetadata:
        return QObject::tr("%1: Loading metadata...").arg(toolDisplayName);
    case ExternalToolStage::AnalysingMetadata:
        return QObject::tr("%1: Analysing metadata...").arg(toolDisplayName);
    case ExternalToolStage::WritingMetadata:
        return QObject::tr("%1: Writing metadata...").arg(toolDisplayName);
    case ExternalToolStage::TeletextDependencyCheck:
        return QObject::tr("%1: Checking teletext runtime...").arg(toolDisplayName);
    case ExternalToolStage::TeletextBackendProbe:
        return QObject::tr("%1: Probing teletext backends...").arg(toolDisplayName);
    case ExternalToolStage::TeletextDeconvolution:
        return QObject::tr("%1: Teletext deconvolution...").arg(toolDisplayName);
    case ExternalToolStage::TeletextSquashing:
        return QObject::tr("%1: Squashing teletext packets...").arg(toolDisplayName);
    case ExternalToolStage::TeletextHtmlGeneration:
        return QObject::tr("%1: Generating teletext HTML...").arg(toolDisplayName);
    case ExternalToolStage::Finishing:
    default:
        return QObject::tr("%1: Finalising...").arg(toolDisplayName);
    }
}

QString externalToolProgressSummary(qint32 processedFields, qint32 totalFields)
{
    if (totalFields <= 0) {
        return QObject::tr("Fields -----/----- (----- to go) | Frames -----/----- (----- to go)");
    }

    const qint32 clampedProcessedFields = qBound<qint32>(0, processedFields, totalFields);
    const qint32 remainingFields = totalFields - clampedProcessedFields;
    const qint32 totalFrames = (totalFields + 1) / 2;
    const qint32 processedFrames = (clampedProcessedFields + 1) / 2;
    const qint32 remainingFrames = qMax<qint32>(0, totalFrames - processedFrames);

    return QObject::tr("Fields %1/%2 (%3 to go) | Frames %4/%5 (%6 to go)")
        .arg(clampedProcessedFields, 5, 10, QChar('0'))
        .arg(totalFields, 5, 10, QChar('0'))
        .arg(remainingFields, 5, 10, QChar('0'))
        .arg(processedFrames, 5, 10, QChar('0'))
        .arg(totalFrames, 5, 10, QChar('0'))
        .arg(remainingFrames, 5, 10, QChar('0'));
}

int externalToolProgressPercent(qint32 processedFields, qint32 totalFields)
{
    if (totalFields <= 0) {
        return 0;
    }

    const qint32 clampedProcessedFields = qBound<qint32>(0, processedFields, totalFields);
    return static_cast<int>((static_cast<double>(clampedProcessedFields) / static_cast<double>(totalFields)) * 100.0);
}

QString externalToolTeletextProgressSummary(qint32 teletextPercent)
{
    if (teletextPercent < 0) {
        return QObject::tr("Teletext progress: --");
    }
    return QObject::tr("Teletext progress: %1%").arg(teletextPercent);
}

QString normalizedPathForCompare(const QString &path)
{
    if (path.isEmpty()) {
        return QString();
    }

    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    if (!canonicalPath.isEmpty()) {
        return canonicalPath;
    }
    return info.absoluteFilePath();
}

bool sameFilePath(const QString &left, const QString &right)
{
    const QString normalizedLeft = normalizedPathForCompare(left);
    const QString normalizedRight = normalizedPathForCompare(right);
    if (normalizedLeft.isEmpty() || normalizedRight.isEmpty()) {
        return false;
    }
    return normalizedLeft == normalizedRight;
}

struct UserNoteMarker
{
    qint32 frame = -1;
    QString comment;
};

qint32 jsonValueToInt(const QJsonValue &value, bool *ok = nullptr)
{
    bool conversionOk = false;
    qint32 output = -1;
    if (value.isDouble()) {
        const double numericValue = value.toDouble();
        output = static_cast<qint32>(numericValue);
        conversionOk = true;
    } else if (value.isString()) {
        output = value.toString().toInt(&conversionOk);
    }
    if (ok) {
        *ok = conversionOk;
    }
    return conversionOk ? output : -1;
}

QVector<UserNoteMarker> normaliseUserNoteMarkers(const QVector<UserNoteMarker> &noteMarkers,
                                                 qint32 totalFrames)
{
    QMap<qint32, QString> notesByFrame;
    const bool clampToKnownRange = (totalFrames > 0);
    for (const UserNoteMarker &noteMarker : noteMarkers) {
        if (noteMarker.frame <= 0) {
            continue;
        }
        qint32 frame = noteMarker.frame;
        if (clampToKnownRange) {
            frame = qBound<qint32>(1, frame, totalFrames);
        }
        if (frame <= 0) {
            continue;
        }
        notesByFrame.insert(frame, noteMarker.comment.trimmed());
    }

    QVector<UserNoteMarker> normalizedMarkers;
    normalizedMarkers.reserve(notesByFrame.size());
    for (auto it = notesByFrame.cbegin(); it != notesByFrame.cend(); ++it) {
        normalizedMarkers.append({it.key(), it.value()});
    }
    return normalizedMarkers;
}

QVector<UserNoteMarker> parseUserMarkersJson(const QString &userMarkersJson, qint32 totalFrames)
{
    const QByteArray jsonBytes = userMarkersJson.trimmed().toUtf8();
    if (jsonBytes.isEmpty()) {
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return {};
    }

    QJsonArray markersArray;
    if (jsonDocument.isArray()) {
        markersArray = jsonDocument.array();
    } else if (jsonDocument.isObject()) {
        const QJsonObject rootObject = jsonDocument.object();
        const QJsonValue notesValue = rootObject.value(QStringLiteral("notes"));
        if (notesValue.isArray()) {
            markersArray = notesValue.toArray();
        }
    }

    QVector<UserNoteMarker> parsedMarkers;
    parsedMarkers.reserve(markersArray.size());
    for (const QJsonValue &markerValue : markersArray) {
        if (!markerValue.isObject()) {
            continue;
        }
        const QJsonObject markerObject = markerValue.toObject();
        bool frameOk = false;
        qint32 frame = -1;
        if (markerObject.contains(QStringLiteral("frame"))) {
            frame = jsonValueToInt(markerObject.value(QStringLiteral("frame")), &frameOk);
        } else if (markerObject.contains(QStringLiteral("selection"))) {
            frame = jsonValueToInt(markerObject.value(QStringLiteral("selection")), &frameOk);
        }
        if (!frameOk || frame <= 0) {
            continue;
        }

        QString comment;
        const QJsonValue commentValue = markerObject.value(QStringLiteral("comment"));
        if (commentValue.isString()) {
            comment = commentValue.toString();
        } else {
            const QJsonValue textValue = markerObject.value(QStringLiteral("text"));
            if (textValue.isString()) {
                comment = textValue.toString();
            }
        }

        parsedMarkers.append({frame, comment});
    }

    return normaliseUserNoteMarkers(parsedMarkers, totalFrames);
}

QVector<UserNoteMarker> userNoteMarkersFromVideoParameters(
    const TbcMetaData::VideoParameters &videoParameters,
    qint32 totalFrames)
{
    QVector<UserNoteMarker> combinedMarkers;
    if (videoParameters.userMarkerSelection > 0) {
        combinedMarkers.append({videoParameters.userMarkerSelection, videoParameters.userMarkerComment});
    }
    combinedMarkers += parseUserMarkersJson(videoParameters.userMarkersJson, totalFrames);
    return normaliseUserNoteMarkers(combinedMarkers, totalFrames);
}

qint32 nextUserMarkerChapterFrame(const QVector<UserNoteMarker> &noteMarkers,
                                  qint32 currentFrameNumber,
                                  qint32 totalFrames)
{
    for (const UserNoteMarker &noteMarker : noteMarkers) {
        if (noteMarker.frame > currentFrameNumber) {
            return noteMarker.frame;
        }
    }
    return totalFrames;
}

qint32 previousUserMarkerChapterFrame(const QVector<UserNoteMarker> &noteMarkers,
                                      qint32 currentFrameNumber)
{
    for (qint32 i = noteMarkers.size() - 1; i >= 0; --i) {
        if (noteMarkers.at(i).frame < currentFrameNumber) {
            return noteMarkers.at(i).frame;
        }
    }
    return 1;
}

QString serializeUserNoteMarkersJson(const QVector<UserNoteMarker> &noteMarkers)
{
    if (noteMarkers.isEmpty()) {
        return QString();
    }

    QJsonArray markersArray;
    for (const UserNoteMarker &noteMarker : noteMarkers) {
        if (noteMarker.frame <= 0) {
            continue;
        }
        QJsonObject markerObject;
        markerObject.insert(QStringLiteral("frame"), noteMarker.frame);
        if (!noteMarker.comment.isEmpty()) {
            markerObject.insert(QStringLiteral("comment"), noteMarker.comment);
        }
        markersArray.append(markerObject);
    }

    if (markersArray.isEmpty()) {
        return QString();
    }
    return QString::fromUtf8(QJsonDocument(markersArray).toJson(QJsonDocument::Compact));
}

bool applyUserNoteMarkersToVideoParameters(TbcMetaData::VideoParameters &videoParameters,
                                           const QVector<UserNoteMarker> &noteMarkers)
{
    const QVector<UserNoteMarker> normalizedMarkers = normaliseUserNoteMarkers(noteMarkers, -1);
    const QString userMarkersJson = serializeUserNoteMarkersJson(normalizedMarkers);
    const qint32 legacyMarkerFrame = normalizedMarkers.isEmpty() ? -1 : normalizedMarkers.first().frame;
    const QString legacyMarkerComment = normalizedMarkers.isEmpty() ? QString() : normalizedMarkers.first().comment;

    bool changed = false;
    if (videoParameters.userMarkersJson != userMarkersJson) {
        videoParameters.userMarkersJson = userMarkersJson;
        changed = true;
    }
    if (videoParameters.userMarkerSelection != legacyMarkerFrame) {
        videoParameters.userMarkerSelection = legacyMarkerFrame;
        changed = true;
    }
    if (videoParameters.userMarkerComment != legacyMarkerComment) {
        videoParameters.userMarkerComment = legacyMarkerComment;
        changed = true;
    }
    return changed;
}

void noteMarkerListsFromMarkers(const QVector<UserNoteMarker> &noteMarkers,
                                QVector<qint32> &noteFrames,
                                QStringList &noteComments)
{
    noteFrames.clear();
    noteComments.clear();
    noteFrames.reserve(noteMarkers.size());
    noteComments.reserve(noteMarkers.size());
    for (const UserNoteMarker &noteMarker : noteMarkers) {
        if (noteMarker.frame <= 0) {
            continue;
        }
        noteFrames.append(noteMarker.frame);
        noteComments.append(noteMarker.comment);
    }
}

qint32 noteMarkerIndexForFrame(const QVector<UserNoteMarker> &noteMarkers, qint32 frame)
{
    for (qint32 i = 0; i < noteMarkers.size(); ++i) {
        if (noteMarkers.at(i).frame == frame) {
            return i;
        }
    }
    return -1;
}

qint32 nearestNoteMarkerIndexForFrame(const QVector<UserNoteMarker> &noteMarkers, qint32 frame)
{
    if (noteMarkers.isEmpty() || frame <= 0) {
        return -1;
    }

    qint32 nearestIndex = -1;
    qint32 nearestDistance = std::numeric_limits<qint32>::max();
    for (qint32 i = 0; i < noteMarkers.size(); ++i) {
        const qint32 candidateDistance = qAbs(noteMarkers.at(i).frame - frame);
        if (candidateDistance < nearestDistance) {
            nearestDistance = candidateDistance;
            nearestIndex = i;
        }
    }
    return nearestIndex;
}

qint32 sliderValueForContextPoint(const QSlider *slider, const QPoint &contextPos)
{
    if (!slider) {
        return 0;
    }

    QStyleOptionSlider option;
    option.initFrom(slider);
    option.orientation = slider->orientation();
    option.minimum = slider->minimum();
    option.maximum = slider->maximum();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    option.upsideDown = slider->invertedAppearance();

    const QRect grooveRect = slider->style()->subControlRect(QStyle::CC_Slider,
                                                              &option,
                                                              QStyle::SC_SliderGroove,
                                                              slider);
    if (!grooveRect.isValid() || grooveRect.width() <= 1) {
        return slider->value();
    }

    const qint32 boundedX = qBound(grooveRect.left(), contextPos.x(), grooveRect.right());
    const qint32 relativeX = boundedX - grooveRect.left();
    return QStyle::sliderValueFromPosition(slider->minimum(),
                                           slider->maximum(),
                                           relativeX,
                                           grooveRect.width() - 1,
                                           option.upsideDown);
}


// A new window is shown (and takes focus); one already open is brought to the
// front instead
void showOrRaise(QWidget *window)
{
    if (window->isVisible()) {
        window->raise();
        window->activateWindow();
    } else {
        window->show();
    }
}
} // namespace

MainWindow::MainWindow(QString inputFilenameParam, bool metadataOnlyParam, QString themeChoiceParam, QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    themeChoice(themeChoiceParam)
{
    ui->setupUi(this);

    // The platform's standard keys where one exists (Ctrl+O / Ctrl+S / Ctrl+Q
    // and Ctrl+W / F5 or Ctrl+R / Ctrl++ and Ctrl+= / Ctrl+- on Linux and
    // Windows; the Cmd equivalents on macOS). Exit takes Quit and Close: Quit
    // has no binding on Windows.
    ui->actionOpen_TBC_file->setShortcuts(QKeySequence::Open);
    ui->actionSave_Metadata->setShortcuts(QKeySequence::Save);
    ui->actionReload_TBC->setShortcuts(QKeySequence::Refresh);
    ui->actionExit->setShortcuts(QKeySequence::keyBindings(QKeySequence::Quit)
                                 + QKeySequence::keyBindings(QKeySequence::Close));
    ui->actionZoom_In->setShortcuts(QKeySequence::keyBindings(QKeySequence::ZoomIn)
                                    << QKeySequence(Qt::CTRL | Qt::Key_Equal));
    ui->actionZoom_Out->setShortcuts(QKeySequence::ZoomOut);

    // Icons from the desktop's icon theme; none is shown where it has none
    ui->actionOpen_TBC_file->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen));
    ui->actionReload_TBC->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ViewRefresh));
    ui->actionSave_Metadata->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentSave));
    ui->actionExit->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ApplicationExit));
    ui->actionAbout_ld_analyse->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::HelpAbout));

    copyCurrentDisplayAction = new QAction(tr("&Copy Current Display"), this);
    copyCurrentDisplayAction->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::EditCopy));
    copyCurrentDisplayAction->setShortcut(QKeySequence::Copy);
    copyCurrentDisplayAction->setShortcutContext(Qt::WindowShortcut);
    connect(copyCurrentDisplayAction, &QAction::triggered,
            this, &MainWindow::copyCurrentDisplayToClipboard);
    addAction(copyCurrentDisplayAction);

    saveAllModesPngAction = new QAction(tr("Save all mode views as PNGs..."), this);
    connect(saveAllModesPngAction, &QAction::triggered,
            this, &MainWindow::saveAllModesAsPngs);
    if (ui->menuFile) {
        if (ui->actionExit) {
            ui->menuFile->insertAction(ui->actionExit, saveAllModesPngAction);
        } else {
            ui->menuFile->addAction(saveAllModesPngAction);
        }
    }
    setAcceptDrops(true);
    if (centralWidget()) {
        centralWidget()->setAcceptDrops(true);
    }
    if (ui->mainTabWidget) {
        ui->mainTabWidget->setAcceptDrops(true);
    }
    if (ui->viewerTab) {
        ui->viewerTab->setAcceptDrops(true);
    }
    if (ui->imageViewerLabel) {
        ui->imageViewerLabel->setAcceptDrops(true);
    }
    if (ui->mainTabWidget && ui->viewerTab) {
        ui->mainTabWidget->setCurrentWidget(ui->viewerTab);
    }
    if (ui->mainTabWidget) {
        connect(ui->mainTabWidget, &QTabWidget::currentChanged, this, [this](int) {
            if (!isViewerTabActive()) {
                clearCursorReadout();
                exportBoundaryDragHandle = ExportBoundaryHandle::None;
                exportBoundarySelectedHandle = ExportBoundaryHandle::None;
                updateExportBoundaryHoverCursor(QPoint(-1, -1));
                return;
            }
            if (resizeFrameWithWindow && tbcSource.getIsSourceLoaded() && isViewerTabActive()) {
                resizeTimer->start();
            }
        });
    }
    if (ui->posTimecodeLineEdit) {
        ui->posTimecodeLineEdit->setPlaceholderText(tr("HH:MM:SS:FF"));
        ui->posTimecodeLineEdit->setMaxLength(15);
        ui->posTimecodeLineEdit->setAlignment(Qt::AlignCenter);
        ui->posTimecodeLineEdit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        QFont valueFont = ui->posTimecodeLineEdit->font();
        valueFont.setPointSize(qMax(12, valueFont.pointSize() + 3));
        ui->posTimecodeLineEdit->setFont(valueFont);
        ui->posTimecodeLineEdit->setTextMargins(3, 0, 1, 0);
        // Height from the enlarged font (Fixed vertical policy = sizeHint),
        // not a fixed 30 px that clips it under a larger system font
        const QFontMetrics valueMetrics(valueFont);
        const int valueMinWidth = valueMetrics.horizontalAdvance(QStringLiteral("00:00:00:00")) + 8;
        ui->posTimecodeLineEdit->setMinimumWidth(valueMinWidth);
        ui->posTimecodeLineEdit->setMaximumWidth(valueMinWidth + 4);
        ui->posTimecodeLineEdit->setVisible(false);
    }
    if (ui->posNumberSpinBox) {
        QFont spinFont = ui->posNumberSpinBox->font();
        spinFont.setPointSize(qMax(11, spinFont.pointSize() + 2));
        ui->posNumberSpinBox->setFont(spinFont);
        ui->posNumberSpinBox->setAlignment(Qt::AlignCenter);
        const QFontMetrics spinMetrics(spinFont);
        const int spinMinWidth = spinMetrics.horizontalAdvance(QStringLiteral("000000")) + 30;
        ui->posNumberSpinBox->setMinimumWidth(spinMinWidth);
        ui->posNumberSpinBox->setMaximumWidth(220);
    }
    if (ui->horizontalLayout_3 && ui->posTimecodeLineEdit) {
        const int timecodeIndex = ui->horizontalLayout_3->indexOf(ui->posTimecodeLineEdit);
        if (timecodeIndex >= 0) {
            ui->horizontalLayout_3->setStretch(timecodeIndex, 0);
        }
    }
    ui->posHorizontalSlider->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->startPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/start-frame.svg")));
    ui->previousPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/prev-frame.svg")));
    ui->playPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/start-playback.svg")));
    ui->nextPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/next-frame.svg")));
    ui->endPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/end-frame.svg")));
    // The zoom buttons mirror the View menu's zoom actions (enabled state,
    // tooltip, icon), so the actions carry the media bar's icons
    ui->actionZoom_In->setIcon(QIcon(QStringLiteral(":/icons/Graphics/zoom-in.svg")));
    ui->actionZoom_Out->setIcon(QIcon(QStringLiteral(":/icons/Graphics/zoom-out.svg")));
    ui->actionZoom_1x->setIcon(QIcon(QStringLiteral(":/icons/Graphics/zoom-original.svg")));
    ui->zoomInPushButton->setDefaultAction(ui->actionZoom_In);
    ui->zoomOutPushButton->setDefaultAction(ui->actionZoom_Out);
    ui->originalSizePushButton->setDefaultAction(ui->actionZoom_1x);
    ui->mouseModePushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/oscilloscope-target.svg")));
    vectorscopeSelectionPushButton = new QPushButton(ui->mediaControl_frame);
    vectorscopeSelectionPushButton->setObjectName(QStringLiteral("vectorscopeSelectionPushButton"));
    vectorscopeSelectionPushButton->setMinimumSize(QSize(30, 30));
    vectorscopeSelectionPushButton->setMaximumSize(QSize(30, 30));
    vectorscopeSelectionPushButton->setCheckable(true);
    vectorscopeSelectionPushButton->setChecked(false);
    vectorscopeSelectionPushButton->setToolTip(tr("Enable vectorscope custom-area selection on the main viewer"));
    vectorscopeSelectionPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/highlight-selection.svg")));
    if (ui->horizontalLayout_3 && ui->mouseModePushButton) {
        const int mouseModeButtonIndex = ui->horizontalLayout_3->indexOf(ui->mouseModePushButton);
        if (mouseModeButtonIndex >= 0) {
            ui->horizontalLayout_3->insertWidget(mouseModeButtonIndex + 1, vectorscopeSelectionPushButton);
        } else {
            ui->horizontalLayout_3->addWidget(vectorscopeSelectionPushButton);
        }
    }
    connect(vectorscopeSelectionPushButton, &QPushButton::toggled,
            this, &MainWindow::onVectorscopeSelectionToggled);
    populateThemesMenu();

    // Set up dialogues
    oscilloscopeDialog = new OscilloscopeDialog(this);
    rgbScopeDialog = new RgbScopeDialog(this);
    yuvRangeDialog = new YuvRangeDialog(this);
    yuvRangeSettings = yuvRangeDialog->settings();
    vectorscopeDialog = new VectorscopeDialog(this);
    waveformMonitorDialog = new WaveformMonitorDialog(this);
    fieldTimingDialog = new FieldTimingDialog(this);
    aboutDialog = new AboutDialog(this);
    vbiDialog = new VbiDialog(this);
    dropoutAnalysisDialog = new DropoutAnalysisDialog(this);
    visibleDropoutAnalysisDialog = new VisibleDropOutAnalysisDialog(this);
    blackSnrAnalysisDialog = new BlackSnrAnalysisDialog(this);
    whiteSnrAnalysisDialog = new WhiteSnrAnalysisDialog(this);
    // Busy indicator while TbcSource loads or saves on its worker thread.
    // Application-modal because the scopes and dialogs read tbcSource too; no
    // cancel. Escape or the title-bar close only hide a QProgressDialog, so
    // while the operation runs it is shown again.
    busyProgress = new QProgressDialog(this);
    busyProgress->setWindowTitle(tr("tbc-analyse"));
    busyProgress->setCancelButton(nullptr);
    busyProgress->setRange(0, 0);
    busyProgress->setWindowModality(Qt::ApplicationModal);
    busyProgress->reset(); // stops the auto-show timer a new QProgressDialog starts
    busyProgress->setAutoReset(false);
    busyProgress->setAutoClose(false);
    busyProgress->hide();
    connect(busyProgress, &QDialog::finished, this, [this]() {
        if (sourceOperationInProgress) {
            busyProgress->show();
        }
    });
    closedCaptionDialog = new ClosedCaptionsDialog(this);
    videoParametersDialog = new VideoParametersDialog(this);
    chromaDecoderConfigDialog = new ChromaDecoderConfigDialog(this);
    metadataConversionDialog = new MetadataConversionDialog(this);
    metadataConversionDialog->setConfiguration(&configuration);
    metadataStatusDialog = new MetadataStatusDialog(this);
    metadataEditorDialog = new MetadataEditorDialog(this);
    connect(metadataEditorDialog, &MetadataEditorDialog::videoParametersChanged,
            this, &MainWindow::videoParametersChangedSignalHandler);
    connect(metadataEditorDialog, &MetadataEditorDialog::pcmAudioParametersChanged,
            this, [this](const TbcMetaData::PcmAudioParameters &pcmAudioParameters) {
        if (!tbcSource.getIsSourceLoaded()) return;
        tbcSource.setPcmAudioParameters(pcmAudioParameters);
        setWindowModified(true);
    });
    // Apply: save the metadata to disk. TbcSource::saveSourceMetadata() writes
    // to a .new file, backs up the original to .bup (timestamped fallback),
    // renames .new to the target, then onSourceSaved reloads the source
    // with the new metadata.
    connect(metadataEditorDialog, &MetadataEditorDialog::refreshRequested,
            this, [this]() {
        if (!tbcSource.getIsSourceLoaded()) return;
        sourceOperationInProgress = true;
        tbcSource.saveSourceMetadata();
    });
    // SECAM per-field first-line-identity edits from the Metadata Editor.
    connect(metadataEditorDialog, &MetadataEditorDialog::secamFirstLineIsRedChanged,
            this, [this](qint32 fieldNumber, bool value, bool applyToAll) {
        if (!tbcSource.getIsSourceLoaded()) return;
        tbcSource.setSecamFirstLineIsRed(fieldNumber, value, applyToAll);
        setWindowModified(true);
        updateMetadataStatusPanel();
        updateImage();
    });
    notesViewerDialog = new NotesViewerDialog(this);
    connect(notesViewerDialog, &NotesViewerDialog::goToFrameRequested, this, [this](qint32 frameNumber) {
        if (!tbcSource.getIsSourceLoaded()) {
            return;
        }
        setPlaybackRunning(false);
        const qint32 clampedFrame = qBound<qint32>(1, frameNumber, qMax<qint32>(1, tbcSource.getNumberOfFrames()));
        setCurrentFrame(clampedFrame);
        const qint32 currentNumber = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
        updatePositionEditorValue(currentNumber);
        ui->posHorizontalSlider->setValue(currentNumber);
    });
    connect(notesViewerDialog, &NotesViewerDialog::notesUpdated, this,
            [this](qint32 inFrame,
                   qint32 outFrame,
                   const QVector<qint32> &noteFrames,
                   const QStringList &noteComments) {
                if (!tbcSource.getIsSourceLoaded()) {
                    return;
                }

                const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
                qint32 clampedIn = (inFrame > 0) ? qBound<qint32>(1, inFrame, totalFrames) : -1;
                qint32 clampedOut = (outFrame > 0) ? qBound<qint32>(1, outFrame, totalFrames) : -1;
                if (clampedIn > 0 && clampedOut > 0 && clampedOut < clampedIn) {
                    clampedOut = clampedIn;
                }

                QVector<UserNoteMarker> updatedMarkers;
                updatedMarkers.reserve(noteFrames.size());
                for (qint32 i = 0; i < noteFrames.size(); ++i) {
                    const QString noteComment = (i < noteComments.size()) ? noteComments.at(i) : QString();
                    updatedMarkers.append({noteFrames.at(i), noteComment});
                }
                updatedMarkers = normaliseUserNoteMarkers(updatedMarkers, totalFrames);

                TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
                bool changed = false;
                if (videoParameters.userEditInSelection != clampedIn) {
                    videoParameters.userEditInSelection = clampedIn;
                    changed = true;
                }
                if (videoParameters.userEditOutSelection != clampedOut) {
                    videoParameters.userEditOutSelection = clampedOut;
                    changed = true;
                }
                if (applyUserNoteMarkersToVideoParameters(videoParameters, updatedMarkers)) {
                    changed = true;
                }
                if (!changed) {
                    return;
                }

                tbcSource.setVideoParameters(videoParameters);
                setWindowModified(true);
                updateMetadataStatusPanel();
                updateTimelineMarkers();
                updateNotesViewerState();
                updateSegmentsViewerState();
            });
    exportDialog = new ExportDialog(this);
    exportDialog->setConfiguration(&configuration);
    ui->mainTabWidget->addTab(exportDialog, tr("Export"));
    exportDialog->setGenerateProxyEnabledPreference(configuration.getGenerateProxyEnabled());
    exportDialog->setExportProfileConfigPreference(configuration.getExportProfileConfigEnabled(),
                                                   configuration.getExportProfileConfigPath());
    connect(exportDialog, &ExportDialog::userEditRangeSelectionChanged,
            this, &MainWindow::exportRangeSelectionChangedSignalHandler);
    connect(exportDialog, &ExportDialog::proxyGenerationPreferenceChanged, this, [this](bool enabled) {
        configuration.setGenerateProxyEnabled(enabled);
        configuration.writeConfiguration();
    });
    connect(exportDialog, &ExportDialog::exportProfileConfigPreferenceChanged, this,
            [this](bool enabled, const QString &configPath) {
                configuration.setExportProfileConfigEnabled(enabled);
                configuration.setExportProfileConfigPath(configPath);
                configuration.writeConfiguration();
            });

    notesViewerAction = new QAction(tr("Marker Viewer..."), this);
    if (ui->menuWindow) {
        ui->menuWindow->addSeparator();
        ui->menuWindow->addAction(notesViewerAction);
    }
    connect(notesViewerAction, &QAction::triggered, this, [this]() {
        updateNotesViewerState();
        updateSegmentsViewerState();
        showOrRaise(notesViewerDialog);
    });

    // Segments viewer: the editable recording-segment layer of the metadata
    segmentsViewerDialog = new SegmentsViewerDialog(this);
    connect(segmentsViewerDialog, &SegmentsViewerDialog::goToFieldRequested, this, &MainWindow::goToField);
    connect(segmentsViewerDialog, &SegmentsViewerDialog::setInOutRequested, this, [this](qint32 segmentIndex) {
        setInOutFromSegment(segmentIndex, true, true);
    });
    connect(segmentsViewerDialog, &SegmentsViewerDialog::segmentsUpdated, this,
            [this](const QVector<TbcMetaData::Segment> &segments) {
                if (!tbcSource.getIsSourceLoaded()) {
                    return;
                }
                applySegmentEdit(segments, tr("Segments updated (%1); Save Metadata stores them").arg(segments.size()));
                updateSegmentsViewerState();
            });
    connect(segmentsViewerDialog, &SegmentsViewerDialog::rederiveRequested, this, &MainWindow::rederiveSegments);
    segmentsViewerAction = new QAction(tr("Segments Viewer..."), this);
    if (ui->menuWindow) {
        ui->menuWindow->addAction(segmentsViewerAction);
    }
    connect(segmentsViewerAction, &QAction::triggered, this, &MainWindow::showSegmentsViewer);

    // The viewer's keys, as actions: listed in the Edit menu with their
    // shortcuts (next to Copy) and enabled with the source like everything
    // else. Their shortcuts apply only while the viewer tab is showing, so
    // they never take keys from the Export tab's controls; a focused text
    // field still gets typed letters (Qt's ShortcutOverride).
    QMenu *editMenu = new QMenu(tr("&Edit"), this);
    menuBar()->insertMenu(ui->menuView->menuAction(), editMenu);
    editMenu->addAction(copyCurrentDisplayAction);
    editMenu->addSeparator();
    const auto addViewerKeyAction = [this, editMenu](const QString &text, const QList<QKeySequence> &keys,
                                                     const std::function<void()> &handler) {
        QAction *action = editMenu->addAction(text);
        action->setAutoRepeat(false);
        connect(action, &QAction::triggered, this, handler);
        viewerKeyActions.append({action, keys});
    };
    addViewerKeyAction(tr("Set &In Point Here"), {QKeySequence(Qt::Key_BracketLeft)},
                       [this]() { setInPointAtCurrentFrame(); });
    addViewerKeyAction(tr("Set &Out Point Here"), {QKeySequence(Qt::Key_BracketRight)},
                       [this]() { setOutPointAtCurrentFrame(); });
    const auto setFromSegment = [this](bool setIn, bool setOut) {
        const qint32 index = segmentIndexContainingField(currentFirstFieldZeroBased());
        if (index >= 0) {
            setInOutFromSegment(index, setIn, setOut);
        } else {
            statusBar()->showMessage(tr("No recording segment at the current frame"), 3000);
        }
    };
    addViewerKeyAction(tr("In Point from Segment &Start"),
                       {QKeySequence(Qt::Key_BraceLeft), QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft)},
                       [setFromSegment]() { setFromSegment(true, false); });
    addViewerKeyAction(tr("Out Point from Segment &End"),
                       {QKeySequence(Qt::Key_BraceRight), QKeySequence(Qt::SHIFT | Qt::Key_BracketRight)},
                       [setFromSegment]() { setFromSegment(false, true); });
    editMenu->addSeparator();
    addViewerKeyAction(tr("Add/Edit &Marker..."), {QKeySequence(Qt::Key_M), QKeySequence(Qt::SHIFT | Qt::Key_M)},
                       [this]() { addOrEditMarkerAtCurrentFrame(); });
    // The viewers' own menu items take their keys the same way
    notesViewerAction->setAutoRepeat(false);
    segmentsViewerAction->setAutoRepeat(false);
    viewerKeyActions.append({notesViewerAction, {QKeySequence(Qt::Key_C)}});
    viewerKeyActions.append({segmentsViewerAction, {QKeySequence(Qt::Key_S)}});
    connect(ui->mainTabWidget, &QTabWidget::currentChanged, this, &MainWindow::updateViewerKeyShortcuts);
    updateViewerKeyShortcuts();
    ui->posHorizontalSlider->setToolTip(
        tr("%1 / %2 set the in / out point at the current frame\n"
           "%3 adds or edits a marker comment at the current frame\n"
           "%4 opens the marker viewer")
            .arg(QKeySequence(Qt::Key_BracketLeft).toString(QKeySequence::NativeText),
                 QKeySequence(Qt::Key_BracketRight).toString(QKeySequence::NativeText),
                 QKeySequence(Qt::Key_M).toString(QKeySequence::NativeText),
                 QKeySequence(Qt::Key_C).toString(QKeySequence::NativeText)));

    // Add a status bar to show the state of the source video file
    ui->statusBar->addWidget(&sourceVideoStatus);
    ui->statusBar->addWidget(&fieldNumberStatus);
    ui->statusBar->addWidget(&vbiStatus);
    ui->statusBar->addWidget(&timeCodeStatus);
    ui->statusBar->addPermanentWidget(&cursorStatus, 1);
    sourceVideoStatus.setText(tr("No source video file loaded"));
    fieldNumberStatus.setText(tr(" Frames: ./. Fields: ./."));
    vbiStatus.hide();
    timeCodeStatus.hide();
    cursorStatus.setText(tr(" Cursor: --"));

    // Center every pop-out sub-window over this main window when it is shown
    // (project UX rule). The filter acts on Qt::Window/Qt::Dialog top-levels
    // that belong to this MainWindow, excluding popups/menus/tooltips.
    qApp->installEventFilter(this);

    if (ui->imageViewerLabel) {
        ui->imageViewerLabel->setMouseTracking(true);
        ui->imageViewerLabel->installEventFilter(this);
    }
    if (ui->scrollArea) {
        ui->scrollArea->setMouseTracking(true);
        if (ui->scrollArea->viewport()) {
            ui->scrollArea->viewport()->setMouseTracking(true);
            ui->scrollArea->viewport()->installEventFilter(this);
        }
    }

    // Set the initial field/frame number
    setCurrentFrame(1);

    // Connect to the scan line changed signal from the oscilloscope dialogue
    connect(oscilloscopeDialog, &OscilloscopeDialog::scopeCoordsChanged, this, &MainWindow::scopeCoordsChangedSignalHandler);
    lastScopeLine = 350;
    lastScopeDot = 1;

    // Make shift-clicking on the oscilloscope change the black/white level
    connect(oscilloscopeDialog, &OscilloscopeDialog::scopeLevelSelect, chromaDecoderConfigDialog, &ChromaDecoderConfigDialog::levelSelected);

    // Connect to the changed signal from the vectorscope dialogue
    connect(vectorscopeDialog, &VectorscopeDialog::scopeChanged, this, &MainWindow::vectorscopeChangedSignalHandler);
    connect(rgbScopeDialog, &RgbScopeDialog::renderTargetSizeChanged, this, [this](const QSize &) {
        if (!rgbScopeDialog || !rgbScopeDialog->isVisible() || rgbScopeDialog->isMinimized()) {
            return;
        }
        updateRgbScopeDialogue(false);
    });
    connect(yuvRangeDialog, &YuvRangeDialog::renderTargetSizeChanged, this, [this](const QSize &) {
        if (!yuvRangeDialog || !yuvRangeDialog->isVisible() || yuvRangeDialog->isMinimized()) {
            return;
        }
        updateYuvRangeScopeDialogue(false);
    });
    connect(yuvRangeDialog, &YuvRangeDialog::settingsChanged, this, [this](const YuvRangeSettings &settings) {
        yuvRangeSettings = settings;
        if (yuvRangeDialog && yuvRangeDialog->isVisible() && !yuvRangeDialog->isMinimized()) {
            updateYuvRangeScopeDialogue(true);
        }
        updateImageViewer();
    });

    // Connect to the video parameters changed signal
    connect(videoParametersDialog, &VideoParametersDialog::videoParametersChanged, this, &MainWindow::videoParametersChangedSignalHandler);
    connect(videoParametersDialog, &VideoParametersDialog::exportBoundaryToggled, this, &MainWindow::exportBoundaryToggledSignalHandler);
    connect(videoParametersDialog, &VideoParametersDialog::exportBoundaryThicknessChanged, this, &MainWindow::exportBoundaryThicknessChangedSignalHandler);

    showExportBoundary = configuration.getShowExportBoundary();
    videoParametersDialog->setShowExportBoundary(showExportBoundary);
    exportBoundaryThickness = configuration.getExportBoundaryThickness();
    videoParametersDialog->setExportBoundaryThickness(exportBoundaryThickness);

    // Connect to the chroma decoder configuration changed signal
    connect(chromaDecoderConfigDialog, &ChromaDecoderConfigDialog::chromaDecoderConfigChanged, this, &MainWindow::chromaDecoderConfigChangedSignalHandler);
    connect(chromaDecoderConfigDialog, &ChromaDecoderConfigDialog::videoLevelsChanged, this, &MainWindow::videoLevelsChangedSignalHandler);

    // Connect to the TbcSource signals (busy and finished loading)
    connect(&tbcSource, &TbcSource::busy, this, &MainWindow::onSourceBusy);
    connect(&tbcSource, &TbcSource::finishedLoading, this, &MainWindow::onSourceLoaded);
    connect(&tbcSource, &TbcSource::finishedSaving, this, &MainWindow::onSourceSaved);
    connect(&asyncFrameRenderWatcher, &QFutureWatcher<QImage>::finished,
            this, &MainWindow::onAsyncFrameRenderFinished);

    // Load the window geometry and settings from the configuration
    const QByteArray savedMainGeometry = configuration.getMainWindowGeometry();
    if (!savedMainGeometry.isEmpty()) {
        restoreGeometry(savedMainGeometry);
    }
    scaleFactor = configuration.getMainWindowScaleFactor();
    setupUiScaleMenu();

    vbiDialog->restoreGeometry(configuration.getVbiDialogGeometry());
    oscilloscopeDialog->restoreGeometry(configuration.getOscilloscopeDialogGeometry());
    if (!vectorscopeDialog->restoreGeometry(configuration.getVectorscopeDialogGeometry())) {
        vectorscopeDialog->resize(900, 730);
    }
    if (!waveformMonitorDialog->restoreGeometry(configuration.getWaveformMonitorDialogGeometry())) {
        waveformMonitorDialog->resize(900, 500);
    }
    dropoutAnalysisDialog->restoreGeometry(configuration.getDropoutAnalysisDialogGeometry());
    visibleDropoutAnalysisDialog->restoreGeometry(configuration.getVisibleDropoutAnalysisDialogGeometry());
    blackSnrAnalysisDialog->restoreGeometry(configuration.getBlackSnrAnalysisDialogGeometry());
    whiteSnrAnalysisDialog->restoreGeometry(configuration.getWhiteSnrAnalysisDialogGeometry());
    closedCaptionDialog->restoreGeometry(configuration.getClosedCaptionDialogGeometry());
    videoParametersDialog->restoreGeometry(configuration.getVideoParametersDialogGeometry());
    chromaDecoderConfigDialog->restoreGeometry(configuration.getChromaDecoderConfigDialogGeometry());
    fieldTimingDialog->restoreGeometry(configuration.getFieldTimingDialogGeometry());
    waveformMonitorDialog->setPhosphorMode(configuration.getWaveformPhosphorMode());

    // Load view options from configuration
    resizeFrameWithWindow = configuration.getResizeFrameWithWindow();
    ui->actionResizeFrameWithWindow->setChecked(resizeFrameWithWindow);

    // Initialize playback timer
    playbackTimer = new QTimer(this);
    playbackTimer->setSingleShot(true);
    playbackTimer->setTimerType(Qt::PreciseTimer);
    connect(playbackTimer, &QTimer::timeout, this, &MainWindow::stepPlayback);
    
    // Initialize resize timer for delayed frame resizing
    resizeTimer = new QTimer(this);
    resizeTimer->setSingleShot(true);
    resizeTimer->setInterval(100); // 100ms delay for resize calculations
    connect(resizeTimer, &QTimer::timeout, this, &MainWindow::resizeFrameToWindow);
    rgbScopeRefreshTimer = new QTimer(this);
    rgbScopeRefreshTimer->setSingleShot(true);
    connect(rgbScopeRefreshTimer, &QTimer::timeout, this, [this]() {
        rgbScopeRefreshPending = false;
        if (rgbScopeDialog && rgbScopeDialog->isVisible()) {
            updateRgbScopeDialogue(true);
        }
    });
    yuvRangeScopeRefreshTimer = new QTimer(this);
    yuvRangeScopeRefreshTimer->setSingleShot(true);
    connect(yuvRangeScopeRefreshTimer, &QTimer::timeout, this, [this]() {
        yuvRangeScopeRefreshPending = false;
        if (yuvRangeDialog && yuvRangeDialog->isVisible()) {
            updateYuvRangeScopeDialogue(true);
        }
    });
    
    // Initialize chroma seek mode tracking
    chromaSeekMode = false;
    originalChromaState = false;
    
    // Set up button hold detection timer
    seekTimer = new QTimer(this);
    seekTimer->setSingleShot(true);
    seekTimer->setInterval(200); // 200ms to distinguish click from hold
    connect(seekTimer, &QTimer::timeout, this, [this]() {
        // Timer expired - enter chroma seek mode
        if (configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder()) {
            chromaSeekMode = true;
            originalChromaState = true;
            tbcSource.setChromaDecoder(false);
            updateVideoPushButton();
        }
    });

    chapterSkipHoldDelayTimer = new QTimer(this);
    chapterSkipHoldDelayTimer->setSingleShot(true);
    chapterSkipHoldDelayTimer->setInterval(220);
    chapterSkipHoldRepeatTimer = new QTimer(this);
    chapterSkipHoldRepeatTimer->setSingleShot(false);
    chapterSkipHoldRepeatTimer->setInterval(110);
    connect(chapterSkipHoldDelayTimer, &QTimer::timeout, this, [this]() {
        if (!chapterSkipHoldButton || !chapterSkipHoldButton->isDown() || !tbcSource.getIsSourceLoaded()) {
            return;
        }
        chapterSkipHoldTriggered = true;
        setPlaybackRunning(false);
        skipFramesBy(chapterSkipHoldDeltaFrames);
        chapterSkipHoldRepeatTimer->start();
    });
    connect(chapterSkipHoldRepeatTimer, &QTimer::timeout, this, [this]() {
        if (!chapterSkipHoldButton || !chapterSkipHoldButton->isDown() || !tbcSource.getIsSourceLoaded()) {
            chapterSkipHoldRepeatTimer->stop();
            return;
        }
        setPlaybackRunning(false);
        skipFramesBy(chapterSkipHoldDeltaFrames);
    });
    // Button press/release signals for chroma seek mode are auto-connected by Qt's auto-connection mechanism

    // Update checker - manual checks via the Help menu plus an automatic weekly
    // background check. The auto check is deferred so it never delays startup.
    updateChecker = new UpdateChecker(this);
    connect(updateChecker, &UpdateChecker::updateAvailable,
            this, &MainWindow::onUpdateAvailable);
    connect(updateChecker, &UpdateChecker::upToDate,
            this, &MainWindow::onUpdateUpToDate);
    connect(updateChecker, &UpdateChecker::checkFailed,
            this, &MainWindow::onUpdateCheckFailed);
    QTimer::singleShot(3000, this, [this]() { maybePerformWeeklyUpdateCheck(); });

    // Set the GUI to unloaded
    updateGuiUnloaded();
    
    // Load configuration settings
    ui->actionToggleChromaDuringSeek->setChecked(configuration.getToggleChromaDuringSeek());

    // Segment-aware chapter skipping (View menu, persisted)
    skipBySegmentsAction = new QAction(tr("Skip by segments"), this);
    skipBySegmentsAction->setCheckable(true);
    skipBySegmentsAction->setChecked(configuration.getSkipBySegments());
    skipBySegmentsAction->setToolTip(tr("Previous/next chapter buttons jump between recording segments when the metadata holds any"));
    if (ui->menuView) {
        ui->menuView->addAction(skipBySegmentsAction);
    }
    connect(skipBySegmentsAction, &QAction::toggled, this, [this](bool enabled) {
        configuration.setSkipBySegments(enabled);
        configuration.writeConfiguration();
    });

    // Was a filename specified on the command line?
    if (!inputFilenameParam.isEmpty()) {
        lastFilename = inputFilenameParam;
        loadTbcFile(inputFilenameParam, metadataOnlyParam);
    } else {
        lastFilename.clear();
    }
}

MainWindow::~MainWindow()
{
    if (asyncFrameRenderInProgress) {
        cancelInFlightAsyncFrameRender();
    }
    // Save the window geometry and settings to the configuration
    configuration.setMainWindowGeometry(saveGeometry());
    configuration.setMainWindowScaleFactor(scaleFactor);
    configuration.setVbiDialogGeometry(vbiDialog->saveGeometry());
    configuration.setOscilloscopeDialogGeometry(oscilloscopeDialog->saveGeometry());
    configuration.setVectorscopeDialogGeometry(vectorscopeDialog->saveGeometry());
    configuration.setWaveformMonitorDialogGeometry(waveformMonitorDialog->saveGeometry());
    configuration.setDropoutAnalysisDialogGeometry(dropoutAnalysisDialog->saveGeometry());
    configuration.setVisibleDropoutAnalysisDialogGeometry(visibleDropoutAnalysisDialog->saveGeometry());
    configuration.setBlackSnrAnalysisDialogGeometry(blackSnrAnalysisDialog->saveGeometry());
    configuration.setWhiteSnrAnalysisDialogGeometry(whiteSnrAnalysisDialog->saveGeometry());
    configuration.setClosedCaptionDialogGeometry(closedCaptionDialog->saveGeometry());
    configuration.setVideoParametersDialogGeometry(videoParametersDialog->saveGeometry());
    configuration.setChromaDecoderConfigDialogGeometry(chromaDecoderConfigDialog->saveGeometry());
    configuration.setFieldTimingDialogGeometry(fieldTimingDialog->saveGeometry());
    configuration.setWaveformPhosphorMode(waveformMonitorDialog->phosphorMode());
    configuration.writeConfiguration();

    // Close the source video if open
    if (tbcSource.getIsSourceLoaded()) {
        tbcSource.unloadSource();
    }
    cleanupTempMetadataFile();
    delete ui;
}

// Qt's own Fusion palette in the chosen colour scheme. The choice is saved;
// the --light-theme/--force-dark-theme options only override it for one
// session. Qt repaints every widget itself on the resulting palette change.
// There is deliberately no "follow the desktop" choice: with Nix's Qt on GNOME
// the platform theme reports the GTK theme's scheme rather than GNOME's
// color-scheme setting, so it would not follow the desktop anyway.
void MainWindow::populateThemesMenu()
{
    themesActionGroup = new QActionGroup(this);
    themesActionGroup->setExclusive(true);

    const QList<QPair<QString, QString>> choices = {
        {tr("Dark"), QStringLiteral("dark")},
        {tr("Light"), QStringLiteral("light")},
    };
    for (const auto &choice : choices) {
        QAction *action = ui->menuThemes->addAction(choice.first);
        action->setCheckable(true);
        action->setChecked(choice.second == themeChoice);
        themesActionGroup->addAction(action);
        const QString value = choice.second;
        connect(action, &QAction::triggered, this, [this, value]() {
            themeChoice = value;
            tbc::ui::applyFusionTheme(value == QLatin1String("light") ? Qt::ColorScheme::Light
                                                                      : Qt::ColorScheme::Dark);
            configuration.setTheme(value);
            configuration.writeConfiguration();
        });
    }
}

// Highlight the dropouts button while the frame has dropouts. Only Button and
// ButtonText are set, from the application palette's accent roles, so every
// other role still follows the application palette. Rerun on each palette
// change (refreshThemeDependentUi) so the tint follows the colour scheme.
void MainWindow::updateDropoutsButtonTint()
{
    QPalette tint;
    if (tbcSource.getIsDropoutPresent()) {
        const QPalette appPalette = QApplication::palette();
        tint.setColor(QPalette::Button, appPalette.color(QPalette::Accent));
        tint.setColor(QPalette::ButtonText, appPalette.color(QPalette::HighlightedText));
    }
    ui->dropoutsPushButton->setPalette(tint);
}

// Re-render the images MainWindow draws from source data (the viewer and the
// scopes) after a palette or style change. Custom-painted widgets repaint
// themselves on QEvent::PaletteChange.
void MainWindow::refreshThemeDependentUi()
{
    updateDropoutsButtonTint();

    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }

    updateImageViewer();
    if (oscilloscopeDialog && oscilloscopeDialog->isVisible()) {
        updateOscilloscopeDialogue();
    }
    if (rgbScopeDialog && rgbScopeDialog->isVisible() && !rgbScopeDialog->isMinimized()) {
        updateRgbScopeDialogue(true);
    }
    if (yuvRangeDialog && yuvRangeDialog->isVisible() && !yuvRangeDialog->isMinimized()) {
        updateYuvRangeScopeDialogue(true);
    }
    if (vectorscopeDialog && vectorscopeDialog->isVisible()) {
        updateVectorscopeDialogue();
    }
    if (waveformMonitorDialog && waveformMonitorDialog->isVisible()) {
        updateWaveformMonitorDialogue();
    }
    if (fieldTimingDialog && fieldTimingDialog->isVisible()) {
        updateFieldTimingDialogue();
    }
}

bool MainWindow::event(QEvent *event)
{
    if (event && event->type() == QEvent::FileOpen) {
        const auto *fileOpenEvent = static_cast<QFileOpenEvent *>(event);
        QString filePath = fileOpenEvent->file();
        if (filePath.isEmpty() && fileOpenEvent->url().isLocalFile()) {
            filePath = fileOpenEvent->url().toLocalFile();
        }

        if (!filePath.isEmpty() && isSupportedInputExtension(filePath)) {
            requestSourceOpen(filePath);
            return true;
        }
    }

    const bool baseHandled = QMainWindow::event(event);

    // The window's modified flag is the one record of unsaved metadata edits
    // (it also puts the [*] marker in the title); Save follows it.
    if (event && event->type() == QEvent::ModifiedChange) {
        ui->actionSave_Metadata->setEnabled(tbcSource.getIsSourceLoaded() && isWindowModified());
        updateMetadataStatusPanel();
    }

    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::StyleChange)) {
        if (!themeRefreshPending) {
            themeRefreshPending = true;
            QTimer::singleShot(0, this, [this]() {
                themeRefreshPending = false;
                refreshThemeDependentUi();
            });
        }
    }

    return baseHandled;
}

// Update GUI methods for when TBC source files are loaded and unloaded -----------------------------------------------

// Enable or disable the GUI controls for whether a source is loaded
// ("enabled"). This is the one place their enabled state is decided: an
// action that can't work in the current state is disabled here, not refused
// after it is chosen. Metadata-only sources have no pictures, so everything
// that needs video data also needs !metadataOnly.
void MainWindow::setGuiEnabled(bool enabled)
{
    const bool video = enabled && !tbcSource.getIsMetadataOnly();

    // Enable the field/frame controls
    ui->posNumberSpinBox->setEnabled(enabled);
    if (ui->posTimecodeLineEdit) {
        ui->posTimecodeLineEdit->setEnabled(enabled);
    }
    ui->previousPushButton->setEnabled(enabled);
    ui->nextPushButton->setEnabled(enabled);
    ui->startPushButton->setEnabled(enabled);
    ui->endPushButton->setEnabled(enabled);
    ui->playPushButton->setEnabled(enabled);
    ui->posHorizontalSlider->setEnabled(enabled);
    ui->mediaControl_frame->setEnabled(enabled);

    // Enable menu options: those that need pictures
    ui->actionLine_scope->setEnabled(video);
    ui->actionRGB_scope->setEnabled(video);
    ui->actionYUV_range_scope->setEnabled(video);
    ui->actionVectorscope->setEnabled(video);
    ui->actionWaveform_monitor->setEnabled(video);
    ui->actionField_timing_scope->setEnabled(video);
    ui->actionSave_frame_as_PNG->setEnabled(video);
    ui->actionSave_frame_as_PNG_with_options->setEnabled(video);
    ui->actionExtract_slideshow_stills->setEnabled(video);
    if (saveAllModesPngAction) {
        saveAllModesPngAction->setEnabled(video);
    }
    if (copyCurrentDisplayAction) {
        copyCurrentDisplayAction->setEnabled(video);
    }

    // ... and those that need a source or its metadata
    ui->actionVBI->setEnabled(enabled);
    ui->actionZoom_In->setEnabled(enabled);
    ui->actionZoom_Out->setEnabled(enabled);
    ui->actionZoom_1x->setEnabled(enabled);
    ui->actionZoom_2x->setEnabled(enabled);
    ui->actionZoom_3x->setEnabled(enabled);
    ui->actionDropout_analysis->setEnabled(enabled);
    ui->actionVisible_Dropout_analysis->setEnabled(enabled);
    ui->actionSNR_analysis->setEnabled(enabled); // Black SNR
    ui->actionWhite_SNR_analysis->setEnabled(enabled);
    ui->actionClosed_Captions->setEnabled(enabled);
    ui->actionFix_JSON_SNR->setEnabled(enabled);
    ui->actionMetadata_Editor->setEnabled(enabled);
    ui->actionVideo_parameters->setEnabled(enabled);
    ui->actionChroma_decoder_configuration->setEnabled(enabled);
    ui->actionReload_TBC->setEnabled(enabled);
    ui->actionOpen_TBC_file->setEnabled(true);
    if (notesViewerAction) {
        notesViewerAction->setEnabled(enabled);
    }
    // The viewer's key actions (including the Marker and Segments viewers)
    for (const auto &entry : std::as_const(viewerKeyActions)) {
        entry.first->setEnabled(enabled);
    }

    // "Save Metadata" is available while there are unsaved edits
    ui->actionSave_Metadata->setEnabled(enabled && isWindowModified());

    // (The zoom buttons follow their actions above)
    if (vectorscopeSelectionPushButton) {
        vectorscopeSelectionPushButton->setEnabled(enabled);
    }
}


bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (!ui || !event) {
        return QMainWindow::eventFilter(watched, event);
    }

    // Center pop-out sub-windows over the main window when they are shown.
    if (event->type() == QEvent::Show) {
        if (auto *widget = qobject_cast<QWidget *>(watched)) {
            if (widget != this && widget->isWindow()) {
                const Qt::WindowFlags flags = widget->windowFlags();
                const Qt::WindowType baseType =
                    static_cast<Qt::WindowType>(static_cast<int>(flags & Qt::WindowType_Mask));
                if (baseType == Qt::Window || baseType == Qt::Dialog) {
                    for (QWidget *p = widget->parentWidget(); p; p = p->parentWidget()) {
                        if (p == this) {
                            tbc::ui::centerDialogOverParent(widget);
                            break;
                        }
                    }
                }
            }
        }
    }

    const bool watchingImageLabel = (ui->imageViewerLabel && watched == ui->imageViewerLabel);
    const bool watchingViewport = (ui->scrollArea && ui->scrollArea->viewport() && watched == ui->scrollArea->viewport());
    if (watchingImageLabel || watchingViewport) {
        if (!tbcSource.getIsSourceLoaded() || !isViewerTabActive()) {
            if (event->type() == QEvent::MouseMove || event->type() == QEvent::Leave) {
                clearCursorReadout();
                updateExportBoundaryHoverCursor(QPoint(-1, -1));
            }
            return QMainWindow::eventFilter(watched, event);
        }

        if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPoint viewerPos = ui->imageViewerLabel->mapFromGlobal(mouseEvent->globalPosition().toPoint());
            updateCursorReadout(viewerPos);
            if (exportBoundaryDragHandle == ExportBoundaryHandle::None) {
                updateExportBoundaryHoverCursor(viewerPos);
            }
        } else if (event->type() == QEvent::Wheel) {
            if (exportBoundarySelectedHandle == ExportBoundaryHandle::None) {
                return QMainWindow::eventFilter(watched, event);
            }
            const auto *wheelEvent = static_cast<QWheelEvent *>(event);
            // One step per whole notch: a touchpad's small deltas add up
            exportBoundaryWheelRemainder += wheelEvent->angleDelta().y();
            const int notches = exportBoundaryWheelRemainder / 120;
            exportBoundaryWheelRemainder -= notches * 120;
            for (int step = 0; step < qAbs(notches); ++step) {
                applyExportBoundaryWheelStep(notches > 0 ? -1 : 1);
            }
            updateExportBoundaryHoverCursor(QPoint(-1, -1));
            event->accept();
            return true;
        } else if (event->type() == QEvent::Leave) {
            clearCursorReadout();
            updateExportBoundaryHoverCursor(QPoint(-1, -1));
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::mapViewerToSourceCoordinates(const QPoint &viewerPoint, qint32 &sourceX, qint32 &sourceY) const
{
    if (!tbcSource.getIsSourceLoaded() || !ui || !ui->imageViewerLabel) {
        return false;
    }

    const QPixmap viewerPixmap = ui->imageViewerLabel->pixmap();
    // viewerPoint and QLabel::width()/height() are logical (device-independent)
    // pixels, but QPixmap::width()/height() are device pixels. On a HiDPI or
    // fractionally-scaled display the two differ, so the pixmap's logical size
    // is what the centring, bounds and normalisation below must use - mixing
    // them offsets the origin and halves the reported source coordinates.
    const QSizeF viewerSize = viewerPixmap.deviceIndependentSize();
    if (viewerPixmap.isNull() || viewerSize.width() <= 0.0 || viewerSize.height() <= 0.0) {
        return false;
    }

    const double offsetX = (static_cast<double>(ui->imageViewerLabel->width()) - viewerSize.width()) / 2.0;
    const double offsetY = (static_cast<double>(ui->imageViewerLabel->height()) - viewerSize.height()) / 2.0;
    const double localX = static_cast<double>(viewerPoint.x()) - offsetX;
    const double localY = static_cast<double>(viewerPoint.y()) - offsetY;
    if (localX < 0.0 || localY < 0.0
        || localX >= viewerSize.width()
        || localY >= viewerSize.height()) {
        return false;
    }

    const qint32 sourceWidth = (!cursorReadoutImage.isNull() && cursorReadoutImage.width() > 0)
        ? cursorReadoutImage.width()
        : tbcSource.getFrameWidth();
    const qint32 sourceHeight = (!cursorReadoutImage.isNull() && cursorReadoutImage.height() > 0)
        ? cursorReadoutImage.height()
        : tbcSource.getFrameHeight();
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return false;
    }

    sourceX = qBound<qint32>(0,
                             static_cast<qint32>((localX / viewerSize.width()) * sourceWidth),
                             sourceWidth - 1);
    sourceY = qBound<qint32>(0,
                             static_cast<qint32>((localY / viewerSize.height()) * sourceHeight),
                             sourceHeight - 1);
    return true;
}

void MainWindow::updateCursorReadout(const QPoint &viewerPoint)
{
    qint32 sourceX = 0;
    qint32 sourceY = 0;
    if (!mapViewerToSourceCoordinates(viewerPoint, sourceX, sourceY)) {
        clearCursorReadout();
        return;
    }

    QColor rgb = Qt::black;
    if (!cursorReadoutImage.isNull()
        && sourceX >= 0 && sourceX < cursorReadoutImage.width()
        && sourceY >= 0 && sourceY < cursorReadoutImage.height()) {
        rgb = cursorReadoutImage.pixelColor(sourceX, sourceY);
    }

    QString rawHex = QStringLiteral("----");
    const bool asyncNnRenderActive = asyncFrameRenderInProgress && shouldRenderFrameAsync();
    if (!asyncNnRenderActive) {
        const TbcSource::ScanLineData scanLineData = tbcSource.getScanLineData(sourceY + 1);
        if (!scanLineData.composite.isEmpty() && sourceX >= 0 && sourceX < scanLineData.composite.size()) {
            const quint16 rawValue = static_cast<quint16>(qBound(0, scanLineData.composite[sourceX], 65535));
            rawHex = QStringLiteral("0x%1").arg(rawValue, 4, 16, QChar('0')).toUpper();
        }
    }

    const QString rgbHex = QStringLiteral("#%1%2%3")
                               .arg(rgb.red(), 2, 16, QChar('0'))
                               .arg(rgb.green(), 2, 16, QChar('0'))
                               .arg(rgb.blue(), 2, 16, QChar('0'))
                               .toUpper();
    cursorStatus.setText(QStringLiteral(" Cursor X:%1 Y:%2 RGB:%3 Raw:%4")
                             .arg(sourceX)
                             .arg(sourceY)
                             .arg(rgbHex)
                             .arg(rawHex));
}

void MainWindow::clearCursorReadout()
{
    cursorStatus.setText(tr(" Cursor: --"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event) {
        return;
    }

    if (!firstSupportedDroppedFile(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event) {
        return;
    }

    if (!firstSupportedDroppedFile(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (!event) {
        return;
    }

    const QString droppedFile = firstSupportedDroppedFile(event->mimeData());
    if (droppedFile.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    if (isTeletextStreamInputExtension(droppedFile)) {
        if (!teletextViewerDialog) {
            teletextViewerDialog = new TeletextViewerDialog(this);
            teletextViewerDialog->setConfiguration(&configuration);
        }
        QString errorMessage;
        if (!teletextViewerDialog->openTeletextStream(droppedFile, &errorMessage)) {
            QMessageBox::warning(this, tr("Teletext open failed"),
                                 errorMessage.isEmpty()
                                     ? tr("Could not open the dropped .tXX teletext stream.")
                                     : errorMessage);
            return;
        }
        showOrRaise(teletextViewerDialog);
        if (statusBar()) {
            statusBar()->showMessage(tr("Opened teletext stream: %1").arg(droppedFile), 5000);
        }
        return;
    }

    lastFilename = droppedFile;
    requestSourceOpen(droppedFile);
}

void MainWindow::requestSourceOpen(const QString &inputFileName)
{
    const QString normalizedInputFileName = QDir::cleanPath(inputFileName.trimmed());
    if (normalizedInputFileName.isEmpty()) {
        return;
    }

    // Opening, reloading or dropping a file replaces the loaded metadata
    if (!maybeSave([this, normalizedInputFileName]() { requestSourceOpen(normalizedInputFileName); })) {
        return;
    }

    lastFilename = normalizedInputFileName;
    if (sourceOperationInProgress) {
        pendingSourceOpenFilename = normalizedInputFileName;
        return;
    }

    loadTbcFile(normalizedInputFileName);
}

void MainWindow::processPendingSourceOpenRequest()
{
    if (pendingSourceOpenFilename.isEmpty()) {
        return;
    }

    if (sourceOperationInProgress) {
        return;
    }

    const QString queuedInputFile = pendingSourceOpenFilename;
    pendingSourceOpenFilename.clear();
    QTimer::singleShot(0, this, [this, queuedInputFile]() {
        requestSourceOpen(queuedInputFile);
    });
}

TbcSource& MainWindow::getTbcSource()
{
	return this->tbcSource;
}

void MainWindow::resetGui()
{
    setPlaybackRunning(false);
    ui->posNumberSpinBox->setMinimum(1);
    ui->posHorizontalSlider->setMinimum(1);

    ui->posNumberSpinBox->setValue(1);
    if (ui->posTimecodeLineEdit) {
        ui->posTimecodeLineEdit->setText(frameToTimecode(1));
        ui->posTimecodeLineEdit->setVisible(true);
    }
    ui->posHorizontalSlider->setValue(1);
    ui->dropoutsPushButton->setChecked(tbcSource.getHighlightDropouts());

    setViewValues();

    // Allow the next and previous frame buttons to auto-repeat
    ui->previousPushButton->setAutoRepeat(true);
    ui->previousPushButton->setAutoRepeatDelay(500);
    ui->previousPushButton->setAutoRepeatInterval(1);
    ui->nextPushButton->setAutoRepeat(true);
    ui->nextPushButton->setAutoRepeatDelay(500);
    ui->nextPushButton->setAutoRepeatInterval(1);

    // Set option button states
    updateVideoPushButton();
    displayAspectRatio = true;
    updateAspectPushButton();
    updateSourcesPushButton();
    ui->fieldOrderPushButton->setChecked(tbcSource.getFieldOrder());

    // Zoom button options
    ui->zoomInPushButton->setAutoRepeat(true);
    ui->zoomInPushButton->setAutoRepeatDelay(500);
    ui->zoomInPushButton->setAutoRepeatInterval(100);
    ui->zoomOutPushButton->setAutoRepeat(true);
    ui->zoomOutPushButton->setAutoRepeatDelay(500);
    ui->zoomOutPushButton->setAutoRepeatInterval(100);

    // Initialize field stretch to 2:1 by default
    tbcSource.setStretchField(true);

    // Update the video parameters dialogue
    videoParametersDialog->setVideoParameters(tbcSource.getVideoParameters());

    // Update the chroma decoder configuration dialogue
    chromaDecoderConfigDialog->setConfiguration(tbcSource.getSystem(), tbcSource.getPalConfiguration(),
                                                tbcSource.getNtscConfiguration(),
                                                tbcSource.getMonoConfiguration(),
                                                tbcSource.getSourceMode(),
                                                true); // set to true because the chroma decoder is already init
    chromaDecoderConfigDialog->setVideoLevels(tbcSource.getVideoParameters());
}

// Method to update the GUI when a file is loaded
void MainWindow::updateGuiLoaded()
{
    // Enable the GUI controls
    setGuiEnabled(true);
    setPlaybackRunning(false);

    // Update the status bar readout
    updateBottomStatusReadout();
    // Update video mode button
    updateVideoPushButton();

    // Update source mode button
    updateSourcesPushButton();

    // Load and show the current image
    showImage();

    // Update the video parameters dialogue
    videoParametersDialog->setVideoParameters(tbcSource.getVideoParameters());

    // Update the chroma decoder configuration dialogue
    chromaDecoderConfigDialog->setConfiguration(tbcSource.getSystem(), tbcSource.getPalConfiguration(),
                                                tbcSource.getNtscConfiguration(),
                                                tbcSource.getMonoConfiguration(),
                                                tbcSource.getSourceMode(),
                                                false); // set to false to init the chroma decoder selection
    chromaDecoderConfigDialog->setVideoLevels(tbcSource.getVideoParameters());

    // Keep load-time sizing stable: either fit frame to existing window, or
    // resize window to image size (legacy auto-resize behavior), but not both.
    if (resizeFrameWithWindow) {
        resizeTimer->start();
    } else if (autoResize) {
        resize_on_aspect();
    }
    updateTimelineMarkers();
    updateNotesViewerState();
    updateSegmentsViewerState();

    updateMetadataStatusPanel();
}

// Method to update the GUI when a file is unloaded
void MainWindow::updateGuiUnloaded()
{
    setPlaybackRunning(false);
    vectorscopeSelectionDragging = false;
    exportBoundaryDragHandle = ExportBoundaryHandle::None;
    exportBoundarySelectedHandle = ExportBoundaryHandle::None;
    updateExportBoundaryHoverCursor(QPoint(-1, -1));
    if (vectorscopeSelectionPushButton) {
        vectorscopeSelectionPushButton->setChecked(false);
    }
    // Disable the GUI controls
    setGuiEnabled(false);

    // Update the current field/frame number
    setCurrentFrame(1);
    ui->posNumberSpinBox->setValue(1);
    ui->posNumberSpinBox->setVisible(false);
    if (ui->posNumberSpinBoxLabel) {
        ui->posNumberSpinBoxLabel->setVisible(false);
    }
    if (ui->posTimecodeLineEdit) {
        ui->posTimecodeLineEdit->setText(frameToTimecode(1));
        ui->posTimecodeLineEdit->setVisible(true);
    }
    ui->posHorizontalSlider->setValue(1);

    // Set the window title
    setWindowFilePath(QString());
    setWindowTitle(tr("tbc-analyse[*]"));

    // Set the status bar text
    sourceVideoStatus.setText(tr("No source video file loaded"));
    fieldNumberStatus.setText(tr(" Frames: ./. Fields: ./."));
    vbiStatus.hide();
    timeCodeStatus.hide();

    // Set option button states
    updateVideoPushButton();
    ui->dropoutsPushButton->setChecked(false);
    displayAspectRatio = false;
    updateAspectPushButton();
    updateSourcesPushButton();
    ui->fieldOrderPushButton->setChecked(false);

    // Hide the displayed image
    hideImage();

    // Hide graphs
    blackSnrAnalysisDialog->hide();
    whiteSnrAnalysisDialog->hide();
    dropoutAnalysisDialog->hide();

    // Hide configuration dialogues
    videoParametersDialog->hide();
    chromaDecoderConfigDialog->hide();
    fieldTimingDialog->hide();
    rgbScopeDialog->hide();
    yuvRangeDialog->hide();
    if (rgbScopeRefreshTimer) {
        rgbScopeRefreshTimer->stop();
    }
    rgbScopeRefreshPending = false;
    rgbScopeLastRefreshMs = 0;
    if (yuvRangeScopeRefreshTimer) {
        yuvRangeScopeRefreshTimer->stop();
    }
    yuvRangeScopeRefreshPending = false;
    yuvRangeScopeLastRefreshMs = 0;
    updateTimelineMarkers();
    updateNotesViewerState();
    updateSegmentsViewerState();

    updateMetadataStatusPanel();
    if (exportDialog) {
        exportDialog->setSource(&tbcSource);
    }
}

void MainWindow::updateVideoPushButton()
{
    if (!ui || !ui->videoPushButton) {
        return;
    }

    if (!tbcSource.getChromaDecoder()) {
        ui->videoPushButton->setText(tr("Source"));
        return;
    }

    switch (tbcSource.getChromaDecodeMode()) {
    case TbcSource::ACTIVE_ONLY_CHROMA_MODE:
        ui->videoPushButton->setText(tr("Active"));
        break;
    case TbcSource::HYBRID_CHROMA_MODE:
        ui->videoPushButton->setText(tr("Hybrid"));
        break;
    case TbcSource::FULL_FRAME_CHROMA_MODE:
        ui->videoPushButton->setText(tr("Chroma"));
        break;
    }
}
// Update the aspect ratio button
void MainWindow::updateAspectPushButton()
{
    if (!displayAspectRatio) {
        ui->aspectPushButton->setText(tr("SAR 1:1"));
    } else if (tbcSource.getIsWidescreen()) {
        ui->aspectPushButton->setText(tr("DAR 16:9"));
    } else {
        ui->aspectPushButton->setText(tr("DAR 4:3"));
    }
}

// Update the source selection button
void MainWindow::updateSourcesPushButton()
{
	// Only show the button if there are multiple sources (not ONE_SOURCE) AND a source is loaded
	if (tbcSource.getSourceMode() != TbcSource::ONE_SOURCE && tbcSource.getIsSourceLoaded()) {
		ui->sourcesPushButton->setVisible(true);
	} else {
		// Hide the button by default (no source loaded or only one source)
		ui->sourcesPushButton->setVisible(false);
		chromaDecoderConfigDialog->updateSourceMode(tbcSource.getSourceMode());
		return;
	}
	
	switch (tbcSource.getSourceMode()) {
	case TbcSource::ONE_SOURCE:
		// This case should not be reached due to early return above
		break;
	case TbcSource::LUMA_SOURCE:
		ui->sourcesPushButton->setText(tr("Y"));
		ui->sourcesPushButton->setToolTip(tr("Showing the Y (luma) source; click to switch sources"));
		break;
	case TbcSource::CHROMA_SOURCE:
		ui->sourcesPushButton->setText(tr("C"));
		ui->sourcesPushButton->setToolTip(tr("Showing the C (chroma) source; click to switch sources"));
		break;
	case TbcSource::BOTH_SOURCES:
		ui->sourcesPushButton->setText(tr("Y/C"));
		ui->sourcesPushButton->setToolTip(tr("Showing both Y/C sources; click to switch sources"));
		break;
	}
	chromaDecoderConfigDialog->updateSourceMode(tbcSource.getSourceMode());
}

void MainWindow::updateMetadataStatusPanel()
{
    if (!ui || !metadataStatusDialog) {
        return;
    }
    MetadataStatusData data;

    if (!tbcSource.getIsSourceLoaded()) {
        data.dbPath = QStringLiteral("—");
        data.jsonPath = QStringLiteral("—");
        data.videoSystem = QStringLiteral("—");
        data.chromaDecoder = QStringLiteral("—");
        data.chromaGain = QStringLiteral("—");
        data.chromaPhase = QStringLiteral("—");
        data.lumaNr = QStringLiteral("—");
        data.ntscAdaptive = QStringLiteral("—");
        data.ntscAdaptThreshold = QStringLiteral("—");
        data.ntscChromaWeight = QStringLiteral("—");
        data.ntscPhaseComp = QStringLiteral("—");
        data.palTransformThreshold = QStringLiteral("—");
        data.savePending = QStringLiteral("No");
        metadataStatusDialog->updateStatus(data);
        return;
    }

    const TbcMetaData::VideoParameters &videoParameters = tbcSource.getVideoParameters();

    const QString metadataPath = tbcSource.getCurrentMetadataFilename();
    const bool metadataIsJson = metadataPath.endsWith(".json", Qt::CaseInsensitive);
    if (metadataIsJson) {
        data.dbPath = QStringLiteral("—");
        data.jsonPath = metadataPath;
    } else {
        data.dbPath = formatOptionalString(metadataPath);
        if (metadataJsonLoaded && !metadataJsonFilename.isEmpty()) {
            data.jsonPath = metadataJsonFilename;
        } else {
            data.jsonPath = QStringLiteral("—");
        }
    }
    data.videoSystem = tbcSource.getSystemDescription();
    data.chromaDecoder = formatOptionalString(videoParameters.chromaDecoder);
    data.chromaGain = formatOptionalDouble(videoParameters.chromaGain);
    data.chromaPhase = formatOptionalDouble(videoParameters.chromaPhase);
    data.lumaNr = formatOptionalDouble(videoParameters.lumaNR);
    data.ntscAdaptive = formatOptionalBoolFromInt(videoParameters.ntscAdaptive);
    data.ntscAdaptThreshold = formatOptionalDouble(videoParameters.ntscAdaptThreshold);
    data.ntscChromaWeight = formatOptionalDouble(videoParameters.ntscChromaWeight);
    data.ntscPhaseComp = formatOptionalBoolFromInt(videoParameters.ntscPhaseCompensation);
    data.palTransformThreshold = formatOptionalDouble(videoParameters.palTransformThreshold);
    data.savePending = isWindowModified() ? QStringLiteral("Yes") : QStringLiteral("No");

    metadataStatusDialog->updateStatus(data);
}

// Frame display methods ----------------------------------------------------------------------------------------------

bool MainWindow::shouldRenderFrameAsync() const
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return false;
    }
    if (!tbcSource.getChromaDecoder()) {
        return false;
    }
    if (tbcSource.getSystem() != NTSC) {
        return false;
    }
    return tbcSource.getNtscConfiguration().nnTransform3D;
}

void MainWindow::startAsyncFrameRender()
{
    if (asyncFrameRenderInProgress || !shouldRenderFrameAsync()) {
        return;
    }

    asyncFrameRenderInProgress = true;
    asyncFrameRenderQueued = false;
    asyncFrameRenderFrameNumber = currentFrameNumber;
    asyncFrameRenderFieldNumber = currentFieldNumber;

    QFuture<QImage> renderFuture = QtConcurrent::run(&TbcSource::getImage, &tbcSource);
    asyncFrameRenderWatcher.setFuture(renderFuture);

    if (statusBar()) {
        statusBar()->showMessage(tr("Rendering nnTransform3D frame..."));
    }
}

void MainWindow::onAsyncFrameRenderFinished()
{
    asyncFrameRenderInProgress = false;
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        asyncFrameImage = QImage();
        asyncFrameRenderQueued = false;
        if (statusBar()) {
            statusBar()->clearMessage();
        }
        return;
    }
    asyncFrameImage = asyncFrameRenderWatcher.result();

    if (asyncFrameRenderQueued
        || asyncFrameRenderFrameNumber != currentFrameNumber
        || asyncFrameRenderFieldNumber != currentFieldNumber) {
        asyncFrameRenderQueued = false;
        QTimer::singleShot(0, this, [this]() {
            showImage();
        });
        return;
    }

    if (statusBar()) {
        statusBar()->clearMessage();
    }
    updateImage();
}

void MainWindow::cancelInFlightAsyncFrameRender()
{
    if (!asyncFrameRenderInProgress) {
        return;
    }
    tbcSource.requestNnTransform3DCancel();
    asyncFrameRenderWatcher.waitForFinished();
    asyncFrameRenderInProgress = false;
    asyncFrameRenderQueued = false;
    asyncFrameRenderFrameNumber = -1;
    asyncFrameRenderFieldNumber = -1;
    asyncFrameImage = QImage();
    if (statusBar()) {
        statusBar()->clearMessage();
    }
}

// Update the UI and displays when currentFrameNumber or currentFieldNumber has changed
void MainWindow::showImage()
{
    if (tbcSourceBusy) {
        showImagePending = true;
        return;
    }
    if (asyncFrameRenderInProgress) {
        if (shouldRenderFrameAsync()) {
            asyncFrameRenderQueued = true;
            return;
        }
        cancelInFlightAsyncFrameRender();
        asyncFrameImage = QImage();
    }
    tbcSource.load(currentFrameNumber, currentFieldNumber);

    updateBottomStatusReadout();

    // Show VBI position in the status bar, if available
    if (tbcSource.getIsFrameVbiValid()) {
        VbiDecoder::Vbi vbi = tbcSource.getFrameVbi();
        if (vbi.clvHr != -1) {
            vbiStatus.setText(QString(" -  CLV time code: %1:%2:%3")
                                  .arg(vbi.clvHr, 2, 10, QChar('0'))
                                  .arg(vbi.clvMin, 2, 10, QChar('0'))
                                  .arg(vbi.clvSec, 2, 10, QChar('0')));
            vbiStatus.show();
        } else if (vbi.picNo != -1) {
            vbiStatus.setText(QString(" -  CAV picture number: %1")
                                  .arg(vbi.picNo, 5, 10, QChar('0')));
            vbiStatus.show();
        } else {
            vbiStatus.hide();
        }
    } else {
        vbiStatus.hide();
    }

    // Show timecode in the status bar, if available
    if (tbcSource.getIsFrameVitcValid()) {
        // Use ; rather than : if the drop flag is set (as ffmpeg does)
        VitcDecoder::Vitc vitc = tbcSource.getFrameVitc();
        timeCodeStatus.setText(QString(" -  VITC time code: %1:%2:%3%4%5")
                                   .arg(vitc.hour, 2, 10, QChar('0'))
                                   .arg(vitc.minute, 2, 10, QChar('0'))
                                   .arg(vitc.second, 2, 10, QChar('0'))
                                   .arg(vitc.isDropFrame ? QChar(';') : QChar(':'))
                                   .arg(vitc.frame, 2, 10, QChar('0')));
        timeCodeStatus.show();
    } else {
        timeCodeStatus.hide();
    }

    // If there are dropouts in the frame, highlight the show dropouts button
    updateDropoutsButtonTint();

    // Update the VBI dialogue
    if (vbiDialog->isVisible()) {
        vbiDialog->updateVbi(tbcSource.getFrameVbi(), tbcSource.getIsFrameVbiValid());
        vbiDialog->updateVideoId(tbcSource.getFrameVideoId(), tbcSource.getIsFrameVideoIdValid());
    }

    // Add the QImage to the QLabel in the dialogue
    ui->imageViewerLabel->clear();
    ui->imageViewerLabel->setScaledContents(false);
    ui->imageViewerLabel->setAlignment(Qt::AlignCenter);

    // Update the field/frame image
    if (shouldRenderFrameAsync()) {
        startAsyncFrameRender();
        updateImageViewer();
    } else {
        updateImage();
    }

    // Update the closed caption dialog
    closedCaptionDialog->addData(currentFrameNumber, tbcSource.getCcData0(), tbcSource.getCcData1());

    // QT Bug workaround for some macOS versions
    #if defined(Q_OS_MACOS)
    	repaint();
    #endif
}

// Redraw all the GUI elements that depend on the decoded field/frame
void MainWindow::updateImage()
{
    // Update the image viewer
    updateImageViewer();
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }

    // If the scope dialogues are open, update them
    if (oscilloscopeDialog->isVisible()) {
        updateOscilloscopeDialogue();
    }
    if (rgbScopeDialog->isVisible() && !rgbScopeDialog->isMinimized()) {
        updateRgbScopeDialogue();
    }
    if (yuvRangeDialog->isVisible() && !yuvRangeDialog->isMinimized()) {
        updateYuvRangeScopeDialogue();
    }
    if (vectorscopeDialog->isVisible()) {
        updateVectorscopeDialogue();
    }
    if (waveformMonitorDialog->isVisible()) {
        updateWaveformMonitorDialogue();
    }
    if (fieldTimingDialog->isVisible()) {
        updateFieldTimingDialogue();
    }
}

// Return the width adjustment for the current aspect mode
qint32 MainWindow::getAspectAdjustment() const {
    // Using source aspect ratio? No adjustment
    if (!displayAspectRatio) return 0;

    return FrameSnapshot::viewerAspectAdjustment(tbcSource.getVideoParameters());
}

bool MainWindow::isViewerTabActive() const
{
    if (!ui || !ui->mainTabWidget || !ui->viewerTab) {
        return true;
    }
    return ui->mainTabWidget->currentWidget() == ui->viewerTab;
}

QImage MainWindow::renderedCurrentFrameImage()
{
    const bool useAsyncRender = shouldRenderFrameAsync();
    if (asyncFrameRenderInProgress && !useAsyncRender) {
        cancelInFlightAsyncFrameRender();
        asyncFrameImage = QImage();
    }
    if (!useAsyncRender) {
        return tbcSource.getImage();
    }

    // nnTransform3D renders off the UI thread. Wait for the frame on screen
    // rather than failing because it has not finished yet.
    const QString label = tr("Rendering nnTransform3D frame...");
    auto isCurrent = [this]() {
        return asyncFrameRenderFrameNumber == currentFrameNumber
               && asyncFrameRenderFieldNumber == currentFieldNumber;
    };
    if (asyncFrameRenderInProgress) {
        const QImage rendered = waitWithProgress(this, asyncFrameRenderWatcher.future(), label);
        if (isCurrent()) {
            return rendered;
        }
    } else if (!asyncFrameImage.isNull() && isCurrent()) {
        return asyncFrameImage;
    }
    return waitWithProgress(this, QtConcurrent::run(&TbcSource::getImage, &tbcSource), label);
}

QImage MainWindow::renderedCurrentImageForExport()
{
    QImage imageToSave = renderedCurrentFrameImage();
    if (imageToSave.isNull()) {
        return imageToSave;
    }

    const qint32 adjustment = getAspectAdjustment();
    if (adjustment != 0) {
        const qint32 adjustedWidth = imageToSave.size().width() + adjustment;
        if (adjustedWidth <= 0 || imageToSave.size().height() <= 0) {
            return QImage();
        }
        imageToSave = imageToSave.scaled(adjustedWidth,
                                         imageToSave.size().height(),
                                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return imageToSave;
}

QString MainWindow::outputRootDirectoryForCurrentSource()
{
    QString directoryPath;
    const QString currentSourceFilename = tbcSource.getCurrentSourceFilename();
    if (!currentSourceFilename.isEmpty()) {
        const QFileInfo sourceInfo(currentSourceFilename);
        directoryPath = sourceInfo.absolutePath();
    }
    if (directoryPath.isEmpty() && !lastFilename.isEmpty()) {
        const QFileInfo sourceInfo(lastFilename);
        directoryPath = sourceInfo.absolutePath();
    }
    if (directoryPath.isEmpty()) {
        directoryPath = configuration.getSourceDirectory();
    }
    if (directoryPath.isEmpty()) {
        directoryPath = QDir::currentPath();
    }
    return directoryPath;
}

QString MainWindow::outputBaseNameForCurrentSource()
{
    QString baseName;
    const QString currentSourceFilename = tbcSource.getCurrentSourceFilename();
    if (!currentSourceFilename.isEmpty()) {
        baseName = QFileInfo(currentSourceFilename).completeBaseName();
    }
    if (baseName.isEmpty() && !lastFilename.isEmpty()) {
        baseName = QFileInfo(lastFilename).completeBaseName();
    }
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("tbc_capture");
    }
    return sanitizedFileToken(baseName);
}

void MainWindow::applyEfmHandlerAutoloads(const QString &directoryPath)
{
    if (!efmHandlerDialog) {
        return;
    }

    const QString normalizedDirectory = QDir::cleanPath(directoryPath.trimmed());
    const QFileInfo directoryInfo(normalizedDirectory);
    if (normalizedDirectory.isEmpty() || !directoryInfo.exists() || !directoryInfo.isDir()) {
        return;
    }

    QString sourceBaseName;
    const QString sourceFilename = tbcSource.getCurrentSourceFilename();
    if (!sourceFilename.isEmpty()) {
        sourceBaseName = QFileInfo(sourceFilename).completeBaseName();
    } else if (!lastFilename.isEmpty()) {
        sourceBaseName = QFileInfo(lastFilename).completeBaseName();
    }

    const EfmAutoloadCandidates candidates =
        discoverEfmAutoloadCandidates(normalizedDirectory, sourceBaseName);
    for (const QString &efmInput : candidates.efmInputs) {
        efmHandlerDialog->setDefaultEfmInput(efmInput);
    }
    if (!candidates.ac3Input.isEmpty()) {
        efmHandlerDialog->setDefaultAc3Input(candidates.ac3Input);
    }

    if (exportDialog
        && !candidates.pcmInputs.isEmpty()
        && !exportDialog->hasAudioTracksConfigured()) {
        const QStringList trackFiles = candidates.pcmInputs.mid(0, 4);
        QStringList trackNames;
        trackNames.reserve(trackFiles.size());
        for (const QString &trackFile : trackFiles) {
            trackNames << QFileInfo(trackFile).completeBaseName();
        }
        exportDialog->loadAudioTracksForExport(trackFiles, trackNames);
    }
}

// Redraw the viewer (for example, when scaleFactor has been changed)
void MainWindow::updateImageViewer()
{
    const bool useAsyncRender = shouldRenderFrameAsync();
    if (asyncFrameRenderInProgress && !useAsyncRender) {
        cancelInFlightAsyncFrameRender();
        asyncFrameImage = QImage();
    }
    QImage image;
    if (useAsyncRender) {
        image = asyncFrameImage;
        if (image.isNull() && !asyncFrameRenderInProgress) {
            startAsyncFrameRender();
        }
    } else {
        image = tbcSource.getImage();
        asyncFrameImage = QImage();
    }
    cursorReadoutImage = image;
    if (image.isNull() || image.width() == 0 || image.height() == 0) {
        cursorReadoutImage = QImage();
        clearCursorReadout();
        if (tbcSource.getIsMetadataOnly()) {
            ui->imageViewerLabel->setText(tr("Metadata-only mode (no TBC image data)"));
        } else if (useAsyncRender && asyncFrameRenderInProgress) {
            ui->imageViewerLabel->setText(tr("Rendering nnTransform3D frame..."));
        }
    }

    if (ui->mouseModePushButton->isChecked() && !image.isNull() && image.width() > 0 && image.height() > 0) {
        // Create a painter object
        QPainter imagePainter;
        imagePainter.begin(&image);

        // Draw lines to indicate the current scope position
        imagePainter.setPen(QColor(0, 255, 0, 127));
        imagePainter.drawLine(0, lastScopeLine - 1, tbcSource.getFrameWidth(), lastScopeLine - 1);
        imagePainter.drawLine(lastScopeDot, 0, lastScopeDot, tbcSource.getFrameHeight());

        // End the painter object
        imagePainter.end();
    }

    QPixmap pixmap = QPixmap::fromImage(image);

    // Get the aspect ratio adjustment if required
    qint32 adjustment = getAspectAdjustment();

    // Scale and apply the pixmap (only if it's valid)
    if (!pixmap.isNull()) {
        // width/height are logical (device-independent) pixels: the size the
        // frame occupies in the layout. The pixmap itself is rendered at the
        // display's device pixel ratio so it stays sharp on a HiDPI or
        // fractionally-scaled screen, and carries that ratio so Qt lays it out
        // at the logical size instead of upscaling it a second time.
        //
        // NOTE: from here on scaledPixmap.width()/height() are DEVICE pixels
        // while every QPainter below works in LOGICAL ones - use width/height.
        const int width = static_cast<int>(scaleFactor * (pixmap.size().width() + adjustment));
        const int height = static_cast<int>(scaleFactor * pixmap.size().height());
        const qreal devicePixelRatio = ui->imageViewerLabel->devicePixelRatioF();
        QPixmap scaledPixmap = pixmap.scaled(QSize(width, height) * devicePixelRatio,
                                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaledPixmap.setDevicePixelRatio(devicePixelRatio);

        if (yuvRangeSettings.overlayEnabled && !tbcSource.getIsMetadataOnly()) {
            const QImage overlayImage = tbcSource.getYuvRangeOverlayImage(yuvRangeSettings);
            if (!overlayImage.isNull() && overlayImage.width() > 0 && overlayImage.height() > 0) {
                // The overlay is source resolution, so scaling it to the frame's
                // device size costs nothing and keeps it as sharp as the frame.
                QPixmap scaledOverlay = QPixmap::fromImage(overlayImage).scaled(
                    scaledPixmap.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
                scaledOverlay.setDevicePixelRatio(devicePixelRatio);
                QPainter painter(&scaledPixmap);
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.drawPixmap(0, 0, scaledOverlay);
            }
        }

        if (showExportBoundary) {
            const QVector<QRect> activeRects = getActiveVideoRects();
            if (!activeRects.isEmpty() && pixmap.width() > 0 && pixmap.height() > 0) {
                const double scaleX = static_cast<double>(width) / static_cast<double>(pixmap.width());
                const double scaleY = static_cast<double>(height) / static_cast<double>(pixmap.height());
                QPainter painter(&scaledPixmap);
                painter.setRenderHint(QPainter::Antialiasing, false);

                QPen pen(QColor(255, 0, 0));
                const int thickness = (exportBoundaryThickness < 1) ? 1 : ((exportBoundaryThickness > 8) ? 8 : exportBoundaryThickness);
                pen.setWidth(thickness);
                pen.setJoinStyle(Qt::MiterJoin);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);

                const int inset = pen.width() / 2;
                for (const QRect &rect : activeRects) {
                    QRect scaledRect(qRound(rect.x() * scaleX),
                                     qRound(rect.y() * scaleY),
                                     qRound(rect.width() * scaleX),
                                     qRound(rect.height() * scaleY));
                    QRect borderRect = scaledRect.adjusted(inset, inset, -inset, -inset);
                    if (borderRect.width() > 0 && borderRect.height() > 0) {
                        painter.drawRect(borderRect);
                    }
                }
            }
        }

        const bool showVectorscopeSelection = vectorscopeDialog
            && (vectorscopeDialog->isCustomAreaModeSelected() || vectorscopeSelectionDragging);
        if (showVectorscopeSelection && !cursorReadoutImage.isNull()
            && cursorReadoutImage.width() > 0 && cursorReadoutImage.height() > 0) {
            const QRect sourceSelectionRect = vectorscopeDialog->customAreaRect();
            if (sourceSelectionRect.width() > 0 && sourceSelectionRect.height() > 0) {
                const double scaleX = static_cast<double>(width) / static_cast<double>(cursorReadoutImage.width());
                const double scaleY = static_cast<double>(height) / static_cast<double>(cursorReadoutImage.height());
                QRect scaledSelectionRect(qRound(sourceSelectionRect.x() * scaleX),
                                          qRound(sourceSelectionRect.y() * scaleY),
                                          qMax(1, qRound(sourceSelectionRect.width() * scaleX)),
                                          qMax(1, qRound(sourceSelectionRect.height() * scaleY)));
                scaledSelectionRect = scaledSelectionRect.intersected(QRect(0, 0, width, height));

                if (scaledSelectionRect.width() > 0 && scaledSelectionRect.height() > 0) {
                    QPainter painter(&scaledPixmap);
                    painter.setRenderHint(QPainter::Antialiasing, false);
                    QPen selectionPen(QColor(255, 255, 0, 220));
                    selectionPen.setWidth(2);
                    selectionPen.setStyle(Qt::DashLine);
                    painter.setPen(selectionPen);
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(scaledSelectionRect.adjusted(0, 0, -1, -1));
                }
            }
        }

        ui->imageViewerLabel->setPixmap(scaledPixmap);
    }

    // Update the current frame markers on the graphs
    blackSnrAnalysisDialog->updateFrameMarker(currentFrameNumber);
    whiteSnrAnalysisDialog->updateFrameMarker(currentFrameNumber);
    dropoutAnalysisDialog->updateFrameMarker(currentFrameNumber);
    visibleDropoutAnalysisDialog->updateFrameMarker(currentFrameNumber);

    // QT Bug workaround for some macOS versions
    #if defined(Q_OS_MACOS)
    	repaint();
    #endif
}

QVector<QRect> MainWindow::getActiveVideoRects() const
{
    QVector<QRect> rects;

    if (!tbcSource.getIsSourceLoaded()) {
        return rects;
    }

    const TbcMetaData::VideoParameters &videoParameters = tbcSource.getVideoParameters();
    if (videoParameters.activeVideoStart < 0 || videoParameters.activeVideoEnd <= videoParameters.activeVideoStart) {
        return rects;
    }

    const int frameWidth = tbcSource.getFrameWidth();
    const int frameHeight = tbcSource.getFrameHeight();
    if (frameWidth <= 0 || frameHeight <= 0) {
        return rects;
    }

    const int rectWidth = videoParameters.activeVideoEnd - videoParameters.activeVideoStart;

    auto appendRect = [&](int x, int y, int width, int height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        QRect rect(x, y, width, height);
        rect = rect.intersected(QRect(0, 0, frameWidth, frameHeight));
        if (rect.width() > 0 && rect.height() > 0) {
            rects.append(rect);
        }
    };

    switch (tbcSource.getViewMode()) {
    case TbcSource::ViewMode::FRAME_VIEW: {
        const QRect rect = FrameSnapshot::activeFrameRect(videoParameters, QSize(frameWidth, frameHeight));
        appendRect(rect.x(), rect.y(), rect.width(), rect.height());
        break;
    }
    case TbcSource::ViewMode::SPLIT_VIEW: {
        if (videoParameters.firstActiveFieldLine < 0 ||
            videoParameters.lastActiveFieldLine <= videoParameters.firstActiveFieldLine) {
            return rects;
        }
        const int fieldHeight = videoParameters.lastActiveFieldLine - videoParameters.firstActiveFieldLine;
        appendRect(videoParameters.activeVideoStart, videoParameters.firstActiveFieldLine - 1, rectWidth, fieldHeight);
        appendRect(videoParameters.activeVideoStart,
                   videoParameters.firstActiveFieldLine + (frameHeight / 2),
                   rectWidth,
                   fieldHeight);
        break;
    }
    case TbcSource::ViewMode::FIELD_VIEW: {
        if (videoParameters.firstActiveFieldLine < 0 ||
            videoParameters.lastActiveFieldLine <= videoParameters.firstActiveFieldLine) {
            return rects;
        }
        const int fieldHeight = videoParameters.lastActiveFieldLine - videoParameters.firstActiveFieldLine;
        if (tbcSource.getStretchField()) {
            appendRect(videoParameters.activeVideoStart,
                       (videoParameters.firstActiveFieldLine - 1) * 2,
                       rectWidth,
                       fieldHeight * 2);
        } else {
            appendRect(videoParameters.activeVideoStart,
                       (videoParameters.firstActiveFieldLine - 1) + (frameHeight / 4),
                       rectWidth,
                       fieldHeight);
        }
        break;
    }
    case TbcSource::ViewMode::RGB_SCOPE_VIEW:
        break;
    }

    return rects;
}

bool MainWindow::isExportBoundaryDragAvailable() const
{
    if (!showExportBoundary || !tbcSource.getIsSourceLoaded() || !isViewerTabActive()) {
        return false;
    }
    if (!ui || !ui->imageViewerLabel || !ui->mouseModePushButton) {
        return false;
    }
    if (ui->mouseModePushButton->isChecked()) {
        return false;
    }
    if (vectorscopeSelectionDragging) {
        return false;
    }
    if (vectorscopeSelectionPushButton && vectorscopeSelectionPushButton->isChecked()) {
        return false;
    }
    if (tbcSource.getViewMode() != TbcSource::ViewMode::FRAME_VIEW) {
        return false;
    }
    return !getActiveVideoRects().isEmpty();
}

MainWindow::ExportBoundaryHandle MainWindow::exportBoundaryHandleAtViewerPoint(const QPoint &viewerPoint) const
{
    if (!isExportBoundaryDragAvailable()) {
        return ExportBoundaryHandle::None;
    }

    qint32 sourceX = 0;
    qint32 sourceY = 0;
    if (!mapViewerToSourceCoordinates(viewerPoint, sourceX, sourceY)) {
        return ExportBoundaryHandle::None;
    }

    const QVector<QRect> activeRects = getActiveVideoRects();
    if (activeRects.isEmpty()) {
        return ExportBoundaryHandle::None;
    }
    const QRect activeRect = activeRects.first();
    if (activeRect.width() <= 0 || activeRect.height() <= 0) {
        return ExportBoundaryHandle::None;
    }

    const qint32 left = activeRect.left();
    const qint32 right = activeRect.right();
    const qint32 top = activeRect.top();
    const qint32 bottom = activeRect.bottom();
    const qint32 handleTolerance = qMax<qint32>(
        2,
        static_cast<qint32>(qCeil(6.0 / qMax(0.1, scaleFactor))));

    ExportBoundaryHandle bestHandle = ExportBoundaryHandle::None;
    qint32 bestDistance = handleTolerance + 1;
    const auto considerHandle = [&](ExportBoundaryHandle handle, qint32 distance) {
        if (distance > handleTolerance) {
            return;
        }
        if (bestHandle == ExportBoundaryHandle::None || distance < bestDistance) {
            bestHandle = handle;
            bestDistance = distance;
        }
    };

    if (sourceY >= top - handleTolerance && sourceY <= bottom + handleTolerance) {
        considerHandle(ExportBoundaryHandle::Left, qAbs(sourceX - left));
        considerHandle(ExportBoundaryHandle::Right, qAbs(sourceX - right));
    }
    if (sourceX >= left - handleTolerance && sourceX <= right + handleTolerance) {
        considerHandle(ExportBoundaryHandle::Top, qAbs(sourceY - top));
    }

    return bestHandle;
}

void MainWindow::updateExportBoundaryHoverCursor(const QPoint &viewerPoint)
{
    if (!ui || !ui->imageViewerLabel) {
        return;
    }
    ExportBoundaryHandle hoverHandle = ExportBoundaryHandle::None;
    if (exportBoundaryDragHandle != ExportBoundaryHandle::None) {
        hoverHandle = exportBoundaryDragHandle;
    } else {
        hoverHandle = exportBoundaryHandleAtViewerPoint(viewerPoint);
        if (hoverHandle == ExportBoundaryHandle::None
            && exportBoundarySelectedHandle != ExportBoundaryHandle::None
            && isExportBoundaryDragAvailable()) {
            hoverHandle = exportBoundarySelectedHandle;
        }
    }

    Qt::CursorShape cursorShape = Qt::ArrowCursor;
    if (hoverHandle == ExportBoundaryHandle::Top) {
        cursorShape = Qt::SizeVerCursor;
    } else if (hoverHandle == ExportBoundaryHandle::Left
               || hoverHandle == ExportBoundaryHandle::Right) {
        cursorShape = Qt::SizeHorCursor;
    }

    if (ui->imageViewerLabel->cursor().shape() != cursorShape) {
        ui->imageViewerLabel->setCursor(cursorShape);
    }
    if (ui->scrollArea && ui->scrollArea->viewport()
        && ui->scrollArea->viewport()->cursor().shape() != cursorShape) {
        ui->scrollArea->viewport()->setCursor(cursorShape);
    }
}

void MainWindow::applyExportBoundaryDragAtViewerPoint(const QPoint &viewerPoint)
{
    if (exportBoundaryDragHandle == ExportBoundaryHandle::None || !isExportBoundaryDragAvailable()) {
        return;
    }

    qint32 sourceX = 0;
    qint32 sourceY = 0;
    if (!mapViewerToSourceCoordinates(viewerPoint, sourceX, sourceY)) {
        return;
    }

    TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    bool changed = false;

    switch (exportBoundaryDragHandle) {
    case ExportBoundaryHandle::Top: {
        const qint32 minimumLine = minActiveFrameLineForSystem(videoParameters.system);
        const qint32 maximumLine = qMax(minimumLine, videoParameters.lastActiveFrameLine);
        const qint32 targetFirstActiveFrameLine = qBound(minimumLine, sourceY, maximumLine);
        if (videoParameters.firstActiveFrameLine != targetFirstActiveFrameLine) {
            videoParameters.firstActiveFrameLine = targetFirstActiveFrameLine;
            changed = true;
        }
        break;
    }
    case ExportBoundaryHandle::Left: {
        const qint32 minimumStart = qBound<qint32>(
            0,
            videoParameters.colourBurstEnd,
            qMax<qint32>(0, videoParameters.fieldWidth - 1));
        const qint32 maximumStart = qMax(minimumStart, videoParameters.activeVideoEnd - 1);
        const qint32 targetActiveVideoStart = qBound(minimumStart, sourceX, maximumStart);
        if (videoParameters.activeVideoStart != targetActiveVideoStart) {
            videoParameters.activeVideoStart = targetActiveVideoStart;
            changed = true;
        }
        break;
    }
    case ExportBoundaryHandle::Right: {
        const qint32 minimumEnd = videoParameters.activeVideoStart + 1;
        const qint32 maximumEnd = qMax(minimumEnd, videoParameters.fieldWidth);
        const qint32 targetActiveVideoEnd = qBound(minimumEnd, sourceX + 1, maximumEnd);
        if (videoParameters.activeVideoEnd != targetActiveVideoEnd) {
            videoParameters.activeVideoEnd = targetActiveVideoEnd;
            changed = true;
        }
        break;
    }
    case ExportBoundaryHandle::None:
    default:
        break;
    }

    if (!changed) {
        return;
    }

    if (videoParametersDialog) {
        videoParametersDialog->setVideoParameters(videoParameters);
    }
    videoParametersChangedSignalHandler(videoParameters);
}

void MainWindow::applyExportBoundaryWheelStep(qint32 step)
{
    if (step == 0 || exportBoundarySelectedHandle == ExportBoundaryHandle::None) {
        return;
    }
    if (!isExportBoundaryDragAvailable()) {
        exportBoundarySelectedHandle = ExportBoundaryHandle::None;
        return;
    }

    TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    bool changed = false;
    switch (exportBoundarySelectedHandle) {
    case ExportBoundaryHandle::Top: {
        const qint32 minimumLine = minActiveFrameLineForSystem(videoParameters.system);
        const qint32 maximumLine = qMax(minimumLine, videoParameters.lastActiveFrameLine);
        const qint32 targetFirstActiveFrameLine = qBound(minimumLine,
                                                         videoParameters.firstActiveFrameLine + step,
                                                         maximumLine);
        if (videoParameters.firstActiveFrameLine != targetFirstActiveFrameLine) {
            videoParameters.firstActiveFrameLine = targetFirstActiveFrameLine;
            changed = true;
        }
        break;
    }
    case ExportBoundaryHandle::Left: {
        const qint32 minimumStart = qBound<qint32>(
            0,
            videoParameters.colourBurstEnd,
            qMax<qint32>(0, videoParameters.fieldWidth - 1));
        const qint32 maximumStart = qMax(minimumStart, videoParameters.activeVideoEnd - 1);
        const qint32 targetActiveVideoStart = qBound(minimumStart,
                                                     videoParameters.activeVideoStart + step,
                                                     maximumStart);
        if (videoParameters.activeVideoStart != targetActiveVideoStart) {
            videoParameters.activeVideoStart = targetActiveVideoStart;
            changed = true;
        }
        break;
    }
    case ExportBoundaryHandle::Right: {
        const qint32 minimumEnd = videoParameters.activeVideoStart + 1;
        const qint32 maximumEnd = qMax(minimumEnd, videoParameters.fieldWidth);
        const qint32 targetActiveVideoEnd = qBound(minimumEnd,
                                                   videoParameters.activeVideoEnd + step,
                                                   maximumEnd);
        if (videoParameters.activeVideoEnd != targetActiveVideoEnd) {
            videoParameters.activeVideoEnd = targetActiveVideoEnd;
            changed = true;
        }
        break;
    }
    case ExportBoundaryHandle::None:
    default:
        break;
    }

    if (!changed) {
        return;
    }
    if (videoParametersDialog) {
        videoParametersDialog->setVideoParameters(videoParameters);
    }
    videoParametersChangedSignalHandler(videoParameters);
}
// Method to hide the current image
void MainWindow::hideImage()
{
    ui->imageViewerLabel->clear();
    vectorscopeSelectionDragging = false;
    exportBoundaryDragHandle = ExportBoundaryHandle::None;
    exportBoundarySelectedHandle = ExportBoundaryHandle::None;
    updateExportBoundaryHoverCursor(QPoint(-1, -1));
    asyncFrameImage = QImage();
    asyncFrameRenderQueued = false;
    cursorReadoutImage = QImage();
    clearCursorReadout();
}

// Misc private methods -----------------------------------------------------------------------------------------------

// Load a TBC file based on the passed file name
void MainWindow::loadTbcFile(QString inputFileName, bool forceMetadataOnly, bool preserveStatusDuringReload)
{
    // The current metadata is being replaced; callers have already offered to
    // save any edits (maybeSave), or just saved them.
    setWindowModified(false);
    setPlaybackRunning(false);
    if (asyncFrameRenderInProgress) {
        cancelInFlightAsyncFrameRender();
    }
    asyncFrameRenderQueued = false;
    asyncFrameImage = QImage();
    if (preserveStatusDuringReload) {
        setGuiEnabled(false);
        sourceVideoStatus.setText(tr("Reloading metadata and analysis..."));
        fieldNumberStatus.setText(tr(" Frames: .../... Fields: .../..."));
        vbiStatus.hide();
        timeCodeStatus.hide();
    } else {
        // Update the GUI
        updateGuiUnloaded();
    }

    // Close current source video (if loaded). A slideshow scan belongs to it.
    if (slideshowDialog) slideshowDialog->close();
    if (tbcSource.getIsSourceLoaded()) {
        tbcSource.unloadSource();
    }

    cleanupTempMetadataFile();
    metadataJsonLoaded = false;
    metadataJsonFilename.clear();
    metadataTempSqliteFilename.clear();

    QString resolvedInput = inputFileName;
    QFileInfo inputInfo(resolvedInput);
    QString suffix = inputInfo.suffix().toLower();
    bool isJson = (suffix == "json");
    bool isSqlite = (suffix == "db");
    const bool isMetadataInput = isJson || isSqlite;
    bool isMetadataOnly = forceMetadataOnly;

    if (forceMetadataOnly && !isJson && !isSqlite) {
        QString dbCandidate = resolvedInput;
        if (!dbCandidate.endsWith(".db", Qt::CaseInsensitive)) {
            dbCandidate += ".db";
        }
        QString jsonCandidate = resolvedInput;
        if (!jsonCandidate.endsWith(".json", Qt::CaseInsensitive)) {
            jsonCandidate += ".json";
        }

        QString metadataCandidate;
        if (QFileInfo::exists(jsonCandidate)) {
            metadataCandidate = jsonCandidate;
        } else if (QFileInfo::exists(dbCandidate)) {
            metadataCandidate = dbCandidate;
        }

        if (metadataCandidate.isEmpty()) {
            QMessageBox::warning(this, tr("Error"),
                                 tr("Metadata-only mode requires a .db or .json file. '%1' and '%2' were not found.")
                                 .arg(dbCandidate, jsonCandidate));
            return;
        }

        resolvedInput = metadataCandidate;
        suffix = QFileInfo(resolvedInput).suffix().toLower();
        isJson = (suffix == "json");
        isSqlite = (suffix == "db");
        isMetadataOnly = true;
    }

    if (!forceMetadataOnly && (isJson || isSqlite)) {
        const QString resolvedSourceFilename = resolveSourceFilenameForMetadata(resolvedInput);
        if (!resolvedSourceFilename.isEmpty()) {
            if (isJson) {
                metadataJsonLoaded = true;
                metadataJsonFilename = resolvedInput;
            }

            // Keep reload behaviour aligned with the file the user selected.
            lastFilename = resolvedInput;
            sourceOperationInProgress = true;
            tbcSource.loadSource(resolvedSourceFilename, resolvedInput);
            return;
        }

        isMetadataOnly = true;
    } else if (isMetadataInput) {
        isMetadataOnly = true;
    }

    if (isMetadataOnly) {
        QString metadataDisplayName = resolvedInput;
        if (isJson) {
            metadataJsonLoaded = true;
            metadataJsonFilename = resolvedInput;
        }

        lastFilename = metadataDisplayName;
        sourceOperationInProgress = true;
        tbcSource.loadMetadata(resolvedInput, metadataDisplayName);
        return;
    }

    lastFilename = inputFileName;
    sourceOperationInProgress = true;
    tbcSource.loadSource(inputFileName);

    // Note: loading continues in the background...
}

void MainWindow::cleanupTempMetadataFile()
{
    if (metadataTempSqliteFilename.isEmpty()) {
        return;
    }

    QFile::remove(metadataTempSqliteFilename);
    metadataTempSqliteFilename.clear();
}

// Method to update the line oscilloscope based on the frame number and scan line
void MainWindow::updateOscilloscopeDialogue()
{
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }
    // Update the oscilloscope dialogue
    oscilloscopeDialog->showTraceImage(tbcSource.getScanLineData(lastScopeLine),
                                       lastScopeDot, lastScopeLine - 1,
                                       tbcSource.getFrameWidth(), tbcSource.getFrameHeight(), tbcSource.getSourceMode() == TbcSource::SourceMode::BOTH_SOURCES);
}

void MainWindow::updateRgbScopeDialogue(bool force)
{
    if (!rgbScopeDialog || !tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 minRefreshIntervalMs = playbackRunning ? 180 : 80;
    if (!force && rgbScopeLastRefreshMs > 0) {
        const qint64 elapsedMs = nowMs - rgbScopeLastRefreshMs;
        if (elapsedMs < minRefreshIntervalMs) {
            if (rgbScopeRefreshTimer && !rgbScopeRefreshPending) {
                rgbScopeRefreshPending = true;
                rgbScopeRefreshTimer->start(static_cast<int>(minRefreshIntervalMs - elapsedMs));
            }
            return;
        }
    }

    rgbScopeDialog->showScopeImage(tbcSource.getRgbScopeImage(rgbScopeDialog->scopeRenderTargetSize()));
    rgbScopeLastRefreshMs = nowMs;
    if (rgbScopeRefreshTimer && rgbScopeRefreshPending) {
        rgbScopeRefreshTimer->stop();
        rgbScopeRefreshPending = false;
    }
}

void MainWindow::updateYuvRangeScopeDialogue(bool force)
{
    if (!yuvRangeDialog || !tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 minRefreshIntervalMs = playbackRunning ? 180 : 80;
    if (!force && yuvRangeScopeLastRefreshMs > 0) {
        const qint64 elapsedMs = nowMs - yuvRangeScopeLastRefreshMs;
        if (elapsedMs < minRefreshIntervalMs) {
            if (yuvRangeScopeRefreshTimer && !yuvRangeScopeRefreshPending) {
                yuvRangeScopeRefreshPending = true;
                yuvRangeScopeRefreshTimer->start(static_cast<int>(minRefreshIntervalMs - elapsedMs));
            }
            return;
        }
    }

    yuvRangeDialog->showScopeImage(tbcSource.getYuvRangeScopeImage(yuvRangeDialog->scopeRenderTargetSize(), yuvRangeSettings));
    yuvRangeScopeLastRefreshMs = nowMs;
    if (yuvRangeScopeRefreshTimer && yuvRangeScopeRefreshPending) {
        yuvRangeScopeRefreshTimer->stop();
        yuvRangeScopeRefreshPending = false;
    }
}

// Method to update the vectorscope
void MainWindow::updateVectorscopeDialogue()
{
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }
    // Update the vectorscope dialogue
    vectorscopeDialog->showTraceImage(tbcSource.getComponentFrame(), tbcSource.getVideoParameters(),
                                      tbcSource.getViewMode(), currentFieldNumber % 2);
}

// Method to update the waveform monitor — feeds the per-field 16-bit TBC
// samples through the dialog, which converts them to the 10-bit CVBS domain.
void MainWindow::updateWaveformMonitorDialogue()
{
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }
    const TbcSource::FieldTimingData timingData = tbcSource.getFieldTimingData();
    if (!timingData.valid) {
        return;
    }
    // Interleave the two fields into a single flat composite/luma/chroma
    // buffer per channel (field 1 then field 2), matching the waveform
    // monitor's expected line-contiguous layout.
    auto interleave = [](const std::vector<uint16_t>& first,
                         const std::vector<uint16_t>& second) -> std::vector<uint16_t> {
        std::vector<uint16_t> out;
        out.reserve(first.size() + second.size());
        out.insert(out.end(), first.begin(), first.end());
        out.insert(out.end(), second.begin(), second.end());
        return out;
    };

    const std::vector<uint16_t> composite =
        interleave(timingData.firstFieldComposite, timingData.secondFieldComposite);
    const std::vector<uint16_t> luma =
        interleave(timingData.firstFieldLuma, timingData.secondFieldLuma);
    const std::vector<uint16_t> chroma =
        interleave(timingData.firstFieldChroma, timingData.secondFieldChroma);

    waveformMonitorDialog->setData(composite, luma, chroma,
                                   timingData.firstFieldHeight,
                                   timingData.secondFieldHeight,
                                   tbcSource.getVideoParameters());
}

// Method to update the field timing scope
void MainWindow::updateFieldTimingDialogue()
{
    if (asyncFrameRenderInProgress && shouldRenderFrameAsync()) {
        return;
    }
    const TbcSource::FieldTimingData timingData = tbcSource.getFieldTimingData();
    if (!timingData.valid) {
        return;
    }

    const std::optional<qint32> secondFieldNumber = timingData.hasSecondField
        ? std::optional<qint32>(timingData.secondFieldNumber)
        : std::nullopt;
    const std::optional<TbcMetaData::VideoParameters> videoParameters = tbcSource.getVideoParameters();

    fieldTimingDialog->setFieldData(
        tbcSource.getCurrentSourceFilename(),
        timingData.firstFieldNumber,
        timingData.firstFieldComposite,
        secondFieldNumber,
        timingData.secondFieldComposite,
        timingData.firstFieldLuma,
        timingData.firstFieldChroma,
        timingData.secondFieldLuma,
        timingData.secondFieldChroma,
        videoParameters,
        timingData.firstFieldHeight,
        timingData.secondFieldHeight);
}

// Method to set the view (field/frame) values
double MainWindow::timecodeFrameRate() const
{
    if (!tbcSource.getIsSourceLoaded()) {
        return 30000.0 / 1001.0;
    }
    return frameRateForSystem(tbcSource.getSystem());
}

int MainWindow::timecodeFrameBaseRate() const
{
    if (!tbcSource.getIsSourceLoaded()) {
        return 30;
    }
    return nominalFrameRateForSystem(tbcSource.getSystem());
}

QString MainWindow::frameToTimecode(qint32 frameNumber) const
{
    const double fps = qMax(0.001, timecodeFrameRate());
    const int frameBase = qMax(1, timecodeFrameBaseRate());
    const qint64 frameIndex = qMax<qint64>(0, static_cast<qint64>(frameNumber) - 1);
    const double totalSecondsExact = static_cast<double>(frameIndex) / fps;
    const qint64 totalSeconds = static_cast<qint64>(qFloor(totalSecondsExact));
    const double fractionalSeconds = totalSecondsExact - static_cast<double>(totalSeconds);
    const int framePart = qBound(0, static_cast<int>(qFloor((fractionalSeconds * frameBase) + 1e-9)), frameBase - 1);
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(framePart, 2, 10, QChar('0'));
}

QString MainWindow::framesToDurationTimecode(qint32 frameCount) const
{
    const double fps = qMax(0.001, timecodeFrameRate());
    const int frameBase = qMax(1, timecodeFrameBaseRate());
    const qint64 safeFrameCount = qMax<qint64>(0, frameCount);
    const double totalSecondsExact = static_cast<double>(safeFrameCount) / fps;
    const qint64 totalSeconds = static_cast<qint64>(qFloor(totalSecondsExact));
    const double fractionalSeconds = totalSecondsExact - static_cast<double>(totalSeconds);
    const int framePart = qBound(0, static_cast<int>(qFloor((fractionalSeconds * frameBase) + 1e-9)), frameBase - 1);
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(framePart, 2, 10, QChar('0'));
}

qint32 MainWindow::timecodeToFrame(const QString &timecodeText, bool *ok) const
{
    if (ok) {
        *ok = false;
    }

    const QString trimmed = timecodeText.trimmed();
    if (trimmed.isEmpty()) {
        return 1;
    }

    bool numericOk = false;
    const qint32 numericFrame = trimmed.toInt(&numericOk);
    if (numericOk) {
        if (ok) {
            *ok = true;
        }
        return qMax(1, numericFrame);
    }

    static const QRegularExpression pattern(
        QStringLiteral("^\\s*(\\d+):(\\d{1,2}):(\\d{1,2}):(\\d{1,2})\\s*$"));
    const QRegularExpressionMatch match = pattern.match(trimmed);
    if (!match.hasMatch()) {
        return 1;
    }

    bool hoursOk = false;
    bool minutesOk = false;
    bool secondsOk = false;
    bool framesOk = false;
    const qint64 hours = match.captured(1).toLongLong(&hoursOk);
    const int minutes = match.captured(2).toInt(&minutesOk);
    const int seconds = match.captured(3).toInt(&secondsOk);
    const int frames = match.captured(4).toInt(&framesOk);
    const double fps = qMax(0.001, timecodeFrameRate());
    const int frameBase = qMax(1, timecodeFrameBaseRate());
    if (!hoursOk || !minutesOk || !secondsOk || !framesOk
        || minutes < 0 || minutes >= 60
        || seconds < 0 || seconds >= 60
        || frames < 0 || frames >= frameBase) {
        return 1;
    }
    const double totalSecondsExact = static_cast<double>((hours * 3600) + (minutes * 60) + seconds)
                                     + (static_cast<double>(frames) / static_cast<double>(frameBase));
    const qint64 totalFrames = qRound64(totalSecondsExact * fps) + 1;
    if (ok) {
        *ok = true;
    }
    return static_cast<qint32>(qMax<qint64>(1, totalFrames));
}

void MainWindow::updatePositionEditorValue(qint32 currentNumber)
{
    if (ui->posNumberSpinBox) {
        const QSignalBlocker blocker(ui->posNumberSpinBox);
        ui->posNumberSpinBox->setValue(currentNumber);
    }
    if (ui->posTimecodeLineEdit && !tbcSource.getFieldViewEnabled()) {
        const QSignalBlocker blocker(ui->posTimecodeLineEdit);
        ui->posTimecodeLineEdit->setText(frameToTimecode(currentNumber));
    }
}

bool MainWindow::playbackUseFastMode() const
{
    if (!tbcSource.getIsSourceLoaded()) {
        return false;
    }

    if (!tbcSource.getChromaDecoder()) {
        return true;
    }

    return tbcSource.getSourceMode() == TbcSource::LUMA_SOURCE;
}

QString MainWindow::playbackStartToolTip() const
{
    const bool isPalSystem = (tbcSource.getIsSourceLoaded() && tbcSource.getSystem() == PAL);
    if (playbackUseFastMode()) {
        const QString fastFpsText = isPalSystem ? QStringLiteral("50.00") : QStringLiteral("59.94");
        return tr("Play at %1 fps (luma/source mode)").arg(fastFpsText);
    }

    const QString fpsText = isPalSystem ? QStringLiteral("25.00") : QStringLiteral("29.97");
    return tr("Play at %1 fps").arg(fpsText);
}

double MainWindow::playbackFrameIntervalMs() const
{
    if (tbcSource.getIsSourceLoaded() && tbcSource.getSystem() == PAL) {
        return 1000.0 / 25.0;
    }
    return 1000.0 * 1001.0 / 30000.0;
}

void MainWindow::scheduleNextPlaybackTick()
{
    if (!playbackRunning || !playbackTimer) {
        return;
    }
    double intervalMs = playbackFrameIntervalMs();
    if (playbackUseFastMode()) {
        intervalMs *= 0.5;
    }
    playbackTickCarryMs += intervalMs;
    const int timerIntervalMs = qMax(1, static_cast<int>(qFloor(playbackTickCarryMs)));
    playbackTickCarryMs -= static_cast<double>(timerIntervalMs);
    playbackTimer->start(timerIntervalMs);
}

void MainWindow::setPlaybackRunning(bool running)
{
    playbackRunning = running && tbcSource.getIsSourceLoaded();
    if (playbackTimer && !playbackRunning) {
        playbackTimer->stop();
    }

    if (!ui || !ui->playPushButton) {
        return;
    }

    {
        const QSignalBlocker blocker(ui->playPushButton);
        ui->playPushButton->setChecked(playbackRunning);
    }

    if (playbackRunning) {
        playbackTickCarryMs = 0.0;
        ui->playPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/stop-playback.svg")));
        ui->playPushButton->setToolTip(tr("Stop playback"));
        scheduleNextPlaybackTick();
        return;
    }

    ui->playPushButton->setIcon(QIcon(QStringLiteral(":/icons/Graphics/start-playback.svg")));
    ui->playPushButton->setToolTip(playbackStartToolTip());
}

void MainWindow::stepPlayback()
{
    if (!playbackRunning || !tbcSource.getIsSourceLoaded()) {
        setPlaybackRunning(false);
        return;
    }

    qint32 currentNumber = currentFrameNumber;
    if (tbcSource.getFieldViewEnabled()) {
        const qint32 nextField = currentFieldNumber + 2;
        if (nextField > tbcSource.getNumberOfFields()) {
            setPlaybackRunning(false);
            return;
        }
        setCurrentField(nextField);
        currentNumber = currentFieldNumber;
    } else {
        const qint32 nextFrame = currentFrameNumber + 1;
        if (nextFrame > tbcSource.getNumberOfFrames()) {
            setPlaybackRunning(false);
            return;
        }
        setCurrentFrame(nextFrame);
        currentNumber = currentFrameNumber;
    }

    updatePositionEditorValue(currentNumber);
    if (ui->posHorizontalSlider) {
        const QSignalBlocker blocker(ui->posHorizontalSlider);
        ui->posHorizontalSlider->setValue(currentNumber);
    }

    scheduleNextPlaybackTick();
}

void MainWindow::updateBottomStatusReadout()
{
    if (!tbcSource.getIsSourceLoaded()) {
        sourceVideoStatus.setText(tr("No source video file loaded"));
        fieldNumberStatus.setText(tr(" Frames: ./. Fields: ./."));
        return;
    }

    QString sourcePrefix;
    if (!tbcSource.getVideoParameters().tapeFormat.isEmpty()) {
        sourcePrefix += tbcSource.getVideoParameters().tapeFormat + QLatin1Char(' ');
    }
    sourcePrefix += tbcSource.getSystemDescription();
    sourcePrefix += tbcSource.getIsMetadataOnly()
                        ? tr(" Metadata Duration: ")
                        : tr(" Source Duration: ");
    sourcePrefix += framesToDurationTimecode(tbcSource.getNumberOfFrames());
    sourceVideoStatus.setText(sourcePrefix);

    const qint32 totalFrames = qMax(1, tbcSource.getNumberOfFrames());
    const qint32 totalFields = qMax(1, tbcSource.getNumberOfFields());
    const qint32 frameCurrent = qBound(1, currentFrameNumber, totalFrames);
    const qint32 firstField = qBound(1, tbcSource.getFirstFieldNumber(), totalFields);
    const qint32 secondField = qBound(1, tbcSource.getSecondFieldNumber(), totalFields);
    QString readout = QStringLiteral(" Frames: %1/%2 Fields: %3/%4")
                          .arg(frameCurrent)
                          .arg(totalFrames)
                          .arg(firstField)
                          .arg(secondField);
    const QVector<TbcMetaData::Segment> &segments = tbcSource.getSegments();
    if (!segments.isEmpty()) {
        const qint32 index = segmentIndexContainingField(qMax<qint32>(0, firstField - 1));
        readout += index >= 0
                       ? QStringLiteral(" Seg: %1/%2").arg(index + 1).arg(segments.size())
                       : QStringLiteral(" Seg: -/%1").arg(segments.size());
    }
    fieldNumberStatus.setText(readout);
    if (segmentsViewerDialog && segmentsViewerDialog->isVisible()) {
        updateSegmentsViewerState();
    }
}
void MainWindow::setViewValues()
{
    qint32 currentNumber, maximum;
    QString buttonLabel, spinLabel;

    if (tbcSource.getFieldViewEnabled()) {
        currentNumber = currentFieldNumber;
        maximum = tbcSource.getNumberOfFields();
        spinLabel = tr("Field #:");
        buttonLabel = tbcSource.getStretchField() ? tr("Field 2:1") : tr("Field 1:1");
    } else {
        currentNumber = currentFrameNumber;
        maximum = tbcSource.getNumberOfFrames();
        spinLabel = tr("Frame #:");
        buttonLabel = tbcSource.getSplitViewEnabled() ? tr("Split") : tr("Frame");
    }

    ui->posNumberSpinBox->setMaximum(maximum);
    updatePositionEditorValue(currentNumber);
    ui->posHorizontalSlider->setMaximum(maximum);
    ui->posHorizontalSlider->setPageStep(maximum / 100);
    ui->posHorizontalSlider->setValue(currentNumber);

    if (ui->posTimecodeLineEdit) {
        const bool fieldView = tbcSource.getFieldViewEnabled();
        ui->posNumberSpinBox->setVisible(fieldView);
        ui->posTimecodeLineEdit->setVisible(!fieldView);
        if (!fieldView) {
            ui->posTimecodeLineEdit->setText(frameToTimecode(currentNumber));
        }
    }

    ui->viewPushButton->setText(buttonLabel);
    if (ui->posNumberSpinBoxLabel) {
        const bool fieldView = tbcSource.getFieldViewEnabled();
        ui->posNumberSpinBoxLabel->setVisible(fieldView);
        if (fieldView) {
            ui->posNumberSpinBoxLabel->setText(spinLabel);
        }
    }

    updateTimelineMarkers();
}
qint32 MainWindow::sliderPositionForFrame(qint32 frameNumber) const
{
    if (frameNumber <= 0 || !tbcSource.getIsSourceLoaded()) {
        return -1;
    }

    if (tbcSource.getFieldViewEnabled()) {
        const qint32 totalFields = qMax<qint32>(1, tbcSource.getNumberOfFields());
        const qint32 fieldPosition = (frameNumber * 2) - 1;
        return qBound<qint32>(1, fieldPosition, totalFields);
    }

    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    return qBound<qint32>(1, frameNumber, totalFrames);
}

qint32 MainWindow::frameForSliderPosition(qint32 sliderPosition) const
{
    if (!tbcSource.getIsSourceLoaded()) {
        return 1;
    }
    if (tbcSource.getFieldViewEnabled()) {
        const qint32 totalFields = qMax<qint32>(1, tbcSource.getNumberOfFields());
        const qint32 clamped = qBound<qint32>(1, sliderPosition, totalFields);
        return qBound<qint32>(1, (clamped + 1) / 2, qMax<qint32>(1, tbcSource.getNumberOfFrames()));
    }
    return qBound<qint32>(1, sliderPosition, qMax<qint32>(1, tbcSource.getNumberOfFrames()));
}

void MainWindow::updateTimelineMarkers()
{
    if (!tbcSource.getIsSourceLoaded()) {
        ui->posHorizontalSlider->setMarkerFrames(-1, -1, {});
        return;
    }
    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());

    const TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    const qint32 inPosition = sliderPositionForFrame(videoParameters.userEditInSelection);
    const qint32 outPosition = sliderPositionForFrame(videoParameters.userEditOutSelection);
    const QVector<UserNoteMarker> noteMarkers = userNoteMarkersFromVideoParameters(videoParameters, totalFrames);
    QVector<qint32> notePositions;
    notePositions.reserve(noteMarkers.size());
    for (const UserNoteMarker &noteMarker : noteMarkers) {
        const qint32 markerPosition = sliderPositionForFrame(noteMarker.frame);
        if (markerPosition > 0) {
            notePositions.append(markerPosition);
        }
    }
    ui->posHorizontalSlider->setMarkerFrames(inPosition, outPosition, notePositions);

    // Recording segments: a tick per boundary (with a tooltip), a tint over
    // noise, blank and disabled spans
    QVector<qint32> boundaryPositions;
    QStringList boundaryTooltips;
    QVector<TimelineSegmentSpan> spans;
    const QVector<TbcMetaData::Segment> &segments = tbcSource.getSegments();
    for (qint32 i = 0; i < segments.size(); ++i) {
        const TbcMetaData::Segment &segment = segments.at(i);
        if (segment.startField > 0) {
            const qint32 position = sliderPositionForField(segment.startField);
            if (position > 0) {
                boundaryPositions.append(position);
                boundaryTooltips.append(segmentSummaryText(segment));
            }
        }
        QColor tint;
        if (segment.kind == QLatin1String("noise")) {
            tint = QColor(255, 170, 0, 50);
        } else if (segment.kind == QLatin1String("blank")) {
            tint = QColor(128, 128, 128, 60);
        } else if (!segment.enabled) {
            tint = QColor(220, 45, 45, 40);
        }
        if (tint.isValid()) {
            TimelineSegmentSpan span;
            span.startPosition = sliderPositionForField(segment.startField);
            span.endPosition = sliderPositionForField(qMax(segment.startField, segment.endFieldExclusive - 1));
            span.color = tint;
            if (span.startPosition > 0 && span.endPosition >= span.startPosition) {
                spans.append(span);
            }
        }
    }
    ui->posHorizontalSlider->setSegmentMarkers(boundaryPositions, boundaryTooltips, spans);
}

qint32 MainWindow::sliderPositionForField(qint32 field) const
{
    if (field < 0 || !tbcSource.getIsSourceLoaded()) {
        return -1;
    }
    if (tbcSource.getFieldViewEnabled()) {
        const qint32 totalFields = qMax<qint32>(1, tbcSource.getNumberOfFields());
        return qBound<qint32>(1, field + 1, totalFields);
    }
    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    qint32 frame = tbcSource.frameContainingField(field);
    if (frame < 1) {
        frame = (field / 2) + 1;
    }
    return qBound<qint32>(1, frame, totalFrames);
}

qint32 MainWindow::currentFirstFieldZeroBased() const
{
    if (!tbcSource.getIsSourceLoaded()) {
        return -1;
    }
    if (tbcSource.getFieldViewEnabled()) {
        return qMax<qint32>(0, currentFieldNumber - 1);
    }
    const qint32 first = tbcSource.firstFieldOfFrame(currentFrameNumber);
    return first >= 0 ? first : qMax<qint32>(0, (currentFrameNumber - 1) * 2);
}

qint32 MainWindow::segmentIndexContainingField(qint32 field) const
{
    const QVector<TbcMetaData::Segment> &segments = tbcSource.getSegments();
    for (qint32 i = 0; i < segments.size(); ++i) {
        if (field >= segments.at(i).startField && field < segments.at(i).endFieldExclusive) {
            return i;
        }
    }
    return -1;
}

qint32 MainWindow::segmentStartFrame(const TbcMetaData::Segment &segment) const
{
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
    if (tbcSource.segmentFrameRange(segment, &startFrame, &lengthFrames)) {
        return startFrame;
    }
    const qint32 frame = tbcSource.frameContainingField(segment.startField);
    return frame >= 1 ? frame : -1;
}

bool MainWindow::skipBySegmentsEnabled() const
{
    return skipBySegmentsAction && skipBySegmentsAction->isChecked() && !tbcSource.getSegments().isEmpty();
}

QString MainWindow::segmentSummaryText(const TbcMetaData::Segment &segment) const
{
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
    QString range;
    if (tbcSource.segmentFrameRange(segment, &startFrame, &lengthFrames)) {
        range = QStringLiteral("%1–%2").arg(frameToTimecode(startFrame)).arg(frameToTimecode(startFrame + lengthFrames - 1));
    } else {
        range = tr("no whole frame");
    }
    const QString title = segment.title.trimmed().isEmpty() ? segment.kind : segment.title.trimmed();
    return tr("Segment %1: %2 (%3)%4")
        .arg(segment.id)
        .arg(title)
        .arg(range)
        .arg(segment.enabled ? QString() : tr(" [disabled]"));
}

// The notesUpdated sequence, for segment edits: metadata, Save, timeline, panels
void MainWindow::applySegmentEdit(const QVector<TbcMetaData::Segment> &segments, const QString &statusText)
{
    tbcSource.setSegments(segments);
    setWindowModified(true);
    updateMetadataStatusPanel();
    updateTimelineMarkers();
    updateBottomStatusReadout();
    if (exportDialog) {
        exportDialog->refreshSegmentsFromSource();
    }
    if (!statusText.isEmpty()) {
        statusBar()->showMessage(statusText, 3000);
    }
}

void MainWindow::setInOutFromSegment(qint32 segmentIndex, bool setIn, bool setOut)
{
    if (!exportDialog || !tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }
    const QVector<TbcMetaData::Segment> &segments = tbcSource.getSegments();
    if (segmentIndex < 0 || segmentIndex >= segments.size()) {
        return;
    }
    qint32 startFrame = 0;
    qint32 lengthFrames = 0;
    if (!tbcSource.segmentFrameRange(segments.at(segmentIndex), &startFrame, &lengthFrames)) {
        statusBar()->showMessage(tr("Segment %1 holds no whole frame").arg(segments.at(segmentIndex).id), 3000);
        return;
    }
    if (setIn) {
        exportDialog->setInPoint(startFrame);
    }
    if (setOut) {
        exportDialog->setOutPoint(startFrame + lengthFrames - 1);
    }
    statusBar()->showMessage(tr("Export %1 set from segment %2 (frames %3–%4)")
                                 .arg(setIn && setOut ? tr("In/Out") : setIn ? tr("In") : tr("Out"))
                                 .arg(segments.at(segmentIndex).id)
                                 .arg(startFrame)
                                 .arg(startFrame + lengthFrames - 1), 3000);
}

// Split the segment containing `field` at that field: the head keeps its id,
// the tail becomes a new user segment. Both are marked user-edited.
bool MainWindow::splitSegmentAtField(qint32 field, QString *statusText)
{
    QVector<TbcMetaData::Segment> segments = tbcSource.getSegments();
    const qint32 index = segmentIndexContainingField(field);
    if (index < 0 || field <= segments.at(index).startField) {
        if (statusText) {
            *statusText = tr("No segment to split at field %1").arg(field);
        }
        return false;
    }
    qint32 maxId = 0;
    for (const TbcMetaData::Segment &segment : segments) {
        maxId = qMax(maxId, segment.id);
    }
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    TbcMetaData::Segment head = segments.at(index);
    TbcMetaData::Segment tail = head;
    head.endFieldExclusive = field;
    head.source = QStringLiteral("user");
    head.updatedAt = now;
    tail.id = maxId + 1;
    tail.startField = field;
    tail.title.clear();
    tail.comment.clear();
    tail.source = QStringLiteral("user");
    tail.createdBy = QStringLiteral("tbc-analyse");
    tail.updatedAt = now;
    tail.derivedFrom.clear();
    segments[index] = head;
    segments.insert(index + 1, tail);
    applySegmentEdit(segments, QString());
    if (statusText) {
        *statusText = tr("Segment %1 split at field %2; segment %3 created").arg(head.id).arg(field).arg(tail.id);
    }
    return true;
}

void MainWindow::updateSegmentsViewerState()
{
    if (!segmentsViewerDialog) {
        return;
    }
    SegmentsViewerState state;
    state.frameRate = timecodeFrameRate();
    state.frameBaseRate = timecodeFrameBaseRate();
    if (tbcSource.getIsSourceLoaded()) {
        state.totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
        state.totalFields = qMax<qint32>(1, tbcSource.getNumberOfFields());
        state.currentFrame = qBound<qint32>(1, currentFrameNumber, state.totalFrames);
        state.currentFirstField = currentFirstFieldZeroBased();
        state.metadataOnly = tbcSource.getIsMetadataOnly();
        state.derivedAtLoad = tbcSource.getSegmentsDerivedAtLoad();
        state.evidenceAvailable = tbcSource.hasSegmentEvidence();
        const QVector<TbcMetaData::Segment> &segments = tbcSource.getSegments();
        state.rows.reserve(segments.size());
        for (const TbcMetaData::Segment &segment : segments) {
            SegmentsViewerRow row;
            row.segment = segment;
            row.hasFrames = tbcSource.segmentFrameRange(segment, &row.startFrame, &row.lengthFrames);
            state.rows.append(row);
        }
    }
    segmentsViewerDialog->setState(state);
}

void MainWindow::showSegmentsViewer()
{
    if (!segmentsViewerDialog) {
        return;
    }
    updateSegmentsViewerState();
    showOrRaise(segmentsViewerDialog);
}

void MainWindow::goToField(qint32 field)
{
    if (!tbcSource.getIsSourceLoaded() || field < 0) {
        return;
    }
    setPlaybackRunning(false);
    if (tbcSource.getFieldViewEnabled()) {
        const qint32 totalFields = qMax<qint32>(1, tbcSource.getNumberOfFields());
        setCurrentField(qBound<qint32>(1, field + 1, totalFields));
    } else {
        const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
        qint32 frame = tbcSource.frameContainingField(field);
        if (frame < 1) {
            frame = (field / 2) + 1;
        }
        setCurrentFrame(qBound<qint32>(1, frame, totalFrames));
    }
    const qint32 currentNumber = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    updatePositionEditorValue(currentNumber);
    ui->posHorizontalSlider->setValue(currentNumber);
}

// Derive again with the given thresholds. User-edited segments are kept and
// derived segments they overlap are dropped, so a re-derive never undoes an edit.
void MainWindow::rederiveSegments(const SegmentsThresholds &thresholds)
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }
    QVector<TbcMetaData::Segment> kept;
    for (const TbcMetaData::Segment &segment : tbcSource.getSegments()) {
        if (segment.source == QLatin1String("user")) {
            kept.append(segment);
        }
    }
    const QVector<TbcMetaData::Segment> derived = tbcSource.deriveSegments(thresholds);
    qint32 nextId = 0;
    for (const TbcMetaData::Segment &segment : kept) {
        nextId = qMax(nextId, segment.id);
    }
    QVector<TbcMetaData::Segment> merged = kept;
    qint32 dropped = 0;
    for (TbcMetaData::Segment segment : derived) {
        bool overlapsUser = false;
        for (const TbcMetaData::Segment &user : kept) {
            if (segment.startField < user.endFieldExclusive && user.startField < segment.endFieldExclusive) {
                overlapsUser = true;
                break;
            }
        }
        if (overlapsUser) {
            dropped++;
            continue;
        }
        segment.id = ++nextId;
        merged.append(segment);
    }
    std::stable_sort(merged.begin(), merged.end(), [](const TbcMetaData::Segment &a, const TbcMetaData::Segment &b) {
        return a.startField < b.startField;
    });
    applySegmentEdit(merged, tr("Segments re-derived: %1 derived (%2 dropped for user segments), %3 user kept")
                                 .arg(derived.size() - dropped)
                                 .arg(dropped)
                                 .arg(kept.size()));
    updateSegmentsViewerState();
}

void MainWindow::updateNotesViewerState()
{
    if (!notesViewerDialog) {
        return;
    }

    NotesViewerState state;
    state.frameRate = timecodeFrameRate();
    state.frameBaseRate = timecodeFrameBaseRate();

    if (tbcSource.getIsSourceLoaded()) {
        const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
        const TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
        state.totalFrames = totalFrames;
        state.currentFrame = qBound<qint32>(1, currentFrameNumber, totalFrames);
        state.inFrame = videoParameters.userEditInSelection;
        state.outFrame = videoParameters.userEditOutSelection;
        const QVector<UserNoteMarker> noteMarkers = userNoteMarkersFromVideoParameters(videoParameters, totalFrames);
        noteMarkerListsFromMarkers(noteMarkers, state.noteFrames, state.noteComments);
    }

    notesViewerDialog->setState(state);
}

void MainWindow::setInPointAtCurrentFrame()
{
    if (!exportDialog || !tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }

    const qint32 playbackPositionValue = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    const qint32 framePoint = qBound<qint32>(1, frameForSliderPosition(playbackPositionValue), totalFrames);
    const QString framePointTimecode = frameToTimecode(framePoint);

    exportDialog->setInPoint(framePoint);
    statusBar()->showMessage(tr("Export In point set to frame %1 (%2)").arg(framePoint).arg(framePointTimecode), 3000);
}

void MainWindow::setOutPointAtCurrentFrame()
{
    if (!exportDialog || !tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }

    const qint32 playbackPositionValue = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    const qint32 framePoint = qBound<qint32>(1, frameForSliderPosition(playbackPositionValue), totalFrames);
    const QString framePointTimecode = frameToTimecode(framePoint);

    exportDialog->setOutPoint(framePoint);
    statusBar()->showMessage(tr("Export Out point set to frame %1 (%2)").arg(framePoint).arg(framePointTimecode), 3000);
}

// Set the current frame, field is updated based on frame number
void MainWindow::setCurrentFrame(qint32 number)
{
    if (number == currentFrameNumber) return;

    currentFrameNumber = number;
    currentFieldNumber = (number * 2) - 1;

    sanitizeCurrentPosition();
    showImage();
}

// Set the current field, frame is updated based on field number
void MainWindow::setCurrentField(qint32 number)
{
    if (number == currentFieldNumber) return;

    currentFieldNumber = number;
    currentFrameNumber = std::ceil((double)number / 2);

    sanitizeCurrentPosition();
    showImage();
}

void MainWindow::skipFramesBy(qint32 frameDelta)
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }

    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    const qint32 targetFrame = qBound<qint32>(1, currentFrameNumber + frameDelta, totalFrames);
    if (targetFrame == currentFrameNumber) {
        return;
    }

    setCurrentFrame(targetFrame);
    const qint32 uiNumber = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    updatePositionEditorValue(uiNumber);
    ui->posHorizontalSlider->setValue(uiNumber);
}

void MainWindow::sanitizeCurrentPosition()
{
    if (currentFrameNumber > tbcSource.getNumberOfFrames() || currentFieldNumber > tbcSource.getNumberOfFields()) {
        currentFrameNumber = tbcSource.getNumberOfFrames();
        currentFieldNumber = tbcSource.getNumberOfFields();
    }

    if (currentFrameNumber == 0)
    {
        currentFrameNumber = 1;
    }

    if (currentFieldNumber == 0) {
        currentFieldNumber = 1;
    }
}

bool MainWindow::runExternalToolWithProgress(const QString &program, const QStringList &arguments,
                                             const QString &toolDisplayName, QString *errorMessage)
{
    ExternalToolStage stage = ExternalToolStage::Starting;
    qint32 totalFields = 0;
    qint32 processedFields = 0;
    qint32 teletextProgressPercent = -1;
    QString lastOutputLine;

    const QRegularExpression totalFieldsExpression(
        QStringLiteral("Using\\s+\\d+\\s+threads\\s+to\\s+process\\s+([0-9,]+)\\s+fields"),
        QRegularExpression::CaseInsensitiveOption);
    const auto parseCountText = [](QString text, bool *ok) -> qint32 {
        text.remove(QLatin1Char(','));
        return text.toInt(ok);
    };
    const QRegularExpression progressFieldsExpression(
        QStringLiteral("(?:Info:\\s*)?Processing\\s+fields\\s+([0-9,]+)\\s*/\\s*([0-9,]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression legacyFieldExpression(
        QStringLiteral("(?:Info:\\s*)?Processing\\s+field\\s+([0-9,]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression completeFieldsExpression(
        QStringLiteral("(?:Info:\\s*)?Processing\\s+complete\\s*-\\s*([0-9,]+)\\s*fields"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression ansiEscapeExpression(QStringLiteral("\\x1B\\[[0-9;]*[A-Za-z]"));
    const QRegularExpression teletextTqdmPercentExpression(QStringLiteral("(\\d{1,3})%\\|"));

    // The progress dialog's text and percentage for the current stage and counts
    const auto describeProgress = [&](int *percent, QString *label) {
        const bool teletextStage = externalToolStageIsTeletext(stage);
        if (teletextStage && teletextProgressPercent >= 0) {
            *percent = qBound<qint32>(0, teletextProgressPercent, 100);
        } else if (!teletextStage && totalFields > 0) {
            *percent = externalToolProgressPercent(processedFields, totalFields);
        }
        QString counts = externalToolProgressSummary(processedFields, totalFields);
        if (teletextStage) {
            counts += QLatin1Char('\n') + externalToolTeletextProgressSummary(teletextProgressPercent);
        }
        *label = externalToolStageLabel(toolDisplayName, stage) + QLatin1Char('\n') + counts;
    };

    // Follow tbc-process-vbi's output: the stage it is in and its field counts
    // (or, for the teletext export, tqdm's percentage)
    const auto processOutputLine = [&](const QString &line, int *percent, QString *label) {
        QString normalizedLine = line;
        normalizedLine.remove(ansiEscapeExpression);
        const QString trimmedLine = normalizedLine.trimmed();
        if (trimmedLine.isEmpty()) {
            return;
        }
        lastOutputLine = trimmedLine;

        if (trimmedLine.contains(QStringLiteral("Reading metadata from"), Qt::CaseInsensitive)) {
            stage = ExternalToolStage::LoadingMetadata;
        } else if (trimmedLine.contains(QStringLiteral("Beginning"), Qt::CaseInsensitive)
                   && trimmedLine.contains(QStringLiteral("processing"), Qt::CaseInsensitive)) {
            stage = ExternalToolStage::AnalysingMetadata;
        } else if (trimmedLine.contains(QStringLiteral("Writing metadata file"), Qt::CaseInsensitive)) {
            stage = ExternalToolStage::WritingMetadata;
            if (totalFields > 0) {
                processedFields = totalFields;
            }
        } else if (trimmedLine.contains(QStringLiteral("Teletext export output directory"), Qt::CaseInsensitive)
                   || trimmedLine.contains(QStringLiteral("Teletext export: using Python executable"),
                                           Qt::CaseInsensitive)
                   || trimmedLine.contains(QStringLiteral("Teletext Python dependency check"),
                                           Qt::CaseInsensitive)) {
            stage = ExternalToolStage::TeletextDependencyCheck;
            teletextProgressPercent = qMax<qint32>(teletextProgressPercent, 0);
        } else if (trimmedLine.contains(QStringLiteral("Teletext export backend probe:"),
                                        Qt::CaseInsensitive)
                   || trimmedLine.contains(QStringLiteral("optional Python modules missing"),
                                           Qt::CaseInsensitive)) {
            stage = ExternalToolStage::TeletextBackendProbe;
            teletextProgressPercent = qMax<qint32>(teletextProgressPercent, 5);
        } else if (trimmedLine.contains(QStringLiteral("Teletext export: starting deconvolution with"),
                                        Qt::CaseInsensitive)
                   || trimmedLine.contains(QStringLiteral("Teletext export: deconvolution attempt failed"),
                                           Qt::CaseInsensitive)
                   || trimmedLine.contains(QStringLiteral("Teletext export: deconvolution succeeded after fallback"),
                                           Qt::CaseInsensitive)) {
            stage = ExternalToolStage::TeletextDeconvolution;
            teletextProgressPercent = qMax<qint32>(teletextProgressPercent, 10);
        } else if (trimmedLine.contains(QStringLiteral("Teletext export: squashing duplicate packets"),
                                        Qt::CaseInsensitive)) {
            stage = ExternalToolStage::TeletextSquashing;
            teletextProgressPercent = qMax<qint32>(teletextProgressPercent, 70);
        } else if (trimmedLine.contains(QStringLiteral("Teletext export: generating HTML pages"),
                                        Qt::CaseInsensitive)) {
            stage = ExternalToolStage::TeletextHtmlGeneration;
            teletextProgressPercent = qMax<qint32>(teletextProgressPercent, 80);
        } else if (trimmedLine.contains(QStringLiteral("Teletext export complete"), Qt::CaseInsensitive)
                   || trimmedLine.contains(QStringLiteral("Teletext export finished but generated no HTML pages"),
                                           Qt::CaseInsensitive)) {
            stage = ExternalToolStage::Finishing;
            teletextProgressPercent = 100;
        }

        const QRegularExpressionMatch totalFieldsMatch = totalFieldsExpression.match(trimmedLine);
        if (totalFieldsMatch.hasMatch()) {
            bool ok = false;
            const qint32 parsedTotal = parseCountText(totalFieldsMatch.captured(1), &ok);
            if (ok && parsedTotal > 0) {
                totalFields = parsedTotal;
                processedFields = qBound<qint32>(0, processedFields, totalFields);
                stage = ExternalToolStage::AnalysingMetadata;
            }
        }

        const QRegularExpressionMatch progressFieldsMatch = progressFieldsExpression.match(trimmedLine);
        if (progressFieldsMatch.hasMatch()) {
            bool processedOk = false;
            bool totalOk = false;
            const qint32 parsedProcessed = parseCountText(progressFieldsMatch.captured(1), &processedOk);
            const qint32 parsedTotal = parseCountText(progressFieldsMatch.captured(2), &totalOk);
            if (processedOk && totalOk && parsedTotal > 0) {
                totalFields = parsedTotal;
                processedFields = qBound<qint32>(0, parsedProcessed, totalFields);
                stage = ExternalToolStage::AnalysingMetadata;
            }
        } else {
            const QRegularExpressionMatch legacyFieldMatch = legacyFieldExpression.match(trimmedLine);
            if (legacyFieldMatch.hasMatch()) {
                bool processedOk = false;
                const qint32 parsedProcessed = parseCountText(legacyFieldMatch.captured(1), &processedOk);
                if (processedOk) {
                    if (totalFields > 0) {
                        processedFields = qBound<qint32>(0, parsedProcessed, totalFields);
                    } else {
                        processedFields = qMax(processedFields, parsedProcessed);
                    }
                    stage = ExternalToolStage::AnalysingMetadata;
                }
            }
        }

        const QRegularExpressionMatch completeFieldsMatch = completeFieldsExpression.match(trimmedLine);
        if (completeFieldsMatch.hasMatch()) {
            bool completeOk = false;
            const qint32 parsedTotal = parseCountText(completeFieldsMatch.captured(1), &completeOk);
            if (completeOk && parsedTotal > 0) {
                totalFields = parsedTotal;
                processedFields = totalFields;
                stage = ExternalToolStage::Finishing;
            }
        }

        const QRegularExpressionMatch teletextPercentMatch = teletextTqdmPercentExpression.match(trimmedLine);
        if (teletextPercentMatch.hasMatch()) {
            bool percentOk = false;
            const qint32 parsedPercent = teletextPercentMatch.captured(1).toInt(&percentOk);
            if (percentOk) {
                teletextProgressPercent = qBound<qint32>(0, parsedPercent, 100);
                if (!externalToolStageIsTeletext(stage)) {
                    stage = ExternalToolStage::TeletextHtmlGeneration;
                }
            }
        }

        describeProgress(percent, label);
    };

    // A load or save must not start underneath the reprocessing; a file opened
    // meanwhile is queued (requestSourceOpen)
    sourceOperationInProgress = true;
    ProcessProgressRunner::Options options;
    options.onLine = processOutputLine;
    options.interruptFirst = true; // tbc-process-vbi stops cleanly on SIGINT
    int initialPercent = -1;
    QString initialLabel;
    describeProgress(&initialPercent, &initialLabel);
    const ProcessProgressRunner::Result result =
        ProcessProgressRunner::run(program, arguments, this, initialLabel, options);
    sourceOperationInProgress = false;

    switch (result.status) {
    case ProcessProgressRunner::Result::FailedToStart:
        if (errorMessage) {
            *errorMessage = tr("Unable to start %1.").arg(toolDisplayName.toLower());
        }
        return false;
    case ProcessProgressRunner::Result::Cancelled:
        if (errorMessage) {
            *errorMessage = tr("%1 cancelled by user.").arg(toolDisplayName);
        }
        return false;
    case ProcessProgressRunner::Result::DidNotFinish:
        if (errorMessage) {
            *errorMessage = tr("%1 did not finish.").arg(toolDisplayName);
        }
        return false;
    case ProcessProgressRunner::Result::Finished:
        break;
    }

    if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0) {
        if (errorMessage) {
            *errorMessage = !lastOutputLine.isEmpty()
                ? lastOutputLine
                : tr("%1 failed with exit code %2.").arg(toolDisplayName).arg(result.exitCode);
        }
        return false;
    }

    return true;
}

MainWindow::UiStateSnapshot MainWindow::captureUiStateSnapshot() const
{
    UiStateSnapshot snapshot;
    if (!tbcSource.getIsSourceLoaded()) {
        return snapshot;
    }

    snapshot.valid = true;
    snapshot.frameNumber = currentFrameNumber;
    snapshot.fieldNumber = currentFieldNumber;
    snapshot.viewMode = tbcSource.getViewMode();
    snapshot.stretchField = tbcSource.getStretchField();
    snapshot.reverseFieldOrder = tbcSource.getFieldOrder();
    snapshot.highlightDropouts = tbcSource.getHighlightDropouts();
    snapshot.chromaEnabled = tbcSource.getChromaDecoder();
    snapshot.chromaDecodeMode = tbcSource.getChromaDecodeMode();
    snapshot.sourceMode = tbcSource.getSourceMode();
    snapshot.aspectRatioEnabled = displayAspectRatio;
    snapshot.imageScaleFactor = scaleFactor;
    snapshot.activeMainTabIndex = (ui && ui->mainTabWidget) ? ui->mainTabWidget->currentIndex() : 0;

    snapshot.showVbiDialog = vbiDialog && vbiDialog->isVisible();
    snapshot.showDropoutDialog = dropoutAnalysisDialog && dropoutAnalysisDialog->isVisible();
    snapshot.showVisibleDropoutDialog = visibleDropoutAnalysisDialog && visibleDropoutAnalysisDialog->isVisible();
    snapshot.showBlackSnrDialog = blackSnrAnalysisDialog && blackSnrAnalysisDialog->isVisible();
    snapshot.showWhiteSnrDialog = whiteSnrAnalysisDialog && whiteSnrAnalysisDialog->isVisible();
    snapshot.showVideoParametersDialog = videoParametersDialog && videoParametersDialog->isVisible();
    snapshot.showChromaConfigDialog = chromaDecoderConfigDialog && chromaDecoderConfigDialog->isVisible();
    return snapshot;
}

void MainWindow::applyUiStateSnapshot(const UiStateSnapshot &snapshot)
{
    if (!snapshot.valid || !tbcSource.getIsSourceLoaded()) {
        return;
    }

    displayAspectRatio = snapshot.aspectRatioEnabled;
    scaleFactor = snapshot.imageScaleFactor;
    tbcSource.setViewMode(snapshot.viewMode);
    tbcSource.setStretchField(snapshot.stretchField);
    tbcSource.setFieldOrder(snapshot.reverseFieldOrder);
    tbcSource.setHighlightDropouts(snapshot.highlightDropouts);
    tbcSource.setChromaDecodeMode(snapshot.chromaDecodeMode);
    tbcSource.setChromaDecoder(snapshot.chromaEnabled);
    if (tbcSource.getSourceMode() != TbcSource::ONE_SOURCE) {
        tbcSource.setSourceMode(snapshot.sourceMode);
    }

    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    const qint32 totalFields = qMax<qint32>(1, tbcSource.getNumberOfFields());
    if (snapshot.viewMode == TbcSource::ViewMode::FIELD_VIEW) {
        const qint32 targetField = qBound<qint32>(1, snapshot.fieldNumber, totalFields);
        if (targetField != currentFieldNumber) {
            setCurrentField(targetField);
        } else {
            currentFieldNumber = targetField;
            currentFrameNumber = static_cast<qint32>(std::ceil(static_cast<double>(targetField) / 2.0));
            sanitizeCurrentPosition();
        }
    } else {
        const qint32 targetFrame = qBound<qint32>(1, snapshot.frameNumber, totalFrames);
        if (targetFrame != currentFrameNumber) {
            setCurrentFrame(targetFrame);
        } else {
            currentFrameNumber = targetFrame;
            currentFieldNumber = (targetFrame * 2) - 1;
            sanitizeCurrentPosition();
        }
    }

    setViewValues();
    updateBottomStatusReadout();
    updateAspectPushButton();
    updateVideoPushButton();
    updateSourcesPushButton();
    updateImage();

    if (ui && ui->mainTabWidget
        && snapshot.activeMainTabIndex >= 0
        && snapshot.activeMainTabIndex < ui->mainTabWidget->count()) {
        ui->mainTabWidget->setCurrentIndex(snapshot.activeMainTabIndex);
    }

    if (snapshot.showVbiDialog && vbiDialog) {
        vbiDialog->updateVbi(tbcSource.getFrameVbi(), tbcSource.getIsFrameVbiValid());
        vbiDialog->updateVideoId(tbcSource.getFrameVideoId(), tbcSource.getIsFrameVideoIdValid());
        vbiDialog->show();
    }
    if (snapshot.showDropoutDialog && dropoutAnalysisDialog) {
        dropoutAnalysisDialog->show();
    }
    if (snapshot.showVisibleDropoutDialog && visibleDropoutAnalysisDialog) {
        visibleDropoutAnalysisDialog->show();
    }
    if (snapshot.showBlackSnrDialog && blackSnrAnalysisDialog) {
        blackSnrAnalysisDialog->show();
    }
    if (snapshot.showWhiteSnrDialog && whiteSnrAnalysisDialog) {
        whiteSnrAnalysisDialog->show();
    }
    if (snapshot.showVideoParametersDialog && videoParametersDialog) {
        videoParametersDialog->show();
    }
    if (snapshot.showChromaConfigDialog && chromaDecoderConfigDialog) {
        chromaDecoderConfigDialog->show();
    }
}

void MainWindow::queueAnalysisRefreshPreservingUserState(const QString &processedInputFile)
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }
    if (!sameFilePath(processedInputFile, tbcSource.getCurrentSourceFilename())) {
        return;
    }

    pendingUiStateSnapshot = captureUiStateSnapshot();
    restoreUiStateAfterReload = pendingUiStateSnapshot.valid;
    if (statusBar()) {
        statusBar()->clearMessage();
    }

    const QString reloadTarget = lastFilename.isEmpty() ? processedInputFile : lastFilename;
    loadTbcFile(reloadTarget, false, true);
}

// Menu bar signal handlers -------------------------------------------------------------------------------------------

void MainWindow::on_actionExit_triggered()
{
    tbcDebugStream() << "MainWindow::on_actionExit_triggered(): Called";

    // Close the main window like the title-bar button: closeEvent() offers to
    // save unsaved metadata edits, and the application quits once it closes.
    close();
}

// Load a TBC file based on the file selection from the GUI
void MainWindow::on_actionOpen_TBC_file_triggered()
{
    tbcDebugStream() << "MainWindow::on_actionOpen_TBC_file_triggered(): Called";
    QString startPath = configuration.getSourceDirectory();
    QFileInfo startPathInfo(startPath);
    if (startPath.isEmpty() || !startPathInfo.exists()) {
        startPath = QDir::homePath();
    } else if (startPathInfo.isFile()) {
        startPath = startPathInfo.absolutePath();
    }

    QStringList filters;
    filters << tr("TBC/Metadata (*.tbc *.ytbc *.ctbc *.tbcy *.tbcc *.db *.json)")
            << tr("TBC output (*.tbc *.ytbc *.ctbc *.tbcy *.tbcc)")
            << tr("Metadata (*.db *.json)")
            << tr("All Files (*)");

    QString inputFileName;
    QFileDialog fileDialog(this, tr("Open TBC/metadata file"), startPath);
    fileDialog.setFileMode(QFileDialog::ExistingFile);
    fileDialog.setNameFilters(filters);
    fileDialog.selectNameFilter(filters.first());
    fileDialog.setAcceptMode(QFileDialog::AcceptOpen);
    if (fileDialog.exec() == QDialog::Accepted) {
        const QStringList selectedFiles = fileDialog.selectedFiles();
        if (!selectedFiles.isEmpty()) {
            inputFileName = selectedFiles.first();
        }
    }

    // Remember where the user browsed to as soon as they pick something. The
    // source directory used to be written only after a load succeeded, so an
    // unsupported or broken file left the next Open dialog somewhere else.
    if (!inputFileName.isEmpty()) {
        configuration.setSourceDirectory(QFileInfo(inputFileName).absolutePath());
        configuration.writeConfiguration();
    }

    if (!inputFileName.isEmpty() && !isSupportedInputExtension(inputFileName)) {
        QMessageBox::warning(this, tr("Unsupported file"),
                             tr("Please select a supported file type (.tbc, .ytbc, .ctbc, .tbcy, .tbcc, .db, .json)."));
        return;
    }

    // Was a filename specified?
    if (!inputFileName.isEmpty() && !inputFileName.isNull()) {
        requestSourceOpen(inputFileName);
    }
}

// Reload the current TBC selection from the GUI
void MainWindow::on_actionReload_TBC_triggered()
{
    // Reload the current TBC file
    if (!lastFilename.isEmpty() && !lastFilename.isNull()) {
        requestSourceOpen(lastFilename);
    }
}

void MainWindow::on_actionMetadata_Conversion_triggered()
{
    metadataConversionDialog->setSourceDirectory(configuration.getSourceDirectory());
    QString defaultInput;
    if (metadataJsonLoaded && !metadataJsonFilename.isEmpty()) {
        defaultInput = metadataJsonFilename;
    } else if (tbcSource.getIsSourceLoaded()) {
        defaultInput = tbcSource.getCurrentMetadataFilename();
    }
    metadataConversionDialog->setDefaultInput(defaultInput);
    showOrRaise(metadataConversionDialog);
}

void MainWindow::on_actionMetadata_Status_triggered()
{
    updateMetadataStatusPanel();
    showOrRaise(metadataStatusDialog);
}
void MainWindow::on_actionExport_Decode_Metadata_triggered()
{
    QString defaultInput;
    if (tbcSource.getIsSourceLoaded()) {
        defaultInput = tbcSource.getCurrentMetadataFilename();
        if (!defaultInput.endsWith(QStringLiteral(".db"), Qt::CaseInsensitive)
            && !defaultInput.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            const QString sourceMetadataCandidate =
                resolveMetadataFilenameForSource(tbcSource.getCurrentSourceFilename());
            if (sourceMetadataCandidate.endsWith(QStringLiteral(".db"), Qt::CaseInsensitive)
                || sourceMetadataCandidate.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                defaultInput = sourceMetadataCandidate;
            }
        }
    }

    if (!defaultInput.endsWith(QStringLiteral(".db"), Qt::CaseInsensitive)
        && !defaultInput.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
        && metadataJsonLoaded) {
        const QString jsonPath = metadataJsonFilename.trimmed();
        if (!jsonPath.isEmpty() && QFileInfo::exists(jsonPath)) {
            defaultInput = jsonPath;
        }
    }
    if (!defaultInput.endsWith(QStringLiteral(".db"), Qt::CaseInsensitive)
        && !defaultInput.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        defaultInput.clear();
    }

    const QString toolPath = resolveExternalExecutable({QStringLiteral("tbc-export-metadata")});
    if (toolPath.isEmpty()) {
        QMessageBox::warning(this, tr("Tool not found"),
                             tr("tbc-export-metadata was not found in PATH or alongside the application."));
        return;
    }

    if (!metadataExportDialog) {
        metadataExportDialog = new MetadataExportDialog(this);
        connect(metadataExportDialog, &QObject::destroyed, this, [this]() {
            metadataExportDialog = nullptr;
        });
    }
    metadataExportDialog->setExportExecutablePath(toolPath);

    const QString sourceDirectory = configuration.getSourceDirectory();
    if (!sourceDirectory.isEmpty()) {
        metadataExportDialog->setSourceDirectory(sourceDirectory);
    }
    if (!defaultInput.isEmpty()) {
        metadataExportDialog->setDefaultInputFile(defaultInput);
    }

    showOrRaise(metadataExportDialog);

    if (!defaultInput.isEmpty()) {
        statusBar()->showMessage(tr("Opened Metadata Export GUI with %1").arg(defaultInput), 5000);
    } else {
        statusBar()->showMessage(tr("Opened Metadata Export GUI. Select a metadata input file (.db or .json)."), 5000);
    }
}

void MainWindow::on_actionProcess_VBI_triggered()
{
    // Processing rewrites the metadata on disk and reloads it
    if (!maybeSave([this]() { on_actionProcess_VBI_triggered(); })) {
        return;
    }

    QString defaultInput;
    if (tbcSource.getIsSourceLoaded()) {
        defaultInput = tbcSource.getCurrentSourceFilename();
    }
    QString inputFileName;
    if (tbcSource.getIsSourceLoaded() && !tbcSource.getIsMetadataOnly()) {
        const QString loadedSourceFilename = tbcSource.getCurrentSourceFilename();
        if (!loadedSourceFilename.isEmpty() && QFileInfo::exists(loadedSourceFilename)) {
            inputFileName = loadedSourceFilename;
        }
    }

    if (inputFileName.isEmpty()) {
        const QString startPath = defaultInput.isEmpty() ? configuration.getSourceDirectory() : defaultInput;
        inputFileName = QFileDialog::getOpenFileName(this,
                                                     tr("Select TBC file for VBI processing"),
                                                     startPath,
                                                     tr("TBC files (*.tbc *.ytbc *.ctbc *.tbcy *.tbcc);;All Files (*)"));
    }
    if (inputFileName.isEmpty()) {
        return;
    }

    const QString toolPath = resolveExternalExecutable({QStringLiteral("tbc-process-vbi")});
    if (toolPath.isEmpty()) {
        QMessageBox::warning(this, tr("Tool not found"),
                             tr("tbc-process-vbi was not found in PATH or alongside the application."));
        return;
    }

    QString preferredMetadataFilename;
    if (tbcSource.getIsSourceLoaded()
        && sameFilePath(inputFileName, tbcSource.getCurrentSourceFilename())) {
        preferredMetadataFilename = tbcSource.getCurrentMetadataFilename();
    }
    const QString metadataFilename = resolveMetadataFilenameForSource(inputFileName, preferredMetadataFilename);
    if (metadataFilename.isEmpty()) {
        QMessageBox::warning(this, tr("Metadata not found"),
                             tr("Could not find metadata for:\n%1\n\nExpected .db or .json metadata file.")
                                 .arg(inputFileName));
        return;
    }

    // Show the VBI processing options dialog (defaults/last-used come from configuration).
    VbiProcessingOptions opts = configuration.getVbiProcessingOptions();
    VbiProcessingDialog processingDialog(opts, this);
    if (processingDialog.exec() != QDialog::Accepted) {
        statusBar()->showMessage(tr("VBI processing cancelled"), 3000);
        return;
    }
    opts = processingDialog.selectedOptions();
    configuration.setVbiProcessingOptions(opts);
    configuration.writeConfiguration();

    QString backupErrorMessage;
    if (!createTimestampedMetadataBackup(metadataFilename, QStringLiteral(".bup"), &backupErrorMessage)) {
        QMessageBox::warning(this, tr("Backup failed"),
                             backupErrorMessage.isEmpty()
                                 ? tr("Could not create metadata backup before processing.")
                                 : backupErrorMessage);
        return;
    }

    QStringList toolArguments = {
        metadataInputOptionForTool(toolPath), metadataFilename
    };
    const QString noBackupOption = noBackupOptionForTool(toolPath);
    if (!noBackupOption.isEmpty()) {
        toolArguments << noBackupOption;
    }

    // Processing options: disable the unticked in-process VBI decoders. Each flag
    // is guarded so the GUI degrades gracefully against older tbc-process-vbi
    // builds that don't yet support it (the type just runs on those builds).
    if (!opts.vbiCore && toolHelpListsOption(toolPath, QStringLiteral("--no-vbi-core"))) {
        toolArguments << QStringLiteral("--no-vbi-core");
    }
    if (!opts.ntsc && toolHelpListsOption(toolPath, QStringLiteral("--no-ntsc"))) {
        toolArguments << QStringLiteral("--no-ntsc");
    }
    if (!opts.vitc && toolHelpListsOption(toolPath, QStringLiteral("--no-vitc"))) {
        toolArguments << QStringLiteral("--no-vitc");
    }
    if (!opts.closedCaptions && toolHelpListsOption(toolPath, QStringLiteral("--no-closed-captions"))) {
        toolArguments << QStringLiteral("--no-closed-captions");
    }

    // Teletext is opt-in. New builds default teletext off and use --teletext to
    // enable it; older builds default teletext on and use --no-teletext-html to
    // suppress it. Handle both so the GUI works across build versions.
    QString teletextOutputDirectory;
    const bool toolHasTeletextFlag = toolHelpListsOption(toolPath, QStringLiteral("--teletext"));
    if (opts.teletext) {
        teletextOutputDirectory = opts.teletextHtmlDir.trimmed().isEmpty()
            ? defaultTeletextHtmlDirectoryForInput(inputFileName)
            : opts.teletextHtmlDir.trimmed();
        if (toolHasTeletextFlag) {
            toolArguments << QStringLiteral("--teletext");
        }
        if (!teletextOutputDirectory.isEmpty()
            && toolHelpListsOption(toolPath, QStringLiteral("--teletext-html-dir"))) {
            toolArguments << QStringLiteral("--teletext-html-dir") << teletextOutputDirectory;
        }
        if (toolHelpListsOption(toolPath, QStringLiteral("--teletext-tape-format"))
            && !opts.teletextTapeFormat.trimmed().isEmpty()
            && opts.teletextTapeFormat.trimmed().compare(QStringLiteral("vhs"), Qt::CaseInsensitive) != 0) {
            toolArguments << QStringLiteral("--teletext-tape-format") << opts.teletextTapeFormat.trimmed();
        }
        if (toolHelpListsOption(toolPath, QStringLiteral("--teletext-min-duplicates"))
            && opts.teletextMinDuplicates > 1) {
            toolArguments << QStringLiteral("--teletext-min-duplicates") << QString::number(opts.teletextMinDuplicates);
        }
    } else if (!toolHasTeletextFlag && toolHelpListsOption(toolPath, QStringLiteral("--no-teletext-html"))) {
        // Old build where teletext defaults on: explicitly suppress it.
        toolArguments << QStringLiteral("--no-teletext-html");
    }

    // VITS is an opt-in in-process mode of tbc-process-vbi (--vits).
    if (opts.vits) {
        if (toolHelpListsOption(toolPath, QStringLiteral("--vits"))) {
            toolArguments << QStringLiteral("--vits");
        } else {
            QMessageBox::warning(this, tr("VITS not supported"),
                                 tr("The selected tbc-process-vbi build does not support the --vits flag. "
                                    "VITS metrics were not processed. Use a newer tbc-process-vbi build."));
            // Continue without --vits rather than aborting the whole VBI run.
        }
    }

    toolArguments << inputFileName;

    QString errorMessage;
    if (!runExternalToolWithProgress(toolPath, toolArguments, tr("VBI processing"), &errorMessage)) {
        if (errorMessage.contains(tr("cancelled"), Qt::CaseInsensitive)) {
            statusBar()->showMessage(tr("VBI processing cancelled for %1").arg(inputFileName), 4000);
            return;
        }
        QMessageBox::warning(this, tr("Process failed"),
                             errorMessage.isEmpty()
                                 ? tr("tbc-process-vbi failed.")
                                 : errorMessage);
        return;
    }

    const bool reloadingCurrentSource = tbcSource.getIsSourceLoaded()
                                        && sameFilePath(inputFileName, tbcSource.getCurrentSourceFilename());

    // Only auto-open the teletext viewer when teletext was actually requested
    // and produced output.
    QString autoTeletextDirectory;
    if (opts.teletext) {
        autoTeletextDirectory = resolveTeletextHtmlDirectoryFromHints({
            teletextOutputDirectory,
            inputFileName,
            metadataFilename
        });
        if (!autoTeletextDirectory.isEmpty()) {
            if (!teletextViewerDialog) {
                teletextViewerDialog = new TeletextViewerDialog(this);
                teletextViewerDialog->setConfiguration(&configuration);
            }
            if (teletextViewerDialog->directory().compare(autoTeletextDirectory, Qt::CaseInsensitive) != 0) {
                teletextViewerDialog->setDirectory(autoTeletextDirectory);
            }
            showOrRaise(teletextViewerDialog);
        }
    }
    if (reloadingCurrentSource) {
        queueAnalysisRefreshPreservingUserState(inputFileName);
    } else {
        if (!autoTeletextDirectory.isEmpty()) {
            statusBar()->showMessage(
                tr("VBI processing completed for %1 (teletext loaded from %2)")
                    .arg(inputFileName, autoTeletextDirectory),
                5000
            );
        } else {
            statusBar()->showMessage(tr("VBI processing completed for %1").arg(inputFileName), 4000);
        }
    }
}

void MainWindow::on_actionFix_JSON_SNR_triggered()
{
    // Fixing rewrites the metadata on disk and reloads it
    if (!maybeSave([this]() { on_actionFix_JSON_SNR_triggered(); })) {
        return;
    }

    const auto isMetadataFile = [](const QString &filename) {
        return filename.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
               || filename.endsWith(QStringLiteral(".db"), Qt::CaseInsensitive);
    };
    const auto isTbcSourceFile = [](const QString &filename) {
        return filename.endsWith(QStringLiteral(".tbc"), Qt::CaseInsensitive)
               || filename.endsWith(QStringLiteral(".ytbc"), Qt::CaseInsensitive)
               || filename.endsWith(QStringLiteral(".ctbc"), Qt::CaseInsensitive)
               || filename.endsWith(QStringLiteral(".tbcy"), Qt::CaseInsensitive)
               || filename.endsWith(QStringLiteral(".tbcc"), Qt::CaseInsensitive);
    };

    // VITS metrics processing is now a mode of tbc-process-vbi (--vits), so fix
    // JSON SNR by running tbc-process-vbi with only VITS enabled (the four VBI
    // decoders disabled to preserve any manually-edited VBI metadata).
    const QString toolPath = resolveExternalExecutable({QStringLiteral("tbc-process-vbi")});
    if (toolPath.isEmpty()) {
        QMessageBox::warning(this, tr("Tool not found"),
                             tr("tbc-process-vbi was not found in PATH or alongside the application."));
        return;
    }

    QString metadataFilename;
    QString inputTbcFilename;
    const QString currentMetadataFilename = tbcSource.getCurrentMetadataFilename();
    if (!currentMetadataFilename.isEmpty()
        && QFileInfo::exists(currentMetadataFilename)
        && isMetadataFile(currentMetadataFilename)) {
        metadataFilename = currentMetadataFilename;
    } else if (metadataJsonLoaded
               && !metadataJsonFilename.isEmpty()
               && QFileInfo::exists(metadataJsonFilename)
               && isMetadataFile(metadataJsonFilename)) {
        metadataFilename = metadataJsonFilename;
    }

    if (metadataFilename.isEmpty()) {
        QMessageBox::warning(this, tr("Metadata not found"),
                             tr("Could not determine the currently loaded metadata file.\n"
                                "Load the metadata you want to fix in Analyse, then run Fix JSON SNR again."));
        return;
    }
    if (!isMetadataFile(metadataFilename)) {
        QMessageBox::warning(this, tr("Unsupported metadata file"),
                             tr("The currently loaded metadata is not a .json or .db file.\n"
                                "Load a supported metadata file in Analyse first."));
        return;
    }

    if (!tbcSource.getIsMetadataOnly()) {
        const QString currentSourceFilename = tbcSource.getCurrentSourceFilename();
        if (!currentSourceFilename.isEmpty()
            && QFileInfo::exists(currentSourceFilename)
            && isTbcSourceFile(currentSourceFilename)) {
            inputTbcFilename = currentSourceFilename;
        }
    }

    if (inputTbcFilename.isEmpty()) {
        inputTbcFilename = resolveSourceFilenameForMetadata(metadataFilename);
    }
    if (inputTbcFilename.isEmpty()) {
        QMessageBox::warning(this, tr("Source TBC not found"),
                             tr("Could not determine the source TBC for the currently loaded metadata:\n%1\n\n"
                                "Load the matching source/metadata in Analyse first, then run Fix JSON SNR again.")
                                 .arg(metadataFilename));
        return;
    }

    if (!isTbcSourceFile(inputTbcFilename)) {
        QMessageBox::warning(this, tr("Unsupported TBC file"),
                             tr("The resolved source file is not a supported TBC input:\n%1")
                                 .arg(inputTbcFilename));
        return;
    }

    QString backupErrorMessage;
    if (!createTimestampedMetadataBackup(metadataFilename, QStringLiteral(".vbup"), &backupErrorMessage)) {
        QMessageBox::warning(this, tr("Backup failed"),
                             backupErrorMessage.isEmpty()
                                 ? tr("Could not create metadata backup before processing.")
                                 : backupErrorMessage);
        return;
    }

    QStringList toolArguments = {
        metadataInputOptionForTool(toolPath), metadataFilename
    };
    const QString noBackupOption = noBackupOptionForTool(toolPath);
    if (!noBackupOption.isEmpty()) {
        toolArguments << noBackupOption;
    }

    // Only recompute SNR: enable VITS and disable the four VBI decoders so
    // existing VBI/NTSC/VITC/closed-caption metadata is round-tripped unchanged.
    // Each flag is guarded so an older tbc-process-vbi build degrades gracefully
    // (missing --no-* flags just mean those decoders re-run, which is harmless;
    // a missing --vits flag makes the operation a no-op and is reported below).
    if (toolHelpListsOption(toolPath, QStringLiteral("--vits"))) {
        toolArguments << QStringLiteral("--vits");
    } else {
        QMessageBox::warning(this, tr("VITS not supported"),
                             tr("The selected tbc-process-vbi build does not support the --vits flag, "
                                "so the SNR metrics cannot be recomputed. Use a newer tbc-process-vbi build."));
        return;
    }
    if (toolHelpListsOption(toolPath, QStringLiteral("--no-vbi-core"))) {
        toolArguments << QStringLiteral("--no-vbi-core");
    }
    if (toolHelpListsOption(toolPath, QStringLiteral("--no-ntsc"))) {
        toolArguments << QStringLiteral("--no-ntsc");
    }
    if (toolHelpListsOption(toolPath, QStringLiteral("--no-vitc"))) {
        toolArguments << QStringLiteral("--no-vitc");
    }
    if (toolHelpListsOption(toolPath, QStringLiteral("--no-closed-captions"))) {
        toolArguments << QStringLiteral("--no-closed-captions");
    }
    toolArguments << inputTbcFilename;

    QString errorMessage;
    if (!runExternalToolWithProgress(toolPath, toolArguments, tr("Fix JSON SNR"), &errorMessage)) {
        if (errorMessage.contains(tr("cancelled"), Qt::CaseInsensitive)) {
            statusBar()->showMessage(tr("Fix JSON SNR cancelled for %1").arg(metadataFilename), 4000);
            return;
        }
        QMessageBox::warning(this, tr("Process failed"),
                             errorMessage.isEmpty()
                                 ? tr("Fix JSON SNR failed.")
                                 : errorMessage);
        return;
    }

    const bool reloadingCurrentSource = tbcSource.getIsSourceLoaded()
                                        && sameFilePath(inputTbcFilename, tbcSource.getCurrentSourceFilename());
    const bool reloadingCurrentMetadata = tbcSource.getIsSourceLoaded()
                                          && (sameFilePath(metadataFilename, tbcSource.getCurrentMetadataFilename())
                                              || sameFilePath(metadataFilename, metadataJsonFilename));
    if (reloadingCurrentSource) {
        queueAnalysisRefreshPreservingUserState(inputTbcFilename);
    } else if (reloadingCurrentMetadata && !lastFilename.isEmpty()) {
        loadTbcFile(lastFilename, false, true);
    } else {
        statusBar()->showMessage(tr("Fix JSON SNR completed for %1").arg(metadataFilename), 4000);
    }
}

void MainWindow::on_actionAuto_Audio_Align_triggered()
{

    QString defaultJsonPath;
    if (metadataJsonLoaded && !metadataJsonFilename.isEmpty()) {
        defaultJsonPath = metadataJsonFilename;
    } else if (tbcSource.getIsSourceLoaded()) {
        const QString currentMetadataFilename = tbcSource.getCurrentMetadataFilename();
        if (currentMetadataFilename.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            defaultJsonPath = currentMetadataFilename;
        } else if (currentMetadataFilename.endsWith(QStringLiteral(".db"), Qt::CaseInsensitive)) {
            QString candidateJsonPath = currentMetadataFilename;
            candidateJsonPath.chop(3);
            candidateJsonPath += QStringLiteral(".json");
            if (QFileInfo::exists(candidateJsonPath)) {
                defaultJsonPath = candidateJsonPath;
            }
        }

        if (defaultJsonPath.isEmpty()) {
            const QString currentSourceFilename = tbcSource.getCurrentSourceFilename();
            if (!currentSourceFilename.isEmpty()) {
                const QString sidecarJsonPath = currentSourceFilename + QStringLiteral(".json");
                if (QFileInfo::exists(sidecarJsonPath)) {
                    defaultJsonPath = sidecarJsonPath;
                } else {
                    const QFileInfo sourceInfo(currentSourceFilename);
                    const QString stemJsonPath = QDir(sourceInfo.absolutePath())
                                                     .filePath(sourceInfo.completeBaseName()
                                                               + QStringLiteral(".json"));
                    if (QFileInfo::exists(stemJsonPath)) {
                        defaultJsonPath = stemJsonPath;
                    }
                }
            }
        }
    }

    if (!audioAlignmentDialog) {
        audioAlignmentDialog = new AudioAlignmentDialog(this);
        audioAlignmentDialog->setWindowFlags(Qt::Window
                                             | Qt::CustomizeWindowHint
                                             | Qt::WindowTitleHint
                                             | Qt::WindowSystemMenuHint
                                             | Qt::WindowMinimizeButtonHint
                                             | Qt::WindowCloseButtonHint);
        connect(audioAlignmentDialog, &QObject::destroyed, this, [this]() {
            audioAlignmentDialog = nullptr;
        });
        connect(audioAlignmentDialog,
                &AudioAlignmentDialog::exportTracksPrepared,
                this,
                [this](const QStringList &trackFiles, const QStringList &trackNames) {
            if (!exportDialog || trackFiles.isEmpty()) {
                return;
            }
            exportDialog->loadAudioTracksForExport(trackFiles, trackNames);
            if (ui && ui->mainTabWidget) {
                ui->mainTabWidget->setCurrentWidget(exportDialog);
            }
            statusBar()->showMessage(tr("Loaded %1 aligned audio track(s) into Export.")
                                         .arg(trackFiles.size()),
                                     5000);
        });
    }

    const QString sourceDirectory = configuration.getSourceDirectory();
    if (!sourceDirectory.isEmpty()) {
        audioAlignmentDialog->setSourceDirectory(sourceDirectory);
    }
    audioAlignmentDialog->setExportTrackOutputFile(QString());
    if (!defaultJsonPath.isEmpty()) {
        audioAlignmentDialog->setDefaultJson(defaultJsonPath);
    }
    // The decoder's stored RF rate (the rate fileLoc counts in) wins over the
    // JSON probe: it is read from whichever store is open, .tbc.db included,
    // where the JSON beside it may be absent or predate the key.
    if (tbcSource.getIsSourceLoaded()) {
        const double storedRfRateHz = tbcSource.getVideoParameters().rfSourceSampleRateHz;
        if (storedRfRateHz > 0.0) {
            audioAlignmentDialog->setRfVideoSampleRateFromMetadata(static_cast<quint32>(std::lround(storedRfRateHz)));
        }
    }

    showOrRaise(audioAlignmentDialog);

    if (!defaultJsonPath.isEmpty()) {
        statusBar()->showMessage(tr("Opened Auto Audio Align with %1").arg(defaultJsonPath), 5000);
    } else {
        statusBar()->showMessage(tr("Opened Auto Audio Align. Select metadata JSON and audio input files."), 5000);
    }
}

void MainWindow::on_actionEFM_Handler_triggered()
{
    if (!efmHandlerDialog) {
        efmHandlerDialog = new EfmHandlerDialog(this);
        efmHandlerDialog->setConfiguration(&configuration);
        efmHandlerDialog->setWindowFlags(Qt::Window
                                         | Qt::CustomizeWindowHint
                                         | Qt::WindowTitleHint
                                         | Qt::WindowSystemMenuHint
                                         | Qt::WindowMinimizeButtonHint
                                         | Qt::WindowCloseButtonHint);
        connect(efmHandlerDialog, &QObject::destroyed, this, [this]() {
            efmHandlerDialog = nullptr;
        });
        connect(efmHandlerDialog, &EfmHandlerDialog::exportTracksPrepared, this,
                [this](const QStringList &trackFiles, const QStringList &trackNames) {
                    if (!exportDialog || trackFiles.isEmpty()) {
                        return;
                    }
                    exportDialog->loadAudioTracksForExport(trackFiles, trackNames);
                    if (ui && ui->mainTabWidget) {
                        ui->mainTabWidget->setCurrentWidget(exportDialog);
                    }
                    statusBar()->showMessage(tr("Loaded %1 decoded EFM audio track(s) into Export.")
                                                 .arg(trackFiles.size()),
                                             5000);
                });
    }

    QString sourceDirectory = outputRootDirectoryForCurrentSource();
    if (sourceDirectory.isEmpty()) {
        sourceDirectory = configuration.getSourceDirectory();
    }
    if (!sourceDirectory.isEmpty()) {
        efmHandlerDialog->setSourceDirectory(sourceDirectory);
    }

    QString suggestedOutputBase;
    const QString sourceFilename = tbcSource.getCurrentSourceFilename();
    if (!sourceFilename.isEmpty()) {
        const QFileInfo sourceInfo(sourceFilename);
        suggestedOutputBase = QDir(sourceInfo.absolutePath())
                                  .filePath(sourceInfo.completeBaseName());
    } else if (!lastFilename.isEmpty()) {
        const QFileInfo sourceInfo(lastFilename);
        suggestedOutputBase = QDir(sourceInfo.absolutePath())
                                  .filePath(sourceInfo.completeBaseName());
    }
    if (!suggestedOutputBase.isEmpty()) {
        efmHandlerDialog->setSuggestedOutputBase(suggestedOutputBase);
    }
    applyEfmHandlerAutoloads(sourceDirectory);

    showOrRaise(efmHandlerDialog);
    statusBar()->showMessage(tr("Opened EFM Handler. Configure EFM/AC3 stages and run the pipeline."), 5000);
}

void MainWindow::on_actionLDS_Converter_triggered()
{
    const QString toolPath = resolveExternalExecutable({QStringLiteral("ld-lds-converter")});
    if (toolPath.isEmpty()) {
        QMessageBox::warning(this, tr("Tool not found"),
                             tr("ld-lds-converter was not found in PATH or alongside the application."));
        return;
    }

    const QStringList toolArguments = {QStringLiteral("--gui")};
    const QString workingDirectory = QFileInfo(toolPath).absolutePath();
    qint64 launchedProcessId = 0;
    const bool started = QProcess::startDetached(toolPath,
                                                 toolArguments,
                                                 workingDirectory,
                                                 &launchedProcessId);
    if (!started) {
        QMessageBox::warning(this, tr("Launch failed"),
                             tr("Could not launch ld-lds-converter as an independent application."));
        return;
    }

    if (statusBar()) {
        statusBar()->showMessage(tr("Opened LDS Converter as an independent application."), 5000);
    }
}
// Start saving the modified metadata
void MainWindow::on_actionSave_Metadata_triggered()
{
    sourceOperationInProgress = true;
    tbcSource.saveSourceMetadata();

    // Saving continues in the background...
}

// Before anything that would discard unsaved metadata edits (closing, opening
// or reloading a source, reprocessing it), offer to save them. Returns true
// when the caller may go ahead now. Choosing Save starts the (asynchronous)
// save and returns false: continueAfterSave then runs once it has succeeded,
// and nothing runs if it fails.
bool MainWindow::maybeSave(std::function<void()> continueAfterSave)
{
    if (!isWindowModified()) {
        return true;
    }

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, tr("Unsaved metadata changes"),
        tr("The metadata of %1 has unsaved changes.\nDo you want to save them?")
            .arg(tbcSource.getCurrentSourceFilename()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Discard) {
        return true;
    }
    if (choice == QMessageBox::Save) {
        afterSaveAction = std::move(continueAfterSave);
        on_actionSave_Metadata_triggered();
    }
    return false;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // A load or save is running on the worker thread; its busy dialog is up
    if (sourceOperationInProgress) {
        event->ignore();
        return;
    }
    if (!maybeSave([this]() { close(); })) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

// Display the scan line oscilloscope view
void MainWindow::on_actionLine_scope_triggered()
{
    if (tbcSource.getIsSourceLoaded()) {
        // Show the oscilloscope dialogue for the selected scan-line
        showOrRaise(oscilloscopeDialog);
        updateOscilloscopeDialogue();
    }
}

// Display the vectorscope view
void MainWindow::on_actionVectorscope_triggered()
{
    if (tbcSource.getIsSourceLoaded()) {
        // Show the vectorscope dialogue
        showOrRaise(vectorscopeDialog);
        updateVectorscopeDialogue();
    }
}

// Display the waveform monitor view
void MainWindow::on_actionWaveform_monitor_triggered()
{
    if (tbcSource.getIsSourceLoaded() && !tbcSource.getIsMetadataOnly()) {
        showOrRaise(waveformMonitorDialog);
        updateWaveformMonitorDialogue();
    }
}

// Display the RGB scope pop-out view
void MainWindow::on_actionRGB_scope_triggered()
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }
    showOrRaise(rgbScopeDialog);
    updateRgbScopeDialogue(true);
}

// Display the YUV range scope pop-out view
void MainWindow::on_actionYUV_range_scope_triggered()
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }
    showOrRaise(yuvRangeDialog);
    updateYuvRangeScopeDialogue(true);
}

// Display the field timing scope view
void MainWindow::on_actionField_timing_scope_triggered()
{
    if (tbcSource.getIsSourceLoaded()) {
        updateFieldTimingDialogue();
        showOrRaise(fieldTimingDialog);
    }
}

// Open the TBC Tools Wiki page
void MainWindow::on_actionTBC_Tools_Wiki_triggered()
{
    const QUrl wikiUrl(QStringLiteral("https://github.com/harrypm/tbc-tools/wiki"));
    if (!QDesktopServices::openUrl(wikiUrl)) {
        QMessageBox::warning(this, tr("Warning"),
                             tr("Could not open TBC Tools Wiki URL:\n%1").arg(wikiUrl.toString()));
        return;
    }

    if (statusBar()) {
        statusBar()->showMessage(tr("Opened TBC Tools Wiki."), 4000);
    }
}

// Show the about window
void MainWindow::on_actionAbout_ld_analyse_triggered()
{
    aboutDialog->show();
}

// Show the Plugin Manager (Plugins > Plugin Manager...)
void MainWindow::on_actionPluginManager_triggered()
{
    if (!pluginManagerDialog) {
        pluginManagerDialog = new PluginManagerDialog(this);
        pluginManagerDialog->setConfiguration(&configuration);
    }
    showOrRaise(pluginManagerDialog);
}

// Show the Metadata Editor (Tools > Metadata Editor...)
void MainWindow::on_actionMetadata_Editor_triggered()
{
    if (!tbcSource.getIsSourceLoaded()) {
        statusBar()->showMessage(tr("No source loaded to edit metadata for."), 3000);
        return;
    }
    metadataEditorDialog->setVideoParameters(tbcSource.getVideoParameters());
    metadataEditorDialog->setPcmAudioParameters(tbcSource.getPcmAudioParameters());
    // Push the current frame's first-field SECAM line identity so the per-field
    // checkbox reflects the loaded source. Only meaningful for SECAM-family.
    {
        const VideoSystem system = tbcSource.getSystem();
        const bool isSecamFamily = (system == SECAM || system == MESECAM);
        const qint32 firstField = tbcSource.getFirstFieldNumber();
        const bool firstLineIsRed = (firstField >= 1) ? tbcSource.getSecamFirstLineIsRed(firstField) : false;
        metadataEditorDialog->setSecamFieldContext(firstField, firstLineIsRed, isSecamFamily);
    }
    showOrRaise(metadataEditorDialog);
}

// Check for updates - manual trigger from the Help menu
void MainWindow::on_actionCheck_for_Updates_triggered()
{
    if (!updateChecker) {
        return;
    }
    // Manual checks always present their full result to the user.
    updateCheckSilent = false;
    if (statusBar()) {
        statusBar()->showMessage(tr("Checking GitHub for updates..."), 0);
    }
    updateChecker->checkForUpdates();
}

// Automatic background check, throttled to once per week via the stored timestamp.
void MainWindow::maybePerformWeeklyUpdateCheck()
{
    if (!updateChecker) {
        return;
    }
    if (!configuration.getUpdateCheckEnabled()) {
        return;
    }

    // Skip the automatic check for unversioned/dev builds to avoid noisy prompts,
    // and for GDH fork builds, whose releases are not on the feed this polls.
    const QString currentVersion = TbcBuildInfo::version();
    if (currentVersion.isEmpty() || currentVersion == QStringLiteral("0.0.0")
        || currentVersion.contains(QStringLiteral("-gdh-"))) {
        return;
    }

    const QString lastTimestamp = configuration.getLastUpdateCheckTimestamp();
    if (!lastTimestamp.isEmpty()) {
        const QDateTime lastCheck = QDateTime::fromString(lastTimestamp, Qt::ISODate);
        if (lastCheck.isValid()) {
            const qint64 secsSinceLastCheck = lastCheck.secsTo(QDateTime::currentDateTimeUtc());
            const qint64 weekSeconds = 7 * 24 * 60 * 60;
            if (secsSinceLastCheck < weekSeconds) {
                tbcDebugStream() << "MainWindow::maybePerformWeeklyUpdateCheck(): last check"
                                 << secsSinceLastCheck << "s ago, not due yet";
                return;
            }
        }
    }

    tbcDebugStream() << "MainWindow::maybePerformWeeklyUpdateCheck(): performing weekly update check";
    updateCheckSilent = true;
    updateChecker->checkForUpdates();
}

// Persist the timestamp of this check attempt so the weekly throttle works.
void MainWindow::recordUpdateCheckAttempt()
{
    configuration.setLastUpdateCheckTimestamp(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    configuration.writeConfiguration();
}

void MainWindow::onUpdateAvailable(const QString &latestVersion, const QString &releaseUrl, const QString &releaseName)
{
    recordUpdateCheckAttempt();
    if (statusBar()) {
        statusBar()->clearMessage();
    }

    if (updateCheckSilent) {
        // Honour a previously-saved "Skip this version" for the automatic check.
        if (!latestVersion.isEmpty()
            && latestVersion == configuration.getSkippedUpdateVersion()) {
            tbcDebugStream() << "MainWindow::onUpdateAvailable(): version" << latestVersion
                             << "skipped by user; suppressing automatic prompt";
            return;
        }
    }

    showUpdateAvailableDialog(latestVersion, releaseUrl, releaseName);
}

void MainWindow::onUpdateUpToDate(const QString &currentVersion, const QString &latestVersion, const QString &releaseUrl)
{
    Q_UNUSED(releaseUrl)
    recordUpdateCheckAttempt();
    if (statusBar()) {
        statusBar()->clearMessage();
    }

    if (updateCheckSilent) {
        if (statusBar()) {
            statusBar()->showMessage(tr("tbc-tools is up to date (v%1).").arg(currentVersion), 5000);
        }
        tbcDebugStream() << "MainWindow::onUpdateUpToDate(): up to date (" << currentVersion << ")";
        return;
    }

    QMessageBox::information(this, tr("Up to date"),
        tr("You are running the latest version of tbc-tools.\n\n"
           "Installed: %1\nLatest: %2").arg(currentVersion, latestVersion));
}

void MainWindow::onUpdateCheckFailed(const QString &errorString)
{
    recordUpdateCheckAttempt();
    if (statusBar()) {
        statusBar()->clearMessage();
    }

    tbcDebugStream() << "MainWindow::onUpdateCheckFailed():" << errorString;

    if (updateCheckSilent) {
        // Stay quiet on automatic failures - the user can still check manually.
        return;
    }

    // Window-modal and asynchronous: no nested event loop
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("Update check failed"));
    box->setText(tr("Could not check for updates."));
    box->setInformativeText(errorString + QStringLiteral("\n\n") +
        tr("You can view the latest release in your browser instead."));
    QPushButton *openButton = box->addButton(tr("Open releases page"), QMessageBox::AcceptRole);
    box->addButton(QMessageBox::Close);
    connect(box, &QMessageBox::finished, this, [box, openButton]() {
        if (box->clickedButton() == openButton) {
            QDesktopServices::openUrl(QUrl(UpdateChecker::releasesUrl()));
        }
    });
    box->open();
}

void MainWindow::showUpdateAvailableDialog(const QString &latestVersion, const QString &releaseUrl, const QString &releaseName)
{
    const QString currentVersion = TbcBuildInfo::version();

    // Window-modal and asynchronous: no nested event loop
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Information);
    box->setWindowTitle(tr("Update available"));
    box->setText(tr("A new version of tbc-tools is available."));
    QString info = tr("Installed: %1\nLatest: %2").arg(currentVersion, latestVersion);
    if (!releaseName.isEmpty()) {
        info += QStringLiteral("\n") + releaseName;
    }
    info += QStringLiteral("\n\n") + tr("Would you like to open the release page to download it?");
    box->setInformativeText(info);

    QPushButton *downloadButton = box->addButton(tr("Download"), QMessageBox::AcceptRole);
    QPushButton *skipButton = box->addButton(tr("Skip this version"), QMessageBox::ActionRole);
    // "Remind me later" is a no-op: the timestamp was already recorded, so it
    // won't prompt again for a week.
    box->addButton(tr("Remind me later"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::finished, this,
            [this, box, downloadButton, skipButton, latestVersion, releaseUrl]() {
        QAbstractButton *clicked = box->clickedButton();
        if (clicked == downloadButton) {
            const QUrl url(releaseUrl.isEmpty() ? UpdateChecker::releasesUrl() : releaseUrl);
            if (!QDesktopServices::openUrl(url)) {
                QMessageBox::warning(this, tr("Warning"),
                    tr("Could not open the release URL:\n%1").arg(url.toString()));
            }
        } else if (clicked == skipButton) {
            configuration.setSkippedUpdateVersion(latestVersion);
            configuration.writeConfiguration();
            if (statusBar()) {
                statusBar()->showMessage(tr("Skipped version %1. You can still check manually via Help > Check for Updates.").arg(latestVersion), 6000);
            }
        }
    });
    box->open();
}
void MainWindow::on_actionTeletext_Viewer_triggered()
{
    if (!teletextViewerDialog) {
        teletextViewerDialog = new TeletextViewerDialog(this);
        teletextViewerDialog->setConfiguration(&configuration);
    }
    const QString suggestedDirectory = resolveTeletextHtmlDirectoryFromHints({
        tbcSource.getCurrentSourceFilename(),
        lastFilename,
        configuration.getSourceDirectory()
    });

    if (!suggestedDirectory.isEmpty()
        && teletextViewerDialog->directory().compare(suggestedDirectory, Qt::CaseInsensitive) != 0) {
        teletextViewerDialog->setDirectory(suggestedDirectory);
    }

    showOrRaise(teletextViewerDialog);
}

// Show the VBI window
void MainWindow::on_actionVBI_triggered()
{
    // Show the VBI dialogue
    vbiDialog->updateVbi(tbcSource.getFrameVbi(), tbcSource.getIsFrameVbiValid());
    vbiDialog->updateVideoId(tbcSource.getFrameVideoId(), tbcSource.getIsFrameVideoIdValid());
    vbiDialog->show();
}

// Show the drop out analysis graph
void MainWindow::on_actionDropout_analysis_triggered()
{
    // Show the dropout analysis dialogue
    dropoutAnalysisDialog->show();
}

// Show the visible drop out analysis graph
void MainWindow::on_actionVisible_Dropout_analysis_triggered()
{
    // Show the visible dropout analysis dialogue
    visibleDropoutAnalysisDialog->show();
}

// Show the Black SNR analysis graph
void MainWindow::on_actionSNR_analysis_triggered()
{
    // Show the black SNR analysis dialogue
    blackSnrAnalysisDialog->show();
}

// Show the White SNR analysis graph
void MainWindow::on_actionWhite_SNR_analysis_triggered()
{
    // Show the white SNR analysis dialogue
    whiteSnrAnalysisDialog->show();
}

// Save current frame as PNG with the options last accepted in the options dialog
void MainWindow::on_actionSave_frame_as_PNG_triggered()
{
    tbcDebugStream() << "MainWindow::on_actionSave_frame_as_PNG_triggered(): Called";
    saveFrameAsPng(configuration.getFrameSnapshotOptions());
}

// Choose framing, aspect, best-frame search and upscaling, then save
void MainWindow::on_actionSave_frame_as_PNG_with_options_triggered()
{
    if (!tbcSource.getIsSourceLoaded()) {
        QMessageBox::warning(this, tr("Warning"), tr("No source file loaded."));
        return;
    }
    if (tbcSource.getIsMetadataOnly()) {
        QMessageBox::warning(this, tr("Warning"), tr("Metadata-only mode cannot export PNG images."));
        return;
    }
    setPlaybackRunning(false);

    const QImage frameImage = renderedCurrentFrameImage();
    if (frameImage.isNull()) {
        QMessageBox::warning(this, tr("Warning"), tr("No image data is available to export as PNG."));
        return;
    }

    SavePngDialog dialog(configuration.getFrameSnapshotOptions(), frameImage, tbcSource.getVideoParameters(),
                         tbcSource.getViewMode() == TbcSource::ViewMode::FRAME_VIEW, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const FrameSnapshot::Options options = dialog.selectedOptions();
    configuration.setFrameSnapshotOptions(options);
    configuration.writeConfiguration();
    saveFrameAsPng(options);
}

void MainWindow::saveFrameAsPng(const FrameSnapshot::Options &options)
{
    if (!tbcSource.getIsSourceLoaded()) {
        QMessageBox::warning(this, tr("Warning"), tr("No source file loaded."));
        return;
    }
    if (tbcSource.getIsMetadataOnly()) {
        QMessageBox::warning(this, tr("Warning"), tr("Metadata-only mode cannot export PNG images."));
        return;
    }
    setPlaybackRunning(false);

    // Framing, aspect, search and upscaling apply to the frame view; the other
    // views are saved as they are shown.
    const bool frameView = tbcSource.getViewMode() == TbcSource::ViewMode::FRAME_VIEW;
    const TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    const QSize frameSize(tbcSource.getFrameWidth(), tbcSource.getFrameHeight());

    QImage averagedImage;
    qint32 averagedFrames = 0;
    qint32 alignedFrames = 0;
    if (frameView && options.stillMode != FrameSnapshot::StillMode::Off) {
        FrameSnapshot::SearchInput input;
        input.tbcFilename = tbcSource.getCurrentSourceFilename();
        input.videoParameters = videoParameters;
        input.anchorFrame = currentFrameNumber;
        input.radius = options.searchRadius;
        input.cropRect = FrameSnapshot::outputRect(options, videoParameters, frameSize);
        input.firstFrame = qMax(1, currentFrameNumber - options.searchRadius);
        const qint32 lastFrame = qMin(tbcSource.getNumberOfFrames(), currentFrameNumber + options.searchRadius);
        const QVector<double> visibleDropouts = tbcSource.getVisibleDropOutGraphData();
        for (qint32 frame = input.firstFrame; frame <= lastFrame; frame++) {
            input.fieldNumbers.append(tbcSource.getFieldNumbersForFrame(frame));
            input.visibleDropouts.append(frame - 1 < visibleDropouts.size() ? visibleDropouts[frame - 1] : 0.0);
        }

        std::atomic<bool> cancel(false);
        std::atomic<qint32> progress(0);
        const FrameSnapshot::SearchResult result = waitWithProgress(
            this,
            QtConcurrent::run([input, &cancel, &progress]() {
                return FrameSnapshot::findStillFrames(input, &cancel, &progress);
            }),
            tr("Finding the frames that show this picture..."), &cancel, &progress, 2 * options.searchRadius + 1);
        if (result.cancelled) {
            return;
        }
        if (!result.errorMessage.isEmpty()) {
            QMessageBox::warning(this, tr("Warning"), result.errorMessage);
            return;
        }

        qint32 runLength = 0;
        for (const FrameSnapshot::FrameScore &score : result.scores) {
            if (score.inRun) runLength++;
        }
        tbcDebugStream() << "MainWindow::saveFrameAsPng(): cleanest frame" << result.bestFrame << "of"
                         << result.eligibleFrames.size() << "usable in a run of" << runLength
                         << "around frame" << currentFrameNumber;

        if (options.stillMode == FrameSnapshot::StillMode::Average) {
            // Render each usable frame through the chroma decoder on a worker
            // thread. Nothing else may use tbcSource meanwhile: no async
            // render, and showImage() waits. Dropout highlighting would be
            // painted into every frame, so it is off for the duration.
            cancelInFlightAsyncFrameRender();
            const bool highlightDropouts = tbcSource.getHighlightDropouts();
            tbcSource.setHighlightDropouts(false);
            tbcSourceBusy = true;

            std::atomic<bool> cancelAverage(false);
            std::atomic<qint32> rendered(0);
            const QVector<qint32> frames = result.eligibleFrames;
            const QVector<FrameSnapshot::FrameAlignment> alignments = result.alignments;
            averagedImage = waitWithProgress(
                this,
                QtConcurrent::run([this, frames, alignments, &cancelAverage, &rendered]() {
                    return FrameSnapshot::averageFrames(frames, [this](qint32 frame) {
                        tbcSource.load(frame, frame * 2 - 1);
                        return tbcSource.getImage();
                    }, &cancelAverage, &rendered, alignments);
                }),
                tr("Averaging %1 frames...").arg(frames.size()), &cancelAverage, &rendered, frames.size());

            tbcSourceBusy = false;
            tbcSource.setHighlightDropouts(highlightDropouts);
            // tbcSource now holds the last averaged frame. The jump below
            // redraws the viewer; without a jump, redraw it here.
            showImagePending = false;
            if (result.bestFrame == currentFrameNumber) {
                showImage();
            }

            if (cancelAverage.load()) {
                return;
            }
            if (averagedImage.isNull()) {
                QMessageBox::warning(this, tr("Warning"), tr("No image data is available to export as PNG."));
                return;
            }
            averagedFrames = frames.size();
            for (const FrameSnapshot::FrameAlignment &alignment : alignments) {
                if (alignment.apply) alignedFrames++;
            }
        }

        if (result.bestFrame != currentFrameNumber) {
            setCurrentFrame(result.bestFrame);
            updatePositionEditorValue(currentFrameNumber);
            ui->posHorizontalSlider->setValue(currentFrameNumber);
        }
        if (options.stillMode == FrameSnapshot::StillMode::Average) {
            statusBar()->showMessage(tr("Averaged %1 of %2 frames showing the same picture (%3 realigned); viewer on the most typical, %4")
                                         .arg(averagedFrames).arg(runLength).arg(alignedFrames).arg(result.bestFrame), 10000);
        } else {
            statusBar()->showMessage(tr("Cleanest frame: %1 (of %2 frames showing the same picture)")
                                         .arg(result.bestFrame).arg(runLength), 10000);
        }
    }

    const QImage imageToSave = !averagedImage.isNull() ? averagedImage
                               : frameView ? renderedCurrentFrameImage() : renderedCurrentImageForExport();
    if (imageToSave.isNull()) {
        QMessageBox::warning(this, tr("Warning"), tr("No image data is available to export as PNG."));
        return;
    }

    QString outputDirectory = configuration.getPngDirectory().trimmed();
    if (outputDirectory.isEmpty()) {
        outputDirectory = outputRootDirectoryForCurrentSource();
    }
    if (outputDirectory.isEmpty()) {
        outputDirectory = QDir::homePath();
    }

    // Create a suggestion for the filename
    QString filenameStem;
    switch (tbcSource.getViewMode()) {
    case TbcSource::ViewMode::FRAME_VIEW:
        filenameStem += tr("frame_");
        break;

    case TbcSource::ViewMode::SPLIT_VIEW:
        filenameStem += tr("fields_");
        break;

    case TbcSource::ViewMode::FIELD_VIEW:
        filenameStem += tr("field_");
        break;

    case TbcSource::ViewMode::RGB_SCOPE_VIEW:
        filenameStem += tr("rgb_scope_");
        break;
    }

    if (tbcSource.getSystem() == PAL) filenameStem += tr("pal_");
    else if (tbcSource.getSystem() == PAL_M) filenameStem += tr("palm_");
    else filenameStem += tr("ntsc_");

    if (!tbcSource.getChromaDecoder()) filenameStem += tr("source_");
    else filenameStem += tr("chroma_");

    if (frameView || displayAspectRatio) {
        if (tbcSource.getIsWidescreen()) filenameStem += tr("ar169_");
        else filenameStem += tr("ar43_");
    }

    if (tbcSource.getViewMode() == TbcSource::ViewMode::FIELD_VIEW) {
        filenameStem += QString::number(currentFieldNumber);
    } else {
        filenameStem += QString::number(currentFrameNumber);
    }

    const QString sourceFileName = QFileInfo(tbcSource.getCurrentSourceFilename()).fileName();
    if (!sourceFileName.isEmpty()) {
        filenameStem += QStringLiteral("_");
        filenameStem += sanitizedFileToken(QFileInfo(sourceFileName).completeBaseName());
    }
    if (averagedFrames > 0) {
        filenameStem += QStringLiteral("_avg%1").arg(averagedFrames);
    }
    if (frameView && options.upscaleFactor > 1) {
        filenameStem += QStringLiteral("_up%1x_%2").arg(options.upscaleFactor).arg(sanitizedFileToken(options.upscaleMethod));
    }
    const QString filenameSuggestion =
        QDir(outputDirectory).filePath(filenameStem + tr(".png"));

    QString pngFilename = QFileDialog::getSaveFileName(this,
                tr("Save PNG file"),
                filenameSuggestion,
                tr("PNG image (*.png);;All Files (*)"));

    // Was a filename specified?
    if (pngFilename.isEmpty()) {
        return;
    }
    if (QFileInfo(pngFilename).suffix().isEmpty()) {
        pngFilename += QStringLiteral(".png");
    }

    QImage finalImage = imageToSave;
    if (frameView) {
        QString errorMessage;
        finalImage = waitWithProgress(
            this,
            QtConcurrent::run([imageToSave, options, videoParameters, &errorMessage]() {
                return FrameSnapshot::process(imageToSave, options, videoParameters, &errorMessage);
            }),
            options.upscaleFactor > 1 ? tr("Upscaling...") : tr("Preparing PNG..."));
        if (finalImage.isNull()) {
            QMessageBox::warning(this, tr("Warning"), errorMessage);
            return;
        }
    }

    // Save the current frame as a PNG
    tbcDebugStream() << "MainWindow::saveFrameAsPng(): Saving current frame as" << pngFilename;
    if (!finalImage.save(pngFilename)) {
        tbcDebugStream() << "MainWindow::saveFrameAsPng(): Failed to save file as" << pngFilename;
        QMessageBox::warning(this, tr("Warning"), tr("Could not save a PNG using the specified filename!"));
    }

    // Update the configuration for the PNG directory
    QFileInfo pngFileInfo(pngFilename);
    configuration.setPngDirectory(pngFileInfo.absolutePath());
    tbcDebugStream() << "MainWindow::saveFrameAsPng(): Setting PNG directory to:" << pngFileInfo.absolutePath();
    configuration.writeConfiguration();
}

// Scan a stretch of a slideshow tape for held photos, review them and save each
void MainWindow::on_actionExtract_slideshow_stills_triggered()
{
    if (!tbcSource.getIsSourceLoaded()) {
        QMessageBox::warning(this, tr("Warning"), tr("No source file loaded."));
        return;
    }
    if (tbcSource.getIsMetadataOnly()) {
        QMessageBox::warning(this, tr("Warning"), tr("Metadata-only mode cannot export PNG images."));
        return;
    }
    setPlaybackRunning(false);
    if (slideshowDialog) {
        showOrRaise(slideshowDialog);
        return;
    }

    const TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    const QString sourceFilename = tbcSource.getCurrentSourceFilename();
    SlideshowDialog::Source source;
    source.videoParameters = videoParameters;
    source.frameSize = QSize(tbcSource.getFrameWidth(), tbcSource.getFrameHeight());
    source.frameCount = tbcSource.getNumberOfFrames();
    source.currentFrame = currentFrameNumber;
    source.inPoint = videoParameters.userEditInSelection;
    source.outPoint = videoParameters.userEditOutSelection;
    source.currentFrameImage = renderedCurrentFrameImage();
    source.defaultDirectory = QDir(QFileInfo(sourceFilename).absolutePath())
                                  .filePath(SlideshowExtract::fileStem(sourceFilename) + QStringLiteral("_stills"));

    auto buildInput = [this](qint32 first, qint32 last) {
        SlideshowExtract::CaptureInput input;
        input.scan.tbcFilename = tbcSource.getCurrentSourceFilename();
        input.scan.videoParameters = tbcSource.getVideoParameters();
        input.scan.firstFrame = first;
        const QVector<double> visibleDropouts = tbcSource.getVisibleDropOutGraphData();
        for (qint32 frame = first; frame <= last; frame++) {
            input.scan.fieldNumbers.append(tbcSource.getFieldNumbersForFrame(frame));
            input.visibleDropouts.append(frame - 1 < visibleDropouts.size() ? visibleDropouts[frame - 1] : 0.0);
        }
        return input;
    };

    slideshowDialog = new SlideshowDialog(configuration.getSlideshowExtractOptions(), source, buildInput, this);
    slideshowDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(slideshowDialog, &SlideshowDialog::jumpRequested, this, [this](qint32 frame) {
        setCurrentFrame(frame);
        updatePositionEditorValue(currentFrameNumber);
        ui->posHorizontalSlider->setValue(currentFrameNumber);
    });
    connect(slideshowDialog, &SlideshowDialog::settingsChanged, this, [this](const SlideshowExtractOptions &settings) {
        configuration.setSlideshowExtractOptions(settings);
        configuration.writeConfiguration();
    });
    connect(slideshowDialog, &SlideshowDialog::saveRequested, this, &MainWindow::saveSlideshowStills);
    showOrRaise(slideshowDialog);
}

void MainWindow::saveSlideshowStills(const SlideshowExtract::CaptureInput &input,
                                     const QVector<SlideshowExtract::Hold> &holds, const QString &directory)
{
    QWidget *parent = slideshowDialog ? static_cast<QWidget *>(slideshowDialog) : this;
    // Frames render as the viewer shows them; framing and aspect need whole frames
    if (tbcSource.getViewMode() != TbcSource::ViewMode::FRAME_VIEW) {
        QMessageBox::warning(parent, tr("Warning"), tr("Stills are saved from the Frame view. Switch the viewer to "
                                                       "Frame view, then save again."));
        return;
    }
    const QDir outputDirectory(directory);
    if (!outputDirectory.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(parent, tr("Warning"), tr("Could not create the folder %1.").arg(directory));
        return;
    }
    const QString stem = SlideshowExtract::fileStem(input.scan.tbcFilename);
    const QString manifestName = outputDirectory.filePath(stem + QStringLiteral("_stills.csv"));
    const qint32 existing = outputDirectory.entryList({stem + QStringLiteral("_still_*.png")}, QDir::Files).size();
    if (existing > 0 || QFileInfo::exists(manifestName)) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            parent, tr("Replace stills?"),
            tr("%1 already holds stills from this tape. Saving replaces its list of stills and any file with "
               "the same name; other files stay.").arg(QDir::toNativeSeparators(directory)),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save);
        if (answer != QMessageBox::Save) return;
    }
    setPlaybackRunning(false);

    // The worker renders through tbcSource, as the frame averaging does: no
    // async render meanwhile, showImage() waits, and dropout highlighting
    // (which would be painted into the frames) is off
    cancelInFlightAsyncFrameRender();
    const bool highlightDropouts = tbcSource.getHighlightDropouts();
    tbcSource.setHighlightDropouts(false);
    tbcSourceBusy = true;

    struct SaveOutcome {
        QVector<SlideshowExtract::Still> stills;
        QString errorMessage;
    };
    std::atomic<bool> cancel(false);
    std::atomic<qint32> saved(0);
    const VideoSystem system = input.scan.videoParameters.system;
    const SaveOutcome outcome = waitWithProgress(
        this,
        QtConcurrent::run([this, input, holds, outputDirectory, stem, system, &cancel, &saved]() {
            SaveOutcome outcome;
            auto render = [this](qint32 frame) {
                tbcSource.load(frame, frame * 2 - 1);
                return tbcSource.getImage();
            };
            for (const SlideshowExtract::Hold &hold : holds) {
                if (cancel.load()) break;
                const SlideshowExtract::Capture capture = SlideshowExtract::captureHold(input, hold, render, &cancel);
                if (cancel.load()) break;
                if (capture.image.isNull()) {
                    outcome.errorMessage = capture.errorMessage;
                    break;
                }
                SlideshowExtract::Still still;
                still.index = outcome.stills.size() + 1;
                still.fileName = SlideshowExtract::stillFileName(stem, still.index, capture, input.options);
                still.hold = hold;
                still.captureFrame = capture.frame;
                still.framesAveraged = capture.framesAveraged;
                still.startTimecode = SlideshowExtract::frameTimecode(hold.first, system);
                still.durationSeconds = hold.length() / SlideshowExtract::frameRate(system);
                if (!capture.image.save(outputDirectory.filePath(still.fileName))) {
                    outcome.errorMessage = QStringLiteral("Could not write %1").arg(outputDirectory.filePath(still.fileName));
                    break;
                }
                outcome.stills.append(still);
                saved.fetch_add(1);
            }
            return outcome;
        }),
        holds.size() == 1 ? tr("Saving 1 still...") : tr("Saving %1 stills...").arg(holds.size()), &cancel, &saved,
        holds.size());

    tbcSourceBusy = false;
    tbcSource.setHighlightDropouts(highlightDropouts);
    // tbcSource now holds the last frame rendered; put the viewer's back
    showImagePending = false;
    tbcSource.load(currentFrameNumber, currentFieldNumber);
    showImage();

    // The manifest lists whatever was saved, even when stopped part way
    QString manifestError;
    if (!outcome.stills.isEmpty() && !SlideshowExtract::writeManifest(manifestName, outcome.stills, &manifestError)) {
        QMessageBox::warning(parent, tr("Warning"), manifestError);
    }
    if (!outcome.errorMessage.isEmpty()) {
        QMessageBox::warning(parent, tr("Warning"), outcome.errorMessage);
    }
    const QString summary = tr("Saved %1 of %2 stills to %3")
                                .arg(outcome.stills.size()).arg(holds.size()).arg(QDir::toNativeSeparators(directory));
    if (slideshowDialog) slideshowDialog->setStatus(summary);
    statusBar()->showMessage(summary, 10000);
}

void MainWindow::copyCurrentFrameToClipboard()
{
    if (!tbcSource.getIsSourceLoaded()) {
        statusBar()->showMessage(tr("No source loaded to copy."), 3000);
        return;
    }

    const QImage imageToCopy = renderedCurrentImageForExport();
    if (imageToCopy.isNull()) {
        statusBar()->showMessage(tr("No display image available to copy."), 3000);
        return;
    }

    QClipboard *clipboard = QApplication::clipboard();
    if (!clipboard) {
        statusBar()->showMessage(tr("Clipboard is unavailable."), 3000);
        return;
    }

    clipboard->setImage(imageToCopy);
    statusBar()->showMessage(tr("Copied current frame to clipboard."), 3000);
}

void MainWindow::copyCurrentDisplayToClipboard()
{
    if (!tbcSource.getIsSourceLoaded()) {
        statusBar()->showMessage(tr("No source loaded to copy."), 3000);
        return;
    }

    if (!isViewerTabActive()) {
        QWidget *focus = QApplication::focusWidget();
        if (focus && (qobject_cast<QLineEdit *>(focus)
                      || qobject_cast<QTextEdit *>(focus)
                      || qobject_cast<QPlainTextEdit *>(focus)
                      || qobject_cast<QAbstractSpinBox *>(focus))) {
            QMetaObject::invokeMethod(focus, "copy");
            return;
        }

        statusBar()->showMessage(tr("Switch to the Viewer tab to copy the current display."), 3000);
        return;
    }

    copyCurrentFrameToClipboard();
}

void MainWindow::saveAllModesAsPngs()
{
    if (!tbcSource.getIsSourceLoaded()) {
        QMessageBox::warning(this, tr("Warning"), tr("No source file loaded."));
        return;
    }
    if (tbcSource.getIsMetadataOnly()) {
        QMessageBox::warning(this, tr("Warning"), tr("Metadata-only mode cannot export PNG images."));
        return;
    }

    QMessageBox exportModeDialog(this);
    exportModeDialog.setIcon(QMessageBox::Question);
    exportModeDialog.setWindowTitle(tr("Save all mode views as PNGs"));
    exportModeDialog.setText(tr("Choose what to export:"));
    QAbstractButton *everythingButton = exportModeDialog.addButton(tr("Everything"), QMessageBox::AcceptRole);
    QAbstractButton *imageButton = exportModeDialog.addButton(tr("Image"), QMessageBox::ActionRole);
    QAbstractButton *scopesButton = exportModeDialog.addButton(tr("Scopes"), QMessageBox::ActionRole);
    QAbstractButton *snrGraphsButton = exportModeDialog.addButton(tr("SNR Graphs"), QMessageBox::ActionRole);
    if (QPushButton *defaultButton = qobject_cast<QPushButton *>(everythingButton)) {
        exportModeDialog.setDefaultButton(defaultButton);
    }
    QAbstractButton *cancelButton = exportModeDialog.addButton(QMessageBox::Cancel);
    exportModeDialog.exec();

    const QAbstractButton *selectedExportModeButton = exportModeDialog.clickedButton();
    if (!selectedExportModeButton || selectedExportModeButton == cancelButton) {
        return;
    }

    const bool exportImages =
        (selectedExportModeButton == everythingButton || selectedExportModeButton == imageButton);
    const bool exportScopes =
        (selectedExportModeButton == everythingButton || selectedExportModeButton == scopesButton);
    const bool exportSnrGraphs =
        (selectedExportModeButton == everythingButton || selectedExportModeButton == snrGraphsButton);

    QString exportSelectionToken = QStringLiteral("all_modes");
    if (selectedExportModeButton == imageButton) {
        exportSelectionToken = QStringLiteral("image");
    } else if (selectedExportModeButton == scopesButton) {
        exportSelectionToken = QStringLiteral("scopes");
    } else if (selectedExportModeButton == snrGraphsButton) {
        exportSelectionToken = QStringLiteral("snr_graphs");
    }

    const QString outputRoot = outputRootDirectoryForCurrentSource();
    if (outputRoot.isEmpty()) {
        QMessageBox::warning(this, tr("Warning"), tr("Could not determine output directory."));
        return;
    }

    const QString baseName = outputBaseNameForCurrentSource();
    const QString frameToken = tbcSource.getFieldViewEnabled()
                                   ? QStringLiteral("field_%1").arg(currentFieldNumber, 6, 10, QChar('0'))
                                   : QStringLiteral("frame_%1").arg(currentFrameNumber, 6, 10, QChar('0'));
    const QString outputFolderName = QStringLiteral("%1_%2_%3_pngs")
                                         .arg(baseName, frameToken, exportSelectionToken);
    const QString outputFolderPath = QDir(outputRoot).filePath(outputFolderName);
    if (!QDir().mkpath(outputFolderPath)) {
        QMessageBox::warning(this, tr("Warning"),
                             tr("Could not create output folder:\n%1").arg(outputFolderPath));
        return;
    }

    const bool originalPlaybackRunning = playbackRunning;
    const bool originalDisplayAspectRatio = displayAspectRatio;
    const bool originalMouseMode = (ui && ui->mouseModePushButton) ? ui->mouseModePushButton->isChecked() : false;
    const TbcSource::SourceMode originalSourceMode = tbcSource.getSourceMode();
    const bool originalChromaEnabled = tbcSource.getChromaDecoder();
    const TbcSource::ChromaDecodeMode originalChromaMode = tbcSource.getChromaDecodeMode();
    const TbcSource::ViewMode originalViewMode = tbcSource.getViewMode();
    const bool originalStretchField = tbcSource.getStretchField();
    const qint32 originalFrameNumber = currentFrameNumber;
    const qint32 originalFieldNumber = currentFieldNumber;

    auto restoreState = [&]() {
        tbcSource.setSourceMode(originalSourceMode);
        tbcSource.setChromaDecodeMode(originalChromaMode);
        tbcSource.setChromaDecoder(originalChromaEnabled);
        tbcSource.setViewMode(originalViewMode);
        tbcSource.setStretchField(originalStretchField);
        displayAspectRatio = originalDisplayAspectRatio;
        if (originalViewMode == TbcSource::ViewMode::FIELD_VIEW) {
            setCurrentField(originalFieldNumber);
        } else {
            setCurrentFrame(originalFrameNumber);
        }
        if (ui && ui->mouseModePushButton) {
            ui->mouseModePushButton->setChecked(originalMouseMode);
        }
        updateVideoPushButton();
        updateSourcesPushButton();
        updateAspectPushButton();
        setViewValues();
        showImage();
        setPlaybackRunning(originalPlaybackRunning);
    };

    setPlaybackRunning(false);
    if (ui && ui->mainTabWidget && ui->viewerTab) {
        ui->mainTabWidget->setCurrentWidget(ui->viewerTab);
    }
    if (ui && ui->mouseModePushButton && ui->mouseModePushButton->isChecked()) {
        ui->mouseModePushButton->setChecked(false);
    }

    struct VideoModeSnapshot {
        QString token;
        bool chromaEnabled;
        TbcSource::ChromaDecodeMode chromaMode;
    };
    const QVector<VideoModeSnapshot> videoModes = {
        {QStringLiteral("source"), false, originalChromaMode},
        {QStringLiteral("hybrid"), true, TbcSource::HYBRID_CHROMA_MODE},
        {QStringLiteral("chroma"), true, TbcSource::FULL_FRAME_CHROMA_MODE}
    };

    struct SourceModeSnapshot {
        QString token;
        TbcSource::SourceMode sourceMode;
    };
    QVector<SourceModeSnapshot> sourceModes;
    if (originalSourceMode == TbcSource::ONE_SOURCE) {
        sourceModes.append({QStringLiteral("single"), TbcSource::ONE_SOURCE});
    } else {
        sourceModes.append({QStringLiteral("y"), TbcSource::LUMA_SOURCE});
        sourceModes.append({QStringLiteral("c"), TbcSource::CHROMA_SOURCE});
        sourceModes.append({QStringLiteral("yc"), TbcSource::BOTH_SOURCES});
    }

    struct ViewModeSnapshot {
        QString token;
        TbcSource::ViewMode viewMode;
        bool stretchField;
    };
    QVector<ViewModeSnapshot> viewModes;
    if (exportImages) {
        viewModes.append({QStringLiteral("frame"), TbcSource::ViewMode::FRAME_VIEW, false});
        viewModes.append({QStringLiteral("split"), TbcSource::ViewMode::SPLIT_VIEW, false});
        viewModes.append({QStringLiteral("field_1x"), TbcSource::ViewMode::FIELD_VIEW, false});
        viewModes.append({QStringLiteral("field_2x"), TbcSource::ViewMode::FIELD_VIEW, true});
    }
    if (exportScopes) {
        viewModes.append({QStringLiteral("rgb_scope"), TbcSource::ViewMode::RGB_SCOPE_VIEW, false});
    }

    int savedCount = 0;
    QStringList failedFiles;
    const QString systemToken = (tbcSource.getSystem() == PAL)
                                    ? QStringLiteral("pal")
                                    : ((tbcSource.getSystem() == PAL_M)
                                           ? QStringLiteral("palm")
                                           : QStringLiteral("ntsc"));

    if (!viewModes.isEmpty()) {
        for (const SourceModeSnapshot &sourceModeSnapshot : sourceModes) {
            tbcSource.setSourceMode(sourceModeSnapshot.sourceMode);

            for (const VideoModeSnapshot &videoModeSnapshot : videoModes) {
                if (videoModeSnapshot.chromaEnabled) {
                    tbcSource.setChromaDecodeMode(videoModeSnapshot.chromaMode);
                    tbcSource.setChromaDecoder(true);
                } else {
                    tbcSource.setChromaDecoder(false);
                }

                for (const ViewModeSnapshot &viewModeSnapshot : viewModes) {
                    tbcSource.setViewMode(viewModeSnapshot.viewMode);
                    tbcSource.setStretchField(viewModeSnapshot.stretchField);

                    if (viewModeSnapshot.viewMode == TbcSource::ViewMode::FIELD_VIEW) {
                        setCurrentField(originalFieldNumber);
                    } else {
                        setCurrentFrame(originalFrameNumber);
                    }

                    updateVideoPushButton();
                    updateSourcesPushButton();
                    updateAspectPushButton();
                    setViewValues();
                    showImage();

                    const QImage imageToSave = renderedCurrentImageForExport();
                    const QString outputStem = QStringLiteral("%1_%2_%3_%4_%5")
                                                   .arg(baseName,
                                                        systemToken,
                                                        videoModeSnapshot.token,
                                                        sourceModeSnapshot.token,
                                                        viewModeSnapshot.token);
                    const QString outputFilePath = QDir(outputFolderPath)
                                                       .filePath(sanitizedFileToken(outputStem) + QStringLiteral(".png"));

                    if (imageToSave.isNull() || !imageToSave.save(outputFilePath)) {
                        failedFiles << outputFilePath;
                    } else {
                        savedCount++;
                    }
                }
            }
        }
    }

    restoreState();

    auto saveDialogSnapshot = [&](QDialog *dialog, const QString &token) {
        if (!dialog) {
            return;
        }

        dialog->update();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QPixmap snapshot = dialog->grab();
        if (snapshot.isNull()) {
            const bool wasVisible = dialog->isVisible();
            if (!wasVisible) {
                dialog->show();
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            snapshot = dialog->grab();
            if (!wasVisible) {
                dialog->hide();
            }
        }

        const QString outputStem = QStringLiteral("%1_%2_%3")
                                       .arg(baseName, systemToken, token);
        const QString outputFilePath = QDir(outputFolderPath)
                                           .filePath(sanitizedFileToken(outputStem) + QStringLiteral(".png"));
        if (snapshot.isNull() || !snapshot.save(outputFilePath)) {
            failedFiles << outputFilePath;
        } else {
            savedCount++;
        }
    };

    if (exportScopes || exportSnrGraphs) {
        const bool resumePlayback = playbackRunning;
        setPlaybackRunning(false);

        if (exportScopes) {
            tbcSource.setViewMode(TbcSource::ViewMode::FRAME_VIEW);
            tbcSource.setStretchField(false);
            setCurrentFrame(originalFrameNumber);
            setViewValues();
            showImage();
            const bool originalOscilloscopeAdvancedTab = oscilloscopeDialog->isAdvancedTabSelected();
            QCheckBox *lineYcCheckBox = oscilloscopeDialog->findChild<QCheckBox *>(QStringLiteral("YCcheckBox"));
            QCheckBox *lineYCheckBox = oscilloscopeDialog->findChild<QCheckBox *>(QStringLiteral("YcheckBox"));
            QCheckBox *lineCCheckBox = oscilloscopeDialog->findChild<QCheckBox *>(QStringLiteral("CcheckBox"));
            QCheckBox *lineDropoutsCheckBox = oscilloscopeDialog->findChild<QCheckBox *>(QStringLiteral("dropoutsCheckBox"));

            const bool originalLineYcChecked = lineYcCheckBox ? lineYcCheckBox->isChecked() : false;
            const bool originalLineYChecked = lineYCheckBox ? lineYCheckBox->isChecked() : false;
            const bool originalLineCChecked = lineCCheckBox ? lineCCheckBox->isChecked() : false;
            const bool originalLineDropoutsChecked =
                lineDropoutsCheckBox ? lineDropoutsCheckBox->isChecked() : false;

            const auto setButtonCheckedWithoutSignals = [](QAbstractButton *button, bool checked) {
                if (!button) {
                    return;
                }
                const QSignalBlocker blocker(button);
                button->setChecked(checked);
            };

            struct LineScopePreset {
                QString token;
                bool advanced;
                bool yc;
                bool y;
                bool c;
                bool dropouts;
            };
            const QVector<LineScopePreset> lineScopePresets = {
                {QStringLiteral("line_scope_basic_yc"), false, true, false, false, true},
                {QStringLiteral("line_scope_basic_y"), false, false, true, false, true},
                {QStringLiteral("line_scope_basic_c"), false, false, false, true, true},
                {QStringLiteral("line_scope_basic_yc_y_c"), false, true, true, true, true},
                {QStringLiteral("line_scope_basic_yc_no_dropouts"), false, true, false, false, false},
                {QStringLiteral("line_scope_advanced_yc"), true, true, false, false, true},
                {QStringLiteral("line_scope_advanced_y"), true, false, true, false, true},
                {QStringLiteral("line_scope_advanced_c"), true, false, false, true, true},
                {QStringLiteral("line_scope_advanced_yc_y_c"), true, true, true, true, true}
            };
            for (const LineScopePreset &lineScopePreset : lineScopePresets) {
                setButtonCheckedWithoutSignals(lineYcCheckBox, lineScopePreset.yc);
                setButtonCheckedWithoutSignals(lineYCheckBox, lineScopePreset.y);
                setButtonCheckedWithoutSignals(lineCCheckBox, lineScopePreset.c);
                setButtonCheckedWithoutSignals(lineDropoutsCheckBox, lineScopePreset.dropouts);
                if (lineYcCheckBox && lineYCheckBox && lineCCheckBox
                    && !lineYcCheckBox->isChecked() && !lineYCheckBox->isChecked() && !lineCCheckBox->isChecked()) {
                    setButtonCheckedWithoutSignals(lineYcCheckBox, true);
                }
                if (!oscilloscopeDialog->setAdvancedTabSelected(lineScopePreset.advanced)) {
                    continue;
                }
                updateOscilloscopeDialogue();
                saveDialogSnapshot(oscilloscopeDialog, lineScopePreset.token);
            }

            setButtonCheckedWithoutSignals(lineYcCheckBox, originalLineYcChecked);
            setButtonCheckedWithoutSignals(lineYCheckBox, originalLineYChecked);
            setButtonCheckedWithoutSignals(lineCCheckBox, originalLineCChecked);
            setButtonCheckedWithoutSignals(lineDropoutsCheckBox, originalLineDropoutsChecked);
            oscilloscopeDialog->setAdvancedTabSelected(originalOscilloscopeAdvancedTab);
            updateOscilloscopeDialogue();

            const bool originalVectorscopeAdvancedMode = vectorscopeDialog->isAdvancedRenderModeSelected();
            const bool originalVectorscopeFullAreaMode = vectorscopeDialog->isFullAreaModeSelected();
            const bool originalVectorscopeCustomAreaMode = vectorscopeDialog->isCustomAreaModeSelected();
            const QRect originalVectorscopeCustomAreaRect = vectorscopeDialog->customAreaRect();
            QRadioButton *fieldSelectAllRadioButton =
                vectorscopeDialog->findChild<QRadioButton *>(QStringLiteral("fieldSelectAllRadioButton"));
            QRadioButton *fieldSelectFirstRadioButton =
                vectorscopeDialog->findChild<QRadioButton *>(QStringLiteral("fieldSelectFirstRadioButton"));
            QRadioButton *fieldSelectSecondRadioButton =
                vectorscopeDialog->findChild<QRadioButton *>(QStringLiteral("fieldSelectSecondRadioButton"));
            QRadioButton *graticuleNoneRadioButton =
                vectorscopeDialog->findChild<QRadioButton *>(QStringLiteral("graticuleNoneRadioButton"));
            QRadioButton *graticule75RadioButton =
                vectorscopeDialog->findChild<QRadioButton *>(QStringLiteral("graticule75RadioButton"));
            QRadioButton *graticule100RadioButton =
                vectorscopeDialog->findChild<QRadioButton *>(QStringLiteral("graticule100RadioButton"));
            QCheckBox *vectorscopeDefocusCheckBox =
                vectorscopeDialog->findChild<QCheckBox *>(QStringLiteral("defocusCheckBox"));
            QCheckBox *vectorscopeBlendColorsCheckBox =
                vectorscopeDialog->findChild<QCheckBox *>(QStringLiteral("blendColorCheckBox"));

            const bool originalFieldAllChecked =
                fieldSelectAllRadioButton ? fieldSelectAllRadioButton->isChecked() : false;
            const bool originalFieldFirstChecked =
                fieldSelectFirstRadioButton ? fieldSelectFirstRadioButton->isChecked() : false;
            const bool originalFieldSecondChecked =
                fieldSelectSecondRadioButton ? fieldSelectSecondRadioButton->isChecked() : false;
            const bool originalGraticuleNoneChecked =
                graticuleNoneRadioButton ? graticuleNoneRadioButton->isChecked() : false;
            const bool originalGraticule75Checked =
                graticule75RadioButton ? graticule75RadioButton->isChecked() : false;
            const bool originalGraticule100Checked =
                graticule100RadioButton ? graticule100RadioButton->isChecked() : false;
            const bool originalVectorscopeDefocusChecked =
                vectorscopeDefocusCheckBox ? vectorscopeDefocusCheckBox->isChecked() : false;
            const bool originalVectorscopeBlendChecked =
                vectorscopeBlendColorsCheckBox ? vectorscopeBlendColorsCheckBox->isChecked() : false;

            struct RadioModePreset {
                QString token;
                QRadioButton *radioButton;
            };
            QVector<RadioModePreset> fieldPresets;
            if (fieldSelectAllRadioButton) {
                fieldPresets.append({QStringLiteral("all"), fieldSelectAllRadioButton});
            }
            if (fieldSelectFirstRadioButton) {
                fieldPresets.append({QStringLiteral("field_1"), fieldSelectFirstRadioButton});
            }
            if (fieldSelectSecondRadioButton) {
                fieldPresets.append({QStringLiteral("field_2"), fieldSelectSecondRadioButton});
            }
            if (fieldPresets.isEmpty()) {
                fieldPresets.append({QStringLiteral("current"), nullptr});
            }

            QVector<RadioModePreset> graticulePresets;
            if (graticuleNoneRadioButton) {
                graticulePresets.append({QStringLiteral("none"), graticuleNoneRadioButton});
            }
            if (graticule75RadioButton) {
                graticulePresets.append({QStringLiteral("75"), graticule75RadioButton});
            }
            if (graticule100RadioButton) {
                graticulePresets.append({QStringLiteral("100"), graticule100RadioButton});
            }
            if (graticulePresets.isEmpty()) {
                graticulePresets.append({QStringLiteral("current"), nullptr});
            }

            struct RenderModePreset {
                QString token;
                bool advanced;
            };
            QVector<RenderModePreset> renderModePresets;
            renderModePresets.append({QStringLiteral("point_plot"), false});
            const bool supportsDensityRenderMode = vectorscopeDialog->setAdvancedRenderModeSelected(true);
            vectorscopeDialog->setAdvancedRenderModeSelected(false);
            if (supportsDensityRenderMode) {
                renderModePresets.append({QStringLiteral("density_plot"), true});
            }

            setButtonCheckedWithoutSignals(vectorscopeDefocusCheckBox, false);
            setButtonCheckedWithoutSignals(vectorscopeBlendColorsCheckBox, false);
            vectorscopeDialog->setFullAreaModeSelected(false);
            vectorscopeDialog->setCustomAreaModeSelected(false);
            for (const RadioModePreset &fieldPreset : fieldPresets) {
                setButtonCheckedWithoutSignals(fieldPreset.radioButton, true);
                for (const RadioModePreset &graticulePreset : graticulePresets) {
                    setButtonCheckedWithoutSignals(graticulePreset.radioButton, true);
                    for (const RenderModePreset &renderModePreset : renderModePresets) {
                        if (renderModePreset.advanced
                            && !vectorscopeDialog->setAdvancedRenderModeSelected(true)) {
                            continue;
                        }
                        if (!renderModePreset.advanced) {
                            vectorscopeDialog->setAdvancedRenderModeSelected(false);
                        }
                        updateVectorscopeDialogue();
                        saveDialogSnapshot(vectorscopeDialog,
                                           QStringLiteral("vectorscope_field_%1_graticule_%2_%3")
                                               .arg(fieldPreset.token,
                                                    graticulePreset.token,
                                                    renderModePreset.token));
                    }
                }
            }

            setButtonCheckedWithoutSignals(fieldSelectAllRadioButton, true);
            setButtonCheckedWithoutSignals(graticule75RadioButton, true);
            vectorscopeDialog->setAdvancedRenderModeSelected(false);
            vectorscopeDialog->setFullAreaModeSelected(false);
            vectorscopeDialog->setCustomAreaModeSelected(false);

            if (vectorscopeDialog->setFullAreaModeSelected(true)) {
                updateVectorscopeDialogue();
                saveDialogSnapshot(vectorscopeDialog, QStringLiteral("vectorscope_scope_area_full_frame"));
                vectorscopeDialog->setFullAreaModeSelected(false);
            }
            if (originalVectorscopeCustomAreaMode) {
                if (originalVectorscopeCustomAreaRect.isValid()) {
                    vectorscopeDialog->setCustomAreaRect(originalVectorscopeCustomAreaRect);
                }
                vectorscopeDialog->setCustomAreaModeSelected(true);
                updateVectorscopeDialogue();
                saveDialogSnapshot(vectorscopeDialog, QStringLiteral("vectorscope_scope_area_custom"));
                vectorscopeDialog->setCustomAreaModeSelected(false);
            }
            if (vectorscopeDefocusCheckBox) {
                setButtonCheckedWithoutSignals(vectorscopeDefocusCheckBox, true);
                updateVectorscopeDialogue();
                saveDialogSnapshot(vectorscopeDialog, QStringLiteral("vectorscope_defocus"));
                setButtonCheckedWithoutSignals(vectorscopeDefocusCheckBox, false);
            }
            if (vectorscopeBlendColorsCheckBox) {
                setButtonCheckedWithoutSignals(vectorscopeBlendColorsCheckBox, true);
                updateVectorscopeDialogue();
                saveDialogSnapshot(vectorscopeDialog, QStringLiteral("vectorscope_blend_colors"));
                setButtonCheckedWithoutSignals(vectorscopeBlendColorsCheckBox, false);
            }

            setButtonCheckedWithoutSignals(fieldSelectAllRadioButton, originalFieldAllChecked);
            setButtonCheckedWithoutSignals(fieldSelectFirstRadioButton, originalFieldFirstChecked);
            setButtonCheckedWithoutSignals(fieldSelectSecondRadioButton, originalFieldSecondChecked);
            setButtonCheckedWithoutSignals(graticuleNoneRadioButton, originalGraticuleNoneChecked);
            setButtonCheckedWithoutSignals(graticule75RadioButton, originalGraticule75Checked);
            setButtonCheckedWithoutSignals(graticule100RadioButton, originalGraticule100Checked);
            setButtonCheckedWithoutSignals(vectorscopeDefocusCheckBox, originalVectorscopeDefocusChecked);
            setButtonCheckedWithoutSignals(vectorscopeBlendColorsCheckBox, originalVectorscopeBlendChecked);
            vectorscopeDialog->setAdvancedRenderModeSelected(originalVectorscopeAdvancedMode);
            if (originalVectorscopeCustomAreaRect.isValid()) {
                vectorscopeDialog->setCustomAreaRect(originalVectorscopeCustomAreaRect);
            }
            if (originalVectorscopeCustomAreaMode) {
                vectorscopeDialog->setCustomAreaModeSelected(true);
            } else {
                vectorscopeDialog->setFullAreaModeSelected(originalVectorscopeFullAreaMode);
            }
            updateVectorscopeDialogue();

            updateRgbScopeDialogue(true);
            updateFieldTimingDialogue();
            saveDialogSnapshot(rgbScopeDialog, QStringLiteral("rgb_scope_window"));
            saveDialogSnapshot(fieldTimingDialog, QStringLiteral("field_timing_scope"));
        }

        if (exportSnrGraphs) {
            blackSnrAnalysisDialog->updateFrameMarker(currentFrameNumber);
            whiteSnrAnalysisDialog->updateFrameMarker(currentFrameNumber);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

            saveDialogSnapshot(blackSnrAnalysisDialog, QStringLiteral("black_snr_graph"));
            saveDialogSnapshot(whiteSnrAnalysisDialog, QStringLiteral("white_snr_graph"));
        }

        setPlaybackRunning(resumePlayback);
    }
    configuration.setPngDirectory(outputFolderPath);
    configuration.writeConfiguration();

    if (failedFiles.isEmpty()) {
        QMessageBox::information(this, tr("Save complete"),
                                 tr("Saved %1 PNG files to:\n%2")
                                     .arg(savedCount)
                                     .arg(outputFolderPath));
        return;
    }

    QMessageBox::warning(this, tr("Save partially complete"),
                         tr("Saved %1 PNG files to:\n%2\n\nFailed to save %3 file(s).")
                             .arg(savedCount)
                             .arg(outputFolderPath)
                             .arg(failedFiles.size()));
}

// Zoom in (menu, shortcut and the media bar's zoom-in button)
void MainWindow::on_actionZoom_In_triggered()
{
    constexpr double factor = 1.1;
    if (((scaleFactor * factor) > 0.333) && ((scaleFactor * factor) < 3.0)) {
        scaleFactor *= factor;
    }

    updateImageViewer();
    resize_on_aspect();
}

// Zoom out (menu, shortcut and the media bar's zoom-out button)
void MainWindow::on_actionZoom_Out_triggered()
{
    constexpr double factor = 0.9;
    if (((scaleFactor * factor) > 0.333) && ((scaleFactor * factor) < 3.0)) {
        scaleFactor *= factor;
    }

    updateImageViewer();
    resize_on_aspect();
}

// Original size 1:1 zoom (menu, shortcut and the media bar's 1:1 button)
void MainWindow::on_actionZoom_1x_triggered()
{
    scaleFactor = 1.0;
    updateImageViewer();
    resize_on_aspect();
}

// Build the View -> UI Scale submenu. The zoom actions above size the picture;
// these size the application itself. Qt reads its scale factor from the
// environment while the application is constructed, so a change here applies at
// the next start (see main.cpp) rather than immediately.
void MainWindow::setupUiScaleMenu()
{
    if (!ui || !ui->menuUiScale) {
        return;
    }

    // Factor of 0 means "follow the desktop scale"; the rest are absolute.
    const QVector<QPair<QAction *, double>> scaleActions = {
        { ui->actionUiScaleAuto, 0.0 },  { ui->actionUiScale100, 1.0 },
        { ui->actionUiScale125, 1.25 },  { ui->actionUiScale150, 1.5 },
        { ui->actionUiScale175, 1.75 },  { ui->actionUiScale200, 2.0 },
    };

    QActionGroup *scaleGroup = new QActionGroup(this);
    scaleGroup->setExclusive(true);
    const double currentScale = configuration.getUiScaleFactor();
    for (const auto &entry : scaleActions) {
        if (!entry.first) continue;
        entry.first->setData(entry.second);
        entry.first->setChecked(qFuzzyCompare(entry.second + 1.0, currentScale + 1.0));
        scaleGroup->addAction(entry.first);
    }

    // An operator who exports QT_SCALE_FACTOR keeps control of the scale, and
    // this menu could not report the truth, so it is disabled rather than lying.
    //
    // main() puts the saved factor into QT_SCALE_FACTOR itself, so the variable
    // being set is NOT on its own evidence that the operator set it - treating
    // it that way disabled the menu permanently as soon as any scale was
    // chosen, with no way back to Auto. Only a value we did not write counts.
    const QByteArray environmentScale = qgetenv("QT_SCALE_FACTOR");
    if (!environmentScale.isEmpty()) {
        bool parsed = false;
        const double environmentValue = environmentScale.toDouble(&parsed);
        const bool isOurOwnValue = parsed && currentScale != 0.0
            && qFuzzyCompare(environmentValue + 1.0, currentScale + 1.0);
        if (!isOurOwnValue) {
            ui->menuUiScale->setEnabled(false);
            ui->menuUiScale->setToolTip(tr("The QT_SCALE_FACTOR environment variable is set, "
                                           "and overrides this setting"));
        }
    }

    connect(scaleGroup, &QActionGroup::triggered, this, &MainWindow::handleUiScaleSelected);
}

void MainWindow::handleUiScaleSelected(QAction *action)
{
    if (!action) {
        return;
    }

    const double selectedScale = action->data().toDouble();
    if (qFuzzyCompare(selectedScale + 1.0, configuration.getUiScaleFactor() + 1.0)) {
        return;
    }

    configuration.setUiScaleFactor(selectedScale);
    configuration.writeConfiguration();

    QMessageBox restartBox(this);
    restartBox.setIcon(QMessageBox::Information);
    restartBox.setWindowTitle(tr("UI scale changed"));
    restartBox.setText(tr("The UI scale is applied when tbc-analyse starts."));
    restartBox.setInformativeText(tr("Restart now to use the new scale?"));
    QPushButton *restartButton = restartBox.addButton(tr("Restart now"), QMessageBox::AcceptRole);
    restartBox.addButton(tr("Later"), QMessageBox::RejectRole);
    restartBox.setDefaultButton(restartButton);
    restartBox.exec();
    if (restartBox.clickedButton() != restartButton) {
        return;
    }

    // Never restart out from under unsaved metadata edits
    if (isWindowModified()) {
        QMessageBox::warning(this, tr("Unsaved metadata changes"),
                             tr("This source has unsaved metadata changes, so tbc-analyse was not "
                                "restarted. Save the metadata and restart when you are ready; the "
                                "new UI scale is applied at the next start."));
        return;
    }

    // Carry the loaded source across the restart so the session comes back.
    QStringList arguments;
    const QString currentSource = tbcSource.getCurrentSourceFilename();
    if (!currentSource.isEmpty()) {
        arguments << currentSource;
    }
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments)) {
        QMessageBox::warning(this, tr("Could not restart"),
                             tr("tbc-analyse could not be restarted automatically. The new UI scale "
                                "is applied the next time you start it."));
        return;
    }
    close();
}

// 2:1 zoom menu option
void MainWindow::on_actionZoom_2x_triggered()
{
    scaleFactor = 2.0;
    updateImageViewer();
	MainWindow::resize_on_aspect();
}

// 3:1 zoom menu option
void MainWindow::on_actionZoom_3x_triggered()
{
    scaleFactor = 3.0;
    updateImageViewer();
	MainWindow::resize_on_aspect();
}

// Show closed captions
void MainWindow::on_actionClosed_Captions_triggered()
{
    closedCaptionDialog->show();
}

// Show video parameters dialogue
void MainWindow::on_actionVideo_parameters_triggered()
{
    videoParametersDialog->show();
}

// Show chroma decoder configuration
void MainWindow::on_actionChroma_decoder_configuration_triggered()
{
    chromaDecoderConfigDialog->show();
}

// Toggle chroma during seek option
void MainWindow::on_actionToggleChromaDuringSeek_triggered()
{
    bool enabled = ui->actionToggleChromaDuringSeek->isChecked();
    configuration.setToggleChromaDuringSeek(enabled);
    configuration.writeConfiguration();

}

// Media control frame signal handlers --------------------------------------------------------------------------------

// Previous field/frame button has been clicked
void MainWindow::on_previousPushButton_clicked()
{
    setPlaybackRunning(false);
    // Enter chroma seek mode if appropriate
    enterChromaSeekMode(ui->previousPushButton);
    
    // Normal frame navigation (works the same in both Source and Chroma modes)
    qint32 currentNumber;
    if (tbcSource.getFieldViewEnabled()) {
        setCurrentField(currentFieldNumber - 1);
        currentNumber = currentFieldNumber;
    } else {
        setCurrentFrame(currentFrameNumber - 1);
        currentNumber = currentFrameNumber;
    }

    updatePositionEditorValue(currentNumber);
    ui->posHorizontalSlider->setValue(currentNumber);
}

// Next field/frame button has been clicked
void MainWindow::on_nextPushButton_clicked()
{
    setPlaybackRunning(false);
    // Enter chroma seek mode if appropriate
    enterChromaSeekMode(ui->nextPushButton);
    
    // Normal frame navigation (works the same in both Source and Chroma modes)
    qint32 currentNumber;
    if (tbcSource.getFieldViewEnabled()) {
        setCurrentField(currentFieldNumber + 1);
        currentNumber = currentFieldNumber;
    } else {
        setCurrentFrame(currentFrameNumber + 1);
        currentNumber = currentFrameNumber;
    }

    updatePositionEditorValue(currentNumber);
    ui->posHorizontalSlider->setValue(currentNumber);
}

// Previous button pressed (for chroma toggle during seek)
void MainWindow::on_previousPushButton_pressed()
{
    if (configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder()) {
        // Start timer to detect if this is a hold (not just a click)
        seekTimer->start();
    }
}

// Previous button released (for chroma toggle during seek)
void MainWindow::on_previousPushButton_released()
{
    // Stop the hold detection timer if still running
    seekTimer->stop();
    
    exitChromaSeekMode(ui->previousPushButton);
}

// Next button pressed (for chroma toggle during seek)
void MainWindow::on_nextPushButton_pressed()
{
    if (configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder()) {
        // Start timer to detect if this is a hold (not just a click)
        seekTimer->start();
    }
}

// Next button released (for chroma toggle during seek)
void MainWindow::on_nextPushButton_released()
{
    // Stop the hold detection timer if still running
    seekTimer->stop();
    
    exitChromaSeekMode(ui->nextPushButton);
}

void MainWindow::on_endPushButton_pressed()
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }
    suppressEndButtonClick = false;
    chapterSkipHoldButton = ui->endPushButton;
    chapterSkipHoldDeltaFrames = 10;
    chapterSkipHoldTriggered = false;
    chapterSkipHoldDelayTimer->start();
}

void MainWindow::on_endPushButton_released()
{
    if (chapterSkipHoldButton == ui->endPushButton && chapterSkipHoldTriggered) {
        suppressEndButtonClick = true;
    }
    chapterSkipHoldDelayTimer->stop();
    chapterSkipHoldRepeatTimer->stop();
    chapterSkipHoldButton = nullptr;
    chapterSkipHoldDeltaFrames = 0;
    chapterSkipHoldTriggered = false;
}

void MainWindow::on_startPushButton_pressed()
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }
    suppressStartButtonClick = false;
    chapterSkipHoldButton = ui->startPushButton;
    chapterSkipHoldDeltaFrames = -10;
    chapterSkipHoldTriggered = false;
    chapterSkipHoldDelayTimer->start();
}

void MainWindow::on_startPushButton_released()
{
    if (chapterSkipHoldButton == ui->startPushButton && chapterSkipHoldTriggered) {
        suppressStartButtonClick = true;
    }
    chapterSkipHoldDelayTimer->stop();
    chapterSkipHoldRepeatTimer->stop();
    chapterSkipHoldButton = nullptr;
    chapterSkipHoldDeltaFrames = 0;
    chapterSkipHoldTriggered = false;
}

// Skip to the next chapter (note: this button was repurposed from 'end frame')
void MainWindow::on_endPushButton_clicked()
{
    if (suppressEndButtonClick) {
        suppressEndButtonClick = false;
        return;
    }
    setPlaybackRunning(false);
    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    qint32 targetFrame = currentFrameNumber;
    if (tbcSource.hasChapterMap()) {
        targetFrame = tbcSource.startOfNextChapter(currentFrameNumber);
    } else if (skipBySegmentsEnabled()) {
        targetFrame = totalFrames;
        for (const TbcMetaData::Segment &segment : tbcSource.getSegments()) {
            const qint32 startFrame = segmentStartFrame(segment);
            if (startFrame > currentFrameNumber && startFrame < targetFrame) {
                targetFrame = startFrame;
            }
        }
    } else {
        const QVector<UserNoteMarker> userChapterMarkers = userNoteMarkersFromVideoParameters(
            tbcSource.getVideoParameters(), totalFrames);
        if (!userChapterMarkers.isEmpty()) {
            targetFrame = nextUserMarkerChapterFrame(userChapterMarkers, currentFrameNumber, totalFrames);
        } else {
            targetFrame = qMin(totalFrames, currentFrameNumber + 10);
        }
    }
    setCurrentFrame(targetFrame);
    auto uiNumber = currentFrameNumber;

    if (tbcSource.getFieldViewEnabled()) {
        uiNumber = currentFieldNumber;
    }

    updatePositionEditorValue(uiNumber);
    ui->posHorizontalSlider->setValue(uiNumber);
}

// Skip to the start of chapter (note: this button was repurposed from 'start frame')
void MainWindow::on_startPushButton_clicked()
{
    if (suppressStartButtonClick) {
        suppressStartButtonClick = false;
        return;
    }
    setPlaybackRunning(false);
    qint32 targetFrame = currentFrameNumber;
    if (tbcSource.hasChapterMap()) {
        targetFrame = tbcSource.startOfChapter(currentFrameNumber);
    } else if (skipBySegmentsEnabled()) {
        targetFrame = 1;
        for (const TbcMetaData::Segment &segment : tbcSource.getSegments()) {
            const qint32 startFrame = segmentStartFrame(segment);
            if (startFrame < currentFrameNumber && startFrame > targetFrame) {
                targetFrame = startFrame;
            }
        }
    } else {
        const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
        const QVector<UserNoteMarker> userChapterMarkers = userNoteMarkersFromVideoParameters(
            tbcSource.getVideoParameters(), totalFrames);
        if (!userChapterMarkers.isEmpty()) {
            targetFrame = previousUserMarkerChapterFrame(userChapterMarkers, currentFrameNumber);
        } else {
            targetFrame = qMax<qint32>(1, currentFrameNumber - 10);
        }
    }
    setCurrentFrame(targetFrame);
    auto uiNumber = currentFrameNumber;

    if (tbcSource.getFieldViewEnabled()) {
        uiNumber = currentFieldNumber;
    }

    updatePositionEditorValue(uiNumber);
    ui->posHorizontalSlider->setValue(uiNumber);
}

// Field/Frame number spin box editing has finished
void MainWindow::on_posNumberSpinBox_editingFinished()
{
    setPlaybackRunning(false);
    if (!tbcSource.getIsSourceLoaded() || !tbcSource.getFieldViewEnabled()) {
        return;
    }
    qint32 currentNumber;
    qint32 totalNumber;
    currentNumber = currentFieldNumber;
    totalNumber = tbcSource.getNumberOfFields();

    if (ui->posNumberSpinBox->value() != currentNumber) {
        if (ui->posNumberSpinBox->value() < 1) ui->posNumberSpinBox->setValue(1);
        if (ui->posNumberSpinBox->value() > totalNumber) ui->posNumberSpinBox->setValue(totalNumber);
        setCurrentField(ui->posNumberSpinBox->value());
        currentNumber = currentFieldNumber;

        ui->posHorizontalSlider->setValue(currentNumber);
    }
}

void MainWindow::on_posTimecodeLineEdit_editingFinished()
{
    setPlaybackRunning(false);
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getFieldViewEnabled() || !ui->posTimecodeLineEdit) {
        return;
    }

    bool ok = false;
    qint32 requestedFrame = timecodeToFrame(ui->posTimecodeLineEdit->text(), &ok);
    if (!ok) {
        ui->posTimecodeLineEdit->setText(frameToTimecode(currentFrameNumber));
        statusBar()->showMessage(tr("Invalid timecode. Use HH:MM:SS:FF."), 3000);
        return;
    }

    const qint32 clampedFrame = qBound<qint32>(1, requestedFrame, qMax(1, tbcSource.getNumberOfFrames()));
    if (clampedFrame != currentFrameNumber) {
        setCurrentFrame(clampedFrame);
        ui->posHorizontalSlider->setValue(clampedFrame);
    }
    ui->posTimecodeLineEdit->setText(frameToTimecode(currentFrameNumber));
}

void MainWindow::on_playPushButton_toggled(bool checked)
{
    setPlaybackRunning(checked);
}

// Field/frame slider value has changed
void MainWindow::on_posHorizontalSlider_valueChanged(int value)
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }
    setPlaybackRunning(false);

    if (tbcSource.getFieldViewEnabled()) {
        setCurrentField(value);
        updatePositionEditorValue(currentFieldNumber);
    } else {
        setCurrentFrame(value);
        updatePositionEditorValue(currentFrameNumber);
    }
}

// User started dragging the slider
void MainWindow::on_posHorizontalSlider_sliderPressed()
{
    setPlaybackRunning(false);
}

// User finished dragging the slider - now update
void MainWindow::on_posHorizontalSlider_sliderReleased()
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }
    const qint32 currentNumber = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    updatePositionEditorValue(currentNumber);
}

void MainWindow::on_posHorizontalSlider_customContextMenuRequested(const QPoint &pos)
{
    if (!ui || !ui->posHorizontalSlider || !exportDialog || !tbcSource.getIsSourceLoaded()) {
        return;
    }
    const int clickedSliderValue = sliderValueForContextPoint(ui->posHorizontalSlider, pos);
    int framePoint = frameForSliderPosition(clickedSliderValue);
    const int totalFrames = qMax(1, tbcSource.getNumberOfFrames());
    framePoint = qBound(1, framePoint, totalFrames);
    const QString framePointTimecode = frameToTimecode(framePoint);
    // In/Out point actions operate on the currently displayed frame, not the click position.
    const qint32 currentPlaybackValue = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    const qint32 currentFramePoint = qBound<qint32>(1, frameForSliderPosition(currentPlaybackValue), totalFrames);
    const QString currentFramePointTimecode = frameToTimecode(currentFramePoint);
    const TbcMetaData::VideoParameters currentVideoParameters = tbcSource.getVideoParameters();
    QVector<UserNoteMarker> noteMarkers = userNoteMarkersFromVideoParameters(currentVideoParameters, totalFrames);
    const qint32 noteIndexAtFrame = noteMarkerIndexForFrame(noteMarkers, framePoint);
    const bool noteAtFrameSet = noteIndexAtFrame >= 0;
    const qint32 nearestNoteIndex = nearestNoteMarkerIndexForFrame(noteMarkers, framePoint);
    const qint32 removableNoteIndex = noteAtFrameSet ? noteIndexAtFrame : nearestNoteIndex;
    const bool noteRemovalAvailable = removableNoteIndex >= 0;
    const qint32 removableFrame = noteRemovalAvailable ? noteMarkers.at(removableNoteIndex).frame : 0;
    const QString removableFrameTimecode = noteRemovalAvailable ? frameToTimecode(removableFrame) : QString();
    const QString noteCommentAtFrame = noteAtFrameSet ? noteMarkers.at(noteIndexAtFrame).comment : QString();

    QMenu sliderMenu(this);
    QAction *setInPointAction = sliderMenu.addAction(tr("Set In Point (%1 | %2)")
                                                        .arg(currentFramePoint)
                                                        .arg(currentFramePointTimecode));
    QAction *setOutPointAction = sliderMenu.addAction(tr("Set Out Point (%1 | %2)")
                                                         .arg(currentFramePoint)
                                                         .arg(currentFramePointTimecode));
    sliderMenu.addSeparator();
    QAction *setMarkerAction = sliderMenu.addAction(noteAtFrameSet
                                                        ? tr("Edit Marker (%1 | %2)...")
                                                              .arg(framePoint)
                                                              .arg(framePointTimecode)
                                                        : tr("Add Marker (%1 | %2)...")
                                                              .arg(framePoint)
                                                              .arg(framePointTimecode));
    QAction *removeNoteAction = sliderMenu.addAction(noteRemovalAvailable
                                                         ? (noteAtFrameSet
                                                                ? tr("Remove Marker (%1 | %2)")
                                                                      .arg(removableFrame)
                                                                      .arg(removableFrameTimecode)
                                                                : tr("Remove Nearest Marker (%1 | %2)")
                                                                      .arg(removableFrame)
                                                                      .arg(removableFrameTimecode))
                                                         : tr("Remove Marker"));
    removeNoteAction->setEnabled(noteRemovalAvailable);
    sliderMenu.addSeparator();
    QAction *openNotesViewerAction = sliderMenu.addAction(tr("Open Marker Viewer..."));
    // Recording segments: act on the segment holding the clicked frame
    const qint32 clickedFirstField = tbcSource.firstFieldOfFrame(framePoint);
    const qint32 segmentIndexAtFrame = clickedFirstField >= 0 ? segmentIndexContainingField(clickedFirstField) : -1;
    QAction *setInOutFromSegmentAction = nullptr;
    QAction *splitSegmentAction = nullptr;
    if (!tbcSource.getSegments().isEmpty()) {
        sliderMenu.addSeparator();
        const qint32 segmentId = segmentIndexAtFrame >= 0 ? tbcSource.getSegments().at(segmentIndexAtFrame).id : -1;
        setInOutFromSegmentAction = sliderMenu.addAction(segmentIndexAtFrame >= 0
                                                             ? tr("Set In/Out From Segment %1").arg(segmentId)
                                                             : tr("Set In/Out From Segment"));
        setInOutFromSegmentAction->setEnabled(segmentIndexAtFrame >= 0 && !tbcSource.getIsMetadataOnly());
        splitSegmentAction = sliderMenu.addAction(tr("Split Segment Here (%1 | %2)")
                                                      .arg(framePoint)
                                                      .arg(framePointTimecode));
        splitSegmentAction->setEnabled(segmentIndexAtFrame >= 0
                                       && clickedFirstField > tbcSource.getSegments().at(segmentIndexAtFrame).startField);
    }
    QAction *openSegmentsViewerAction = sliderMenu.addAction(tr("Open Segments Viewer..."));
    QAction *selectedAction = sliderMenu.exec(ui->posHorizontalSlider->mapToGlobal(pos));
    if (!selectedAction) {
        return;
    }
    auto updateMarkerMetadata = [this](const QVector<UserNoteMarker> &updatedNoteMarkers) {
        TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
        if (!applyUserNoteMarkersToVideoParameters(videoParameters, updatedNoteMarkers)) {
            return false;
        }
        tbcSource.setVideoParameters(videoParameters);
        setWindowModified(true);
        updateTimelineMarkers();
        updateNotesViewerState();
        updateSegmentsViewerState();
        updateMetadataStatusPanel();
        return true;
    };
    if (selectedAction == setInPointAction) {
        setInPointAtCurrentFrame();
    } else if (selectedAction == setOutPointAction) {
        setOutPointAtCurrentFrame();
    } else if (selectedAction == setMarkerAction) {
        bool ok = false;
        const QString markerComment = QInputDialog::getText(this,
                                                            noteAtFrameSet ? tr("Edit Marker") : tr("Add Marker"),
                                                            tr("Marker name/comment for frame %1 (%2):")
                                                                .arg(framePoint)
                                                                .arg(framePointTimecode),
                                                            QLineEdit::Normal,
                                                            noteCommentAtFrame,
                                                            &ok);
        if (!ok) {
            return;
        }
        const QString trimmedComment = markerComment.trimmed();
        if (noteAtFrameSet) {
            noteMarkers[noteIndexAtFrame].comment = trimmedComment;
        } else {
            noteMarkers.append({framePoint, trimmedComment});
        }
        noteMarkers = normaliseUserNoteMarkers(noteMarkers, totalFrames);
        if (updateMarkerMetadata(noteMarkers)) {
            statusBar()->showMessage(tr("Marker saved at frame %1 (%2)")
                                         .arg(framePoint)
                                         .arg(framePointTimecode), 3000);
        }
    } else if (selectedAction == removeNoteAction && noteRemovalAvailable) {
        noteMarkers.removeAt(removableNoteIndex);
        noteMarkers = normaliseUserNoteMarkers(noteMarkers, totalFrames);
        if (updateMarkerMetadata(noteMarkers)) {
            statusBar()->showMessage(noteAtFrameSet
                                         ? tr("Marker removed from frame %1 (%2)")
                                               .arg(removableFrame)
                                               .arg(removableFrameTimecode)
                                         : tr("Nearest marker removed from frame %1 (%2)")
                                               .arg(removableFrame)
                                               .arg(removableFrameTimecode), 3000);
        }
    } else if (selectedAction == openNotesViewerAction) {
        updateNotesViewerState();
        updateSegmentsViewerState();
        showOrRaise(notesViewerDialog);
    } else if (setInOutFromSegmentAction && selectedAction == setInOutFromSegmentAction) {
        setInOutFromSegment(segmentIndexAtFrame, true, true);
    } else if (splitSegmentAction && selectedAction == splitSegmentAction) {
        QString splitStatus;
        splitSegmentAtField(clickedFirstField, &splitStatus);
        statusBar()->showMessage(splitStatus, 3000);
        updateSegmentsViewerState();
    } else if (selectedAction == openSegmentsViewerAction) {
        showSegmentsViewer();
    }
}


// Source/Chroma select button clicked
void MainWindow::on_videoPushButton_clicked()
{
    const bool resumePlayback = playbackRunning;
    setPlaybackRunning(false);
    cancelInFlightAsyncFrameRender();
    if (!tbcSource.getChromaDecoder()) {
        tbcSource.setChromaDecodeMode(TbcSource::HYBRID_CHROMA_MODE);
        tbcSource.setChromaDecoder(true);
    } else if (tbcSource.getChromaDecodeMode() == TbcSource::FULL_FRAME_CHROMA_MODE) {
        tbcSource.setChromaDecoder(false);
    } else {
        tbcSource.setChromaDecodeMode(TbcSource::FULL_FRAME_CHROMA_MODE);
    }
    updateVideoPushButton();

    // Show the current image
    showImage();
    if (resumePlayback) {
        setPlaybackRunning(true);
    } else if (ui && ui->playPushButton) {
        ui->playPushButton->setToolTip(playbackStartToolTip());
    }
}

// Aspect ratio button clicked
void MainWindow::on_aspectPushButton_clicked()
{
    displayAspectRatio = !displayAspectRatio;

    // Update the button text
	updateAspectPushButton();

    // Update the image viewer (the scopes don't depend on this)
    updateImageViewer();

	//resize the windows to fit the new size
	resize_on_aspect();
}

void MainWindow::resize_on_aspect()
{
    QPixmap pixmap = ui->imageViewerLabel->pixmap();
    if (pixmap.isNull() || !ui || !ui->scrollArea) {
        return;
    }
    if (this->isFullScreen() || this->isMaximized() || !autoResize) {
        return;
    }

    const QSize viewportSize = ui->scrollArea->viewport()->size();
    if (viewportSize.width() < 1 || viewportSize.height() < 1) {
        return;
    }

    const int nonViewerWidth = this->width() - viewportSize.width();
    const int nonViewerHeight = this->height() - viewportSize.height();
    // The window geometry is in logical pixels, so the frame must contribute its
    // logical size here - its raw pixmap size is device pixels, which on a HiDPI
    // display would ask for a window several times too large.
    const QSize pixmapLogicalSize = pixmap.deviceIndependentSize().toSize();
    QSize targetWindowSize(pixmapLogicalSize.width() + nonViewerWidth,
                           pixmapLogicalSize.height() + nonViewerHeight);
    targetWindowSize = targetWindowSize.expandedTo(this->minimumSize());

    if (QScreen *windowScreen = this->screen()) {
        const QSize maxWindowSize = windowScreen->availableGeometry().size() - QSize(12, 12);
        targetWindowSize = targetWindowSize.boundedTo(maxWindowSize);
    }

    this->resize(targetWindowSize);
}

// Resize the frame to fit within the current window size
void MainWindow::resizeFrameToWindow()
{
	if (!tbcSource.getIsSourceLoaded()) {
		return;
	}
    if (!isViewerTabActive()) {
        return;
    }

	// Get the scroll area size (which contains the imageViewerLabel)
	QScrollArea* scrollArea = ui->scrollArea;
	QSize availableSize = scrollArea->viewport()->size() - QSize(2, 2);
	
	// Ensure we have a valid size - sometimes during resize events the size might be invalid
	if (availableSize.width() <= 0 || availableSize.height() <= 0) {
		// Use the central widget size as fallback, accounting for margins and toolbars
		QSize centralSize = ui->centralWidget->size();
		availableSize = QSize(centralSize.width() - 40, centralSize.height() - 200); // Account for UI elements
	}
	
	// Get the original image size
	QImage originalImage;
	const bool useAsyncRender = shouldRenderFrameAsync();
	if (asyncFrameRenderInProgress && !useAsyncRender) {
		cancelInFlightAsyncFrameRender();
		asyncFrameImage = QImage();
	}
	if (useAsyncRender) {
		originalImage = asyncFrameImage;
		if (originalImage.isNull()) {
			if (!asyncFrameRenderInProgress) {
				startAsyncFrameRender();
			}
			return;
		}
	} else {
		originalImage = tbcSource.getImage();
	}
	if (originalImage.isNull()) {
		return;
	}

	// Calculate scale factor to fit image within available space while maintaining aspect ratio
	qint32 adjustment = getAspectAdjustment();
	double scaleX = static_cast<double>(availableSize.width()) / static_cast<double>(originalImage.width() + adjustment);
	double scaleY = static_cast<double>(availableSize.height()) / static_cast<double>(originalImage.height());
	
	// Use the smaller scale factor to maintain aspect ratio
	double newScaleFactor = qMin(scaleX, scaleY);
	
	// Apply a minimum scale factor to prevent the image from becoming too small
	if (newScaleFactor < 0.1) {
		newScaleFactor = 0.1;
	}
	
	// Only update if there's a significant change to avoid constant tiny adjustments
	if (qAbs(newScaleFactor - scaleFactor) > 0.001) {
		scaleFactor = newScaleFactor;
		updateImageViewer();
	}
}

// Helper method to enter chroma seek mode
void MainWindow::enterChromaSeekMode(QPushButton* button)
{
    if (!chromaSeekMode && !seekTimer->isActive() && configuration.getToggleChromaDuringSeek() && tbcSource.getChromaDecoder() && button->isDown()) {
        chromaSeekMode = true;
        originalChromaState = true;
        tbcSource.setChromaDecoder(false);
        updateVideoPushButton();
    }
}

// Helper method to exit chroma seek mode
void MainWindow::exitChromaSeekMode(QPushButton* button)
{
    // An auto-repeat also emits released(), but with the button still down; a
    // real release has cleared it by then
    if (chromaSeekMode && !button->isDown()) {
        chromaSeekMode = false;
        tbcSource.setChromaDecoder(originalChromaState);
        updateVideoPushButton();
        updateImage(); // Fast refresh without reloading - frame data already loaded
    }
}

// Show/hide dropouts button clicked
void MainWindow::on_dropoutsPushButton_clicked()
{
    tbcSource.setHighlightDropouts(ui->dropoutsPushButton->isChecked());

    // Show the current image (why isn't this option passed?)
    showImage();
}

// Source selection button clicked
void MainWindow::on_sourcesPushButton_clicked()
{
    const bool resumePlayback = playbackRunning;
    setPlaybackRunning(false);
    cancelInFlightAsyncFrameRender();
    switch (tbcSource.getSourceMode()) {
    case TbcSource::ONE_SOURCE:
        // Do nothing - the button's disabled anyway
        break;
    case TbcSource::LUMA_SOURCE:
        tbcSource.setSourceMode(TbcSource::CHROMA_SOURCE);
        break;
    case TbcSource::CHROMA_SOURCE:
        tbcSource.setSourceMode(TbcSource::BOTH_SOURCES);
        break;
    case TbcSource::BOTH_SOURCES:
        tbcSource.setSourceMode(TbcSource::LUMA_SOURCE);
        break;
    }

    // Update the button
    updateSourcesPushButton();

    // Show the current image
    showImage();
    if (resumePlayback) {
        setPlaybackRunning(true);
    } else if (ui && ui->playPushButton) {
        ui->playPushButton->setToolTip(playbackStartToolTip());
    }
}

// Frame/Field view button clicked
void MainWindow::on_viewPushButton_clicked()
{
    switch (tbcSource.getViewMode()) {
        case TbcSource::ViewMode::FRAME_VIEW:
            tbcDebugStream() << "Changing to SPLIT_VIEW mode";

            // Set split mode
            tbcSource.setViewMode(TbcSource::ViewMode::SPLIT_VIEW);
            break;

        case TbcSource::ViewMode::SPLIT_VIEW:
            tbcDebugStream() << "Changing to FIELD_VIEW mode (1:1)";

            // Set field mode with 1:1 aspect
            tbcSource.setViewMode(TbcSource::ViewMode::FIELD_VIEW);
            tbcSource.setStretchField(false);
            break;

        case TbcSource::ViewMode::FIELD_VIEW:
            if (!tbcSource.getStretchField()) {
                tbcDebugStream() << "Changing to FIELD_VIEW mode (2:1)";

                // Set field mode with 2:1 aspect
                tbcSource.setStretchField(true);
            } else {
                tbcDebugStream() << "Changing to FRAME_VIEW mode";

                // Set frame mode
                tbcSource.setViewMode(TbcSource::ViewMode::FRAME_VIEW);
                tbcSource.setStretchField(false);
            }
            break;

        case TbcSource::ViewMode::RGB_SCOPE_VIEW:
            tbcDebugStream() << "Changing to FRAME_VIEW mode";

            // Set frame mode
            tbcSource.setViewMode(TbcSource::ViewMode::FRAME_VIEW);
            break;
    }

    setViewValues();
    updateGuiLoaded();

    // Show the current image
    showImage();
}

// Normal/Reverse field order button clicked
void MainWindow::on_fieldOrderPushButton_clicked()
{
    tbcSource.setFieldOrder(ui->fieldOrderPushButton->isChecked());

    // If the TBC field order is changed, the number of available frames can change, so we need to update the GUI
    resetGui();
    updateGuiLoaded();

    // Show the current image
    showImage();
}

void MainWindow::on_toggleAutoResize_toggled(bool checked)
{
	autoResize = checked;
}

void MainWindow::on_actionResizeFrameWithWindow_toggled(bool checked)
{
	resizeFrameWithWindow = checked;
	
	// Save the setting to configuration
	configuration.setResizeFrameWithWindow(checked);
	configuration.writeConfiguration();
	
	// If resizeFrameWithWindow is now enabled, resize frame to fit current window
	if (checked && tbcSource.getIsSourceLoaded()) {
		resizeTimer->start();
	}
}


// Mouse mode button clicked
void MainWindow::on_mouseModePushButton_clicked()
{
    if (ui->mouseModePushButton->isChecked()) {
        exportBoundaryDragHandle = ExportBoundaryHandle::None;
        exportBoundarySelectedHandle = ExportBoundaryHandle::None;
        if (vectorscopeSelectionPushButton && vectorscopeSelectionPushButton->isChecked()) {
            vectorscopeSelectionPushButton->setChecked(false);
        }
        // Show the oscilloscope view if currently hidden
        if (!oscilloscopeDialog->isVisible()) {
            oscilloscopeDialog->show();
        }
        updateOscilloscopeDialogue();
    }

    // Update the image viewer to display/hide the indicator line
    updateImageViewer();
}

void MainWindow::onVectorscopeSelectionToggled(bool checked)
{
    vectorscopeSelectionDragging = false;
    if (checked) {
        exportBoundaryDragHandle = ExportBoundaryHandle::None;
        exportBoundarySelectedHandle = ExportBoundaryHandle::None;
    }
    if (checked && ui->mouseModePushButton && ui->mouseModePushButton->isChecked()) {
        ui->mouseModePushButton->setChecked(false);
    }
    if (vectorscopeDialog) {
        vectorscopeDialog->setCustomAreaModeSelected(checked);
    }
    if (tbcSource.getIsSourceLoaded()) {
        updateVectorscopeDialogue();
    }

    updateImageViewer();
}

// Miscellaneous handler methods --------------------------------------------------------------------------------------

// Handler called when another class changes the currently selected scan line
void MainWindow::scopeCoordsChangedSignalHandler(qint32 xCoord, qint32 yCoord)
{
    tbcDebugStream() << "MainWindow::scanLineChangedSignalHandler(): Called with xCoord =" << xCoord << "and yCoord =" << yCoord;
    if (!tbcSource.getIsSourceLoaded() || !isViewerTabActive()) {
        return;
    }

    // Show the oscilloscope dialogue for the selected scan-line
    lastScopeDot = xCoord;
    lastScopeLine = yCoord + 1;
    if (!oscilloscopeDialog->isVisible()) {
        oscilloscopeDialog->show();
    }
    updateOscilloscopeDialogue();

    // Update the image viewer
    updateImageViewer();
}

// Handler called when vectorscope settings are changed
void MainWindow::vectorscopeChangedSignalHandler()
{
    tbcDebugStream() << "MainWindow::vectorscopeChangedSignalHandler(): Called";
    if (vectorscopeDialog && vectorscopeSelectionPushButton) {
        const bool customAreaSelected = vectorscopeDialog->isCustomAreaModeSelected();
        if (vectorscopeSelectionPushButton->isChecked() != customAreaSelected) {
            const QSignalBlocker blocker(vectorscopeSelectionPushButton);
            vectorscopeSelectionPushButton->setChecked(customAreaSelected);
        }
        if (!customAreaSelected) {
            vectorscopeSelectionDragging = false;
        }
    }

    if (tbcSource.getIsSourceLoaded()) {
        // Update the vectorscope
        updateVectorscopeDialogue();
        updateImageViewer();
    }
}

// The viewer's key shortcuts (Edit menu, Marker and Segments viewers) apply
// only while the viewer tab is showing
void MainWindow::updateViewerKeyShortcuts()
{
    const bool viewerShowing = ui->mainTabWidget->currentWidget() == ui->viewerTab;
    for (const auto &entry : std::as_const(viewerKeyActions)) {
        entry.first->setShortcuts(viewerShowing ? entry.second : QList<QKeySequence>());
    }
}

// Add a marker comment at the current frame, or edit the one already there
void MainWindow::addOrEditMarkerAtCurrentFrame()
{
    if (!tbcSource.getIsSourceLoaded()) {
        return;
    }

    const qint32 playbackPositionValue = tbcSource.getFieldViewEnabled() ? currentFieldNumber : currentFrameNumber;
    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    const qint32 framePoint = qBound<qint32>(1, frameForSliderPosition(playbackPositionValue), totalFrames);
    const QString framePointTimecode = frameToTimecode(framePoint);

    QVector<UserNoteMarker> noteMarkers = userNoteMarkersFromVideoParameters(tbcSource.getVideoParameters(), totalFrames);
    const qint32 noteIndexAtFrame = noteMarkerIndexForFrame(noteMarkers, framePoint);
    const bool noteAtFrameSet = noteIndexAtFrame >= 0;
    const QString noteCommentAtFrame = noteAtFrameSet ? noteMarkers.at(noteIndexAtFrame).comment : QString();

    // M opens the Add/Edit Marker comment dialog at the current frame.
    bool ok = false;
    const QString markerComment = QInputDialog::getText(this,
                                                        noteAtFrameSet ? tr("Edit Marker") : tr("Add Marker"),
                                                        tr("Marker name/comment for frame %1 (%2):")
                                                            .arg(framePoint)
                                                            .arg(framePointTimecode),
                                                        QLineEdit::Normal,
                                                        noteCommentAtFrame,
                                                        &ok);
    if (!ok) {
        return;
    }

    const QString trimmedComment = markerComment.trimmed();
    if (noteAtFrameSet) {
        noteMarkers[noteIndexAtFrame].comment = trimmedComment;
    } else {
        noteMarkers.append({framePoint, trimmedComment});
    }
    noteMarkers = normaliseUserNoteMarkers(noteMarkers, totalFrames);

    TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    if (applyUserNoteMarkersToVideoParameters(videoParameters, noteMarkers)) {
        tbcSource.setVideoParameters(videoParameters);
        setWindowModified(true);
        updateTimelineMarkers();
        updateNotesViewerState();
        updateSegmentsViewerState();
        updateMetadataStatusPanel();
        statusBar()->showMessage(tr("Marker saved at frame %1 (%2)")
                                     .arg(framePoint)
                                     .arg(framePointTimecode), 3000);
    }
}

// Mouse press event handler
void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (!event) {
        return;
    }
    if (!tbcSource.getIsSourceLoaded() || !isViewerTabActive()) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    // Get the mouse position relative to our scene
    QPoint origin = ui->imageViewerLabel->mapFromGlobal(event->globalPosition().toPoint());

    // Check that the mouse click is within bounds of the current picture
    qint32 oX = origin.x();
    qint32 oY = origin.y();

    if (oX + 1 >= 0 &&
            oY >= 0 &&
            oX + 1 <= ui->imageViewerLabel->width() &&
            oY <= ui->imageViewerLabel->height()) {
        if (vectorscopeSelectionPushButton
            && vectorscopeSelectionPushButton->isChecked()
            && vectorscopeDialog
            && event->button() == Qt::LeftButton) {
            qint32 sourceX = 0;
            qint32 sourceY = 0;
            if (mapViewerToSourceCoordinates(origin, sourceX, sourceY)) {
                vectorscopeSelectionDragging = true;
                vectorscopeSelectionAnchor = QPoint(sourceX, sourceY);
                vectorscopeDialog->setCustomAreaModeSelected(true);
                vectorscopeDialog->setCustomAreaRect(QRect(sourceX, sourceY, 1, 1));
                updateImageViewer();
                updateVectorscopeDialogue();
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && isExportBoundaryDragAvailable()) {
            const ExportBoundaryHandle dragHandle = exportBoundaryHandleAtViewerPoint(origin);
            if (dragHandle != ExportBoundaryHandle::None) {
                exportBoundarySelectedHandle = dragHandle;
                exportBoundaryDragHandle = dragHandle;
                applyExportBoundaryDragAtViewerPoint(origin);
                updateExportBoundaryHoverCursor(origin);
                event->accept();
                return;
            }
        }
        if (event->button() == Qt::LeftButton) {
            exportBoundarySelectedHandle = ExportBoundaryHandle::None;
            exportBoundaryDragHandle = ExportBoundaryHandle::None;
        }
        if (exportBoundaryDragHandle == ExportBoundaryHandle::None) {
            updateExportBoundaryHoverCursor(origin);
        }

        mouseScanLineSelect(oX, oY);
        event->accept();
        return;
    }

    QMainWindow::mousePressEvent(event);
}

// Mouse move event
void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!event) {
        return;
    }
    if (!tbcSource.getIsSourceLoaded() || !isViewerTabActive()) {
        QMainWindow::mouseMoveEvent(event);
        return;
    }

    // Get the mouse position relative to our scene
    QPoint origin = ui->imageViewerLabel->mapFromGlobal(event->globalPosition().toPoint());
    if (exportBoundaryDragHandle != ExportBoundaryHandle::None
        && !(event->buttons() & Qt::LeftButton)) {
        exportBoundaryDragHandle = ExportBoundaryHandle::None;
    }
    if (exportBoundaryDragHandle != ExportBoundaryHandle::None
        && (event->buttons() & Qt::LeftButton)) {
        applyExportBoundaryDragAtViewerPoint(origin);
        updateExportBoundaryHoverCursor(origin);
        event->accept();
        return;
    }

    // Check that the mouse click is within bounds of the current picture
    qint32 oX = origin.x();
    qint32 oY = origin.y();

    if (oX + 1 >= 0 &&
            oY >= 0 &&
            oX + 1 <= ui->imageViewerLabel->width() &&
            oY <= ui->imageViewerLabel->height()) {
        if (vectorscopeSelectionDragging && vectorscopeDialog) {
            qint32 sourceX = 0;
            qint32 sourceY = 0;
            if (mapViewerToSourceCoordinates(origin, sourceX, sourceY)) {
                const qint32 left = qMin(vectorscopeSelectionAnchor.x(), sourceX);
                const qint32 right = qMax(vectorscopeSelectionAnchor.x(), sourceX);
                const qint32 top = qMin(vectorscopeSelectionAnchor.y(), sourceY);
                const qint32 bottom = qMax(vectorscopeSelectionAnchor.y(), sourceY);
                vectorscopeDialog->setCustomAreaRect(QRect(left, top, (right - left) + 1, (bottom - top) + 1));
                updateImageViewer();
                updateVectorscopeDialogue();
            }
            event->accept();
            return;
        }
        if (exportBoundaryDragHandle == ExportBoundaryHandle::None) {
            updateExportBoundaryHoverCursor(origin);
        }

        mouseScanLineSelect(oX, oY);
        event->accept();
        return;
    }

    if (!vectorscopeSelectionDragging) {
        clearCursorReadout();
        if (exportBoundaryDragHandle == ExportBoundaryHandle::None) {
            updateExportBoundaryHoverCursor(QPoint(-1, -1));
        }
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!event) {
        return;
    }
    if (!tbcSource.getIsSourceLoaded() || !isViewerTabActive()) {
        QMainWindow::mouseReleaseEvent(event);
        return;
    }

    if (vectorscopeSelectionDragging && event->button() == Qt::LeftButton) {
        vectorscopeSelectionDragging = false;
        QPoint origin = ui->imageViewerLabel->mapFromGlobal(event->globalPosition().toPoint());
        qint32 sourceX = 0;
        qint32 sourceY = 0;
        if (vectorscopeDialog && mapViewerToSourceCoordinates(origin, sourceX, sourceY)) {
            const qint32 left = qMin(vectorscopeSelectionAnchor.x(), sourceX);
            const qint32 right = qMax(vectorscopeSelectionAnchor.x(), sourceX);
            const qint32 top = qMin(vectorscopeSelectionAnchor.y(), sourceY);
            const qint32 bottom = qMax(vectorscopeSelectionAnchor.y(), sourceY);
            vectorscopeDialog->setCustomAreaModeSelected(true);
            vectorscopeDialog->setCustomAreaRect(QRect(left, top, (right - left) + 1, (bottom - top) + 1));
            updateVectorscopeDialogue();
        }
        updateImageViewer();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton
        && exportBoundaryDragHandle != ExportBoundaryHandle::None) {
        QPoint origin = ui->imageViewerLabel->mapFromGlobal(event->globalPosition().toPoint());
        applyExportBoundaryDragAtViewerPoint(origin);
        exportBoundaryDragHandle = ExportBoundaryHandle::None;
        updateExportBoundaryHoverCursor(origin);
        event->accept();
        return;
    }

    QMainWindow::mouseReleaseEvent(event);
}

// Perform mouse based scan line selection
void MainWindow::mouseScanLineSelect(qint32 oX, qint32 oY)
{
    if (!isViewerTabActive()) {
        return;
    }
    const QPoint viewerPoint(oX, oY);
    updateCursorReadout(viewerPoint);

    qint32 sourceX = 0;
    qint32 sourceY = 0;
    if (!mapViewerToSourceCoordinates(viewerPoint, sourceX, sourceY)) {
        return;
    }

    // Show the oscilloscope dialogue for the selected scan-line (if the right mouse mode is selected)
    if (ui->mouseModePushButton->isChecked()) {
        // Remember the last line rendered
        lastScopeLine = qBound<qint32>(1, sourceY + 1, tbcSource.getFrameHeight());
        lastScopeDot = sourceX;
        if (!oscilloscopeDialog->isVisible()) {
            oscilloscopeDialog->show();
        }

        updateOscilloscopeDialogue();

        // Update the image viewer
        updateImageViewer();
    }
}

// Handle parameters changed signal from the video parameters dialogue
void MainWindow::videoParametersChangedSignalHandler(const TbcMetaData::VideoParameters &videoParameters)
{
    cancelInFlightAsyncFrameRender();
    // Update the VideoParameters in the source
    tbcSource.setVideoParameters(videoParameters);
    if (exportDialog) {
        exportDialog->refreshResolutionOptions();
    }

    // Enable the "Save Metadata" action, since the metadata has been modified
    setWindowModified(true);

    // Update the aspect button's label
    updateAspectPushButton();

    // Update the image viewer
    updateImage();
    updateVideoPushButton();
    updateTimelineMarkers();
    updateNotesViewerState();
    updateSegmentsViewerState();

    updateMetadataStatusPanel();
}
void MainWindow::videoLevelsChangedSignalHandler(qint32 blackLevel, qint32 whiteLevel)
{
    TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    videoParameters.black16bIre = blackLevel;
    videoParameters.white16bIre = whiteLevel;
    videoParametersChangedSignalHandler(videoParameters);
}
void MainWindow::exportRangeSelectionChangedSignalHandler(int inPoint, int outPoint, bool clearMetadataValues)
{
    if (!tbcSource.getIsSourceLoaded() || tbcSource.getIsMetadataOnly()) {
        return;
    }

    const qint32 totalFrames = qMax<qint32>(1, tbcSource.getNumberOfFrames());
    qint32 clampedIn = qBound<qint32>(1, inPoint, totalFrames);
    qint32 clampedOut = qBound<qint32>(1, outPoint, totalFrames);
    if (clampedOut < clampedIn) {
        clampedOut = clampedIn;
    }

    const qint32 metadataIn = clearMetadataValues ? -1 : clampedIn;
    const qint32 metadataOut = clearMetadataValues ? -1 : clampedOut;

    TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    if (videoParameters.userEditInSelection == metadataIn
        && videoParameters.userEditOutSelection == metadataOut) {
        return;
    }

    videoParameters.userEditInSelection = metadataIn;
    videoParameters.userEditOutSelection = metadataOut;
    tbcSource.setVideoParameters(videoParameters);

    setWindowModified(true);
    updateTimelineMarkers();
    updateNotesViewerState();
    updateSegmentsViewerState();
    updateMetadataStatusPanel();
}

void MainWindow::exportBoundaryToggledSignalHandler(bool enabled)
{
    showExportBoundary = enabled;
    if (!enabled) {
        exportBoundaryDragHandle = ExportBoundaryHandle::None;
        exportBoundarySelectedHandle = ExportBoundaryHandle::None;
        updateExportBoundaryHoverCursor(QPoint(-1, -1));
    }
    configuration.setShowExportBoundary(enabled);
    configuration.writeConfiguration();

    updateImageViewer();
}

void MainWindow::exportBoundaryThicknessChangedSignalHandler(int thickness)
{
    exportBoundaryThickness = thickness;
    configuration.setExportBoundaryThickness(thickness);
    configuration.writeConfiguration();

    updateImageViewer();
}

// Handle configuration changed signal from the chroma decoder configuration dialogue
void MainWindow::chromaDecoderConfigChangedSignalHandler()
{
    const bool resumePlayback = playbackRunning;
    setPlaybackRunning(false);
    cancelInFlightAsyncFrameRender();
    const PalColour::Configuration &palConfig = chromaDecoderConfigDialog->getPalConfiguration();
    const Comb::Configuration &ntscConfig = chromaDecoderConfigDialog->getNtscConfiguration();
    // Set the new configuration
    tbcSource.setChromaConfiguration(palConfig, ntscConfig);
    tbcSource.setSecamPredemodFirstLineIsRedOverride(chromaDecoderConfigDialog->getSecamPredemodFirstLineIsRedOverride());

    TbcMetaData::VideoParameters videoParameters = tbcSource.getVideoParameters();
    videoParameters.chromaGain = palConfig.chromaGain;
    videoParameters.chromaPhase = palConfig.chromaPhase;
    videoParameters.lumaNR = (tbcSource.getSystem() == NTSC) ? ntscConfig.yNRLevel : palConfig.yNRLevel;
    videoParameters.ntscAdaptive = ntscConfig.adaptive ? 1 : 0;
    videoParameters.ntscAdaptThreshold = ntscConfig.adaptThreshold;
    videoParameters.ntscChromaWeight = ntscConfig.chromaWeight;
    videoParameters.ntscPhaseCompensation = ntscConfig.phaseCompensation ? 1 : 0;
    videoParameters.palTransformThreshold = palConfig.transformThreshold;

    const QString decoderName = chromaDecoderNameFromConfig(tbcSource.getSystem(), palConfig, ntscConfig);
    if (!decoderName.isEmpty()) {
        videoParameters.chromaDecoder = decoderName;
    }

    tbcSource.setVideoParameters(videoParameters);

    // Enable the \"Save Metadata\" action, since the metadata has been modified
    setWindowModified(true);

    // Update the image viewer
    updateImage();
    updateVideoPushButton();
    if (resumePlayback) {
        setPlaybackRunning(true);
    } else if (ui && ui->playPushButton) {
        ui->playPushButton->setToolTip(playbackStartToolTip());
    }

    updateMetadataStatusPanel();
}

// TbcSource class signal handlers ------------------------------------------------------------------------------------

// Signal handler for busy signal from TbcSource class
void MainWindow::onSourceBusy(QString infoMessage)
{
    setPlaybackRunning(false);
    tbcDebugStream() << "MainWindow::onSourceBusy(): Got signal with message" << infoMessage;
    sourceOperationInProgress = true;
    // The Show event filter centres it over the main window
    busyProgress->setLabelText(infoMessage);
    if (!busyProgress->isVisible()) {
        busyProgress->show();
    }
}

// Signal handler for finishedLoading signal from TbcSource class
void MainWindow::onSourceLoaded(bool success)
{
    tbcDebugStream() << "MainWindow::onSourceLoaded(): Called";
    setPlaybackRunning(false);
    sourceOperationInProgress = false;
    busyProgress->hide();

    // Ensure source loaded ok
    if (success) {
        // Generate the graph data
        dropoutAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());
        visibleDropoutAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());
        blackSnrAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());
        whiteSnrAnalysisDialog->startUpdate(tbcSource.getNumberOfFrames());

        QVector<double> doGraphData = tbcSource.getDropOutGraphData();
        QVector<double> visibleDoGraphData = tbcSource.getVisibleDropOutGraphData();
        QVector<double> blackSnrGraphData = tbcSource.getBlackSnrGraphData();
        QVector<double> whiteSnrGraphData = tbcSource.getWhiteSnrGraphData();

        for (qint32 frameNumber = 0; frameNumber < tbcSource.getNumberOfFrames(); frameNumber++) {
            dropoutAnalysisDialog->addDataPoint(frameNumber + 1, doGraphData[frameNumber]);
            visibleDropoutAnalysisDialog->addDataPoint(frameNumber + 1, visibleDoGraphData[frameNumber]);
            blackSnrAnalysisDialog->addDataPoint(frameNumber + 1, blackSnrGraphData[frameNumber]);
            whiteSnrAnalysisDialog->addDataPoint(frameNumber + 1, whiteSnrGraphData[frameNumber]);
        }

        dropoutAnalysisDialog->finishUpdate(currentFrameNumber);
        visibleDropoutAnalysisDialog->finishUpdate(currentFrameNumber);
        blackSnrAnalysisDialog->finishUpdate(currentFrameNumber);
        whiteSnrAnalysisDialog->finishUpdate(currentFrameNumber);

        // Update the GUI
        resetGui();
        updateGuiLoaded();
        if (restoreUiStateAfterReload) {
            applyUiStateSnapshot(pendingUiStateSnapshot);
            pendingUiStateSnapshot = UiStateSnapshot();
            restoreUiStateAfterReload = false;
        } else if (ui && ui->mainTabWidget && ui->viewerTab) {
            ui->mainTabWidget->setCurrentWidget(ui->viewerTab);
        }
        if (exportDialog) {
            exportDialog->setSource(&tbcSource);
        }

        // The window's file (macOS proxy icon) and title; [*] shows the
        // modified marker while there are unsaved metadata edits
        setWindowFilePath(QFileInfo(lastFilename).absoluteFilePath());
        setWindowTitle(tr("%1[*] - tbc-analyse").arg(tbcSource.getCurrentSourceFilename()));

        // Update the configuration for the source directory
        QFileInfo inFileInfo(tbcSource.getCurrentSourceFilename());
        configuration.setSourceDirectory(inFileInfo.absolutePath());
        tbcDebugStream() << "MainWindow::loadTbcFile(): Setting source directory to:" << inFileInfo.absolutePath();
        configuration.writeConfiguration();
        applyEfmHandlerAutoloads(inFileInfo.absolutePath());
    } else {
        // Load failed
        updateGuiUnloaded();
        pendingUiStateSnapshot = UiStateSnapshot();
        restoreUiStateAfterReload = false;

        // Show the error to the user
        QMessageBox::warning(this, tr("Error"), tbcSource.getLastIOError());
    }

    processPendingSourceOpenRequest();
}

// Signal handler for finishedSaving signal from TbcSource class
void MainWindow::onSourceSaved(bool success)
{
    tbcDebugStream() << "MainWindow::onSourceSaved(): Called";
    sourceOperationInProgress = false;
    busyProgress->hide();

    if (success) {
        setWindowModified(false);

        // A save chosen at maybeSave()'s prompt: carry on with what was asked
        // for (close, open, reprocess) instead of reloading this source.
        if (afterSaveAction) {
            QTimer::singleShot(0, this, std::exchange(afterSaveAction, {}));
            updateMetadataStatusPanel();
            return;
        }

        // Reload the source with the newly-saved metadata so the GUI reflects
        // the edited values (e.g. TV system change, chroma decoder switch).
        if (tbcSource.getIsSourceLoaded()) {
            pendingUiStateSnapshot = captureUiStateSnapshot();
            restoreUiStateAfterReload = true;
            const QString currentSource = tbcSource.getCurrentSourceFilename();
            const QString currentMetadata = tbcSource.getCurrentMetadataFilename();
            if (!currentSource.isEmpty()) {
                loadTbcFile(currentSource, false, true);
            } else if (!currentMetadata.isEmpty()) {
                loadTbcFile(currentMetadata, true, true);
            }
        }
    } else {
        // Whatever was waiting on the save doesn't happen; the edits stay.
        afterSaveAction = {};
        // Show the error to the user
        QMessageBox::warning(this, tr("Error"), tbcSource.getLastIOError());
    }

    updateMetadataStatusPanel();

    processPendingSourceOpenRequest();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

	// Resize frame with window if resizeFrameWithWindow is enabled
	if (resizeFrameWithWindow && tbcSource.getIsSourceLoaded()) {
		resizeTimer->start();
	}
}
