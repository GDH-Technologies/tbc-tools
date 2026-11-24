#ifndef EXPORTARGUMENTS_H
#define EXPORTARGUMENTS_H

#include <QString>
#include <QStringList>

namespace ExportArguments {
bool shouldDisableDropoutCorrection(const QString &dropoutMode, int startFrameOneBased,
                                    bool reverseFieldOrder = false);
bool isDefaultActiveAreaFraming(const QString &resolutionMode, bool hasAnyVerticalLineAdjustment);
QString sanitizeOutputBasePath(const QString &path);
bool shouldExportLumaOnly(bool isSplitSource, const QString &chromaDecoderName);
bool lumaOnlyNeedsYuv422(const QString &profileName);
QStringList defaultExecutableSearchDirs();
}

#endif // EXPORTARGUMENTS_H
