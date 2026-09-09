#ifndef METADATACONVERTERUTIL_H
#define METADATACONVERTERUTIL_H

#include <QString>

class QWidget;

namespace MetadataConverterUtil {
struct ExportDecodeMetadataOptions {
    bool debug = false;
    bool quiet = false;
};
enum class MetadataConversionDirection {
    Unknown,
    JsonToSqlite,
    SqliteToJson
};

QString normalizePathForCurrentPlatform(const QString &path);
MetadataConversionDirection inferMetadataConversionDirection(const QString &inputFilename);
QString metadataConversionDirectionArgument(MetadataConversionDirection direction);
QString resolveMetadataConverterPath();
QString defaultMetadataOutputPath(const QString &inputFilename, bool jsonToSqlite);
QString defaultMetadataOutputPath(const QString &inputFilename, MetadataConversionDirection direction);
QString resolveExportDecodeMetadataPath();
QString defaultExportDecodeMetadataOutputPath(const QString &inputFilename);
// A parent widget keeps the GUI responsive and offers a Cancel button while
// the tool runs; passing nullptr blocks the calling thread as before.
bool runMetadataConverter(const QString &direction,
                          const QString &inputFilename,
                          const QString &outputFilename,
                          QString *errorMessage = nullptr,
                          QWidget *parent = nullptr,
                          bool *wasCancelled = nullptr);
bool runMetadataConverter(MetadataConversionDirection direction,
                          const QString &inputFilename,
                          const QString &outputFilename,
                          QString *errorMessage = nullptr,
                          QWidget *parent = nullptr,
                          bool *wasCancelled = nullptr);
bool runExportDecodeMetadata(const QString &inputFilename,
                             const QString &outputFilename,
                             QString *errorMessage = nullptr,
                             QWidget *parent = nullptr);
bool runExportDecodeMetadata(const QString &inputFilename,
                             const QString &outputFilename,
                             const ExportDecodeMetadataOptions &options,
                             QString *errorMessage = nullptr,
                             QWidget *parent = nullptr);
}

#endif // METADATACONVERTERUTIL_H
