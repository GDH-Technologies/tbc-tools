#include "metadataconverterutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include "gui/processprogressrunner.h"

namespace {
QString normalizeForPathParsing(const QString &path)
{
    QString normalized = path.trimmed();
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QDir::cleanPath(normalized);
}
QString resolveToolPath(const QStringList &toolNames)
{
    for (const QString &name : toolNames) {
        const QString toolPath = QStandardPaths::findExecutable(name);
        if (!toolPath.isEmpty()) {
            return toolPath;
        }
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidateDirs = {
        QStringLiteral("."),
        QStringLiteral(".."),
        QStringLiteral("../bin"),
        QStringLiteral("../../bin")
    };

    for (const QString &dir : candidateDirs) {
        for (const QString &name : toolNames) {
            const QString localCandidate = QDir(appDir).filePath(dir + QLatin1Char('/') + name);
            if (QFileInfo::exists(localCandidate)) {
                return localCandidate;
            }
        }
    }

    return QString();
}

MetadataConverterUtil::MetadataConversionDirection metadataDirectionFromString(const QString &direction)
{
    const QString directionValue = direction.trimmed().toLower();
    if (directionValue == QLatin1String("json-to-sqlite")) {
        return MetadataConverterUtil::MetadataConversionDirection::JsonToSqlite;
    }
    if (directionValue == QLatin1String("sqlite-to-json")) {
        return MetadataConverterUtil::MetadataConversionDirection::SqliteToJson;
    }
    return MetadataConverterUtil::MetadataConversionDirection::Unknown;
}

QString outputPathWithSuffix(const QFileInfo &info, const QString &fallbackInput, const QString &suffix)
{
    const QString baseName = info.completeBaseName();
    if (baseName.isEmpty()) {
        return fallbackInput + suffix;
    }

    const QString path = info.path();
    if (path.isEmpty() || path == QLatin1String(".")) {
        return baseName + suffix;
    }
    return path + QLatin1Char('/') + baseName + suffix;
}
} // namespace

namespace MetadataConverterUtil {
QString normalizePathForCurrentPlatform(const QString &path)
{
    const QString normalized = normalizeForPathParsing(path);
    if (normalized.isEmpty()) {
        return QString();
    }
    return QDir::toNativeSeparators(normalized);
}

MetadataConversionDirection inferMetadataConversionDirection(const QString &inputFilename)
{
    const QString parsePath = normalizeForPathParsing(inputFilename);
    if (parsePath.isEmpty()) {
        return MetadataConversionDirection::Unknown;
    }

    const QString suffix = QFileInfo(parsePath).suffix().toLower();
    if (suffix == QLatin1String("json")) {
        return MetadataConversionDirection::JsonToSqlite;
    }
    if (suffix == QLatin1String("db")) {
        return MetadataConversionDirection::SqliteToJson;
    }
    return MetadataConversionDirection::Unknown;
}

QString metadataConversionDirectionArgument(MetadataConversionDirection direction)
{
    switch (direction) {
    case MetadataConversionDirection::JsonToSqlite:
        return QStringLiteral("json-to-sqlite");
    case MetadataConversionDirection::SqliteToJson:
        return QStringLiteral("sqlite-to-json");
    case MetadataConversionDirection::Unknown:
    default:
        return QString();
    }
}
QString resolveMetadataConverterPath()
{
    const QStringList converterNames = {
        QStringLiteral("tbc-metadata-converter")
    };

    return resolveToolPath(converterNames);
}

QString defaultMetadataOutputPath(const QString &inputFilename, bool jsonToSqlite)
{
    return defaultMetadataOutputPath(inputFilename,
                                     jsonToSqlite ? MetadataConversionDirection::JsonToSqlite
                                                  : MetadataConversionDirection::SqliteToJson);
}

QString defaultMetadataOutputPath(const QString &inputFilename, MetadataConversionDirection direction)
{
    const QString normalizedInput = normalizeForPathParsing(inputFilename);
    if (normalizedInput.isEmpty()) {
        return QString();
    }

    const QFileInfo info(normalizedInput);
    QString outputPath;
    switch (direction) {
    case MetadataConversionDirection::JsonToSqlite:
        outputPath = info.suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0
                         ? outputPathWithSuffix(info, normalizedInput, QStringLiteral(".db"))
                         : normalizedInput + QStringLiteral(".db");
        break;
    case MetadataConversionDirection::SqliteToJson:
        outputPath = info.suffix().compare(QStringLiteral("db"), Qt::CaseInsensitive) == 0
                         ? outputPathWithSuffix(info, normalizedInput, QStringLiteral(".json"))
                         : normalizedInput + QStringLiteral(".json");
        break;
    case MetadataConversionDirection::Unknown:
    default:
        return QString();
    }

    return QDir::toNativeSeparators(outputPath);
}
QString resolveExportDecodeMetadataPath()
{
    const QStringList toolNames = {
        QStringLiteral("tbc-export-metadata")
    };

    return resolveToolPath(toolNames);
}

QString defaultExportDecodeMetadataOutputPath(const QString &inputFilename)
{
    const QString normalizedInput = normalizeForPathParsing(inputFilename);
    QFileInfo info(normalizedInput);
    if (info.suffix().compare(QStringLiteral("db"), Qt::CaseInsensitive) == 0) {
        return QDir::toNativeSeparators(info.path() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".export.json"));
    }
    return QDir::toNativeSeparators(normalizedInput + QStringLiteral(".export.json"));
}

bool runMetadataConverter(const QString &direction,
                          const QString &inputFilename,
                          const QString &outputFilename,
                          QString *errorMessage,
                          QWidget *parent,
                          bool *wasCancelled)
{
    return runMetadataConverter(metadataDirectionFromString(direction), inputFilename, outputFilename,
                                errorMessage, parent, wasCancelled);
}

bool runMetadataConverter(MetadataConversionDirection direction,
                          const QString &inputFilename,
                          const QString &outputFilename,
                          QString *errorMessage,
                          QWidget *parent,
                          bool *wasCancelled)
{
    if (wasCancelled) *wasCancelled = false;

    const QString converterPath = resolveMetadataConverterPath();
    if (converterPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("tbc-metadata-converter not found in PATH or alongside the application.");
        }
        return false;
    }
    if (direction == MetadataConversionDirection::Unknown) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Could not determine metadata conversion direction from input file extension.");
        }
        return false;
    }

    const QString normalizedInput = normalizePathForCurrentPlatform(inputFilename);
    const QString normalizedOutput = normalizePathForCurrentPlatform(outputFilename);
    const QString directionArgument = metadataConversionDirectionArgument(direction);

    QStringList arguments;
    arguments << QStringLiteral("--direction") << directionArgument;
    if (direction == MetadataConversionDirection::JsonToSqlite) {
        arguments << QStringLiteral("--input-json") << normalizedInput
                  << QStringLiteral("--output-sqlite") << normalizedOutput;
    } else {
        arguments << QStringLiteral("--input-sqlite") << normalizedInput
                  << QStringLiteral("--output-json") << normalizedOutput;
    }

    const ProcessProgressRunner::Result result =
        ProcessProgressRunner::run(converterPath, arguments, parent,
                                   QObject::tr("Running tbc-metadata-converter..."));

    if (result.status == ProcessProgressRunner::Result::Cancelled) {
        if (wasCancelled) *wasCancelled = true;
        if (errorMessage) {
            *errorMessage = QObject::tr("tbc-metadata-converter was cancelled.");
        }
        return false;
    }

    if (result.status != ProcessProgressRunner::Result::Finished) {
        if (errorMessage) {
            *errorMessage = QObject::tr("tbc-metadata-converter did not finish.");
        }
        return false;
    }

    if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0) {
        const QString stdErr = QString::fromLocal8Bit(result.standardError).trimmed();
        const QString stdOut = QString::fromLocal8Bit(result.standardOutput).trimmed();
        if (errorMessage) {
            *errorMessage = stdErr.isEmpty() ? stdOut : stdErr;
            if (errorMessage->isEmpty()) {
                *errorMessage = QObject::tr("tbc-metadata-converter failed with exit code %1.").arg(result.exitCode);
            }
        }
        return false;
    }

    return true;
}

bool runExportDecodeMetadata(const QString &inputFilename,
                             const QString &outputFilename,
                             QString *errorMessage,
                             QWidget *parent)
{
    return runExportDecodeMetadata(inputFilename,
                                   outputFilename,
                                   ExportDecodeMetadataOptions(),
                                   errorMessage,
                                   parent);
}

bool runExportDecodeMetadata(const QString &inputFilename,
                             const QString &outputFilename,
                             const ExportDecodeMetadataOptions &options,
                             QString *errorMessage,
                             QWidget *parent)
{
    const QString toolPath = resolveExportDecodeMetadataPath();
    if (toolPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("tbc-export-metadata not found in PATH or alongside the application.");
        }
        return false;
    }

    QStringList arguments;
    if (options.debug) {
        arguments << QStringLiteral("--debug");
    }
    if (options.quiet) {
        arguments << QStringLiteral("--quiet");
    }
    arguments << QStringLiteral("--input-sqlite") << normalizePathForCurrentPlatform(inputFilename)
              << QStringLiteral("--output-json") << normalizePathForCurrentPlatform(outputFilename);

    const ProcessProgressRunner::Result result =
        ProcessProgressRunner::run(toolPath, arguments, parent,
                                   QObject::tr("Running tbc-export-metadata..."));

    if (result.status == ProcessProgressRunner::Result::Cancelled) {
        if (errorMessage) {
            *errorMessage = QObject::tr("tbc-export-metadata was cancelled.");
        }
        return false;
    }

    if (result.status != ProcessProgressRunner::Result::Finished) {
        if (errorMessage) {
            *errorMessage = QObject::tr("tbc-export-metadata did not finish.");
        }
        return false;
    }

    if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0) {
        const QString stdErr = QString::fromLocal8Bit(result.standardError).trimmed();
        const QString stdOut = QString::fromLocal8Bit(result.standardOutput).trimmed();
        if (errorMessage) {
            *errorMessage = stdErr.isEmpty() ? stdOut : stdErr;
            if (errorMessage->isEmpty()) {
                *errorMessage = QObject::tr("tbc-export-metadata failed with exit code %1.").arg(result.exitCode);
            }
        }
        return false;
    }

    return true;
}
} // namespace MetadataConverterUtil
