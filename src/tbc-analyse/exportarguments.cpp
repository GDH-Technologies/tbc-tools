#include "exportarguments.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace ExportArguments {

namespace {
bool hasKnownOutputContainerSuffix(const QString &suffix)
{
    static const QStringList knownSuffixes = {
        QStringLiteral("mkv"),
        QStringLiteral("mov"),
        QStringLiteral("mp4"),
        QStringLiteral("mxf"),
        QStringLiteral("avi")
    };
    for (const QString &knownSuffix : knownSuffixes) {
        if (suffix.compare(knownSuffix, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString stripKnownOutputContainerSuffix(const QString &fileName)
{
    const int lastDot = fileName.lastIndexOf(QLatin1Char('.'));
    if (lastDot <= 0) {
        return fileName;
    }
    const QString suffix = fileName.mid(lastDot + 1);
    if (!hasKnownOutputContainerSuffix(suffix)) {
        return fileName;
    }
    const QString strippedName = fileName.left(lastDot);
    return strippedName.isEmpty() ? fileName : strippedName;
}
} // namespace

bool shouldDisableDropoutCorrection(const QString &dropoutMode, int startFrameOneBased,
                                    bool reverseFieldOrder)
{
    Q_UNUSED(startFrameOneBased);
    // Reversed field order exports must disable dropout correction
    // (tbc-video-export CLI contract: --reverse requires --no-dropout-correct).
    if (reverseFieldOrder) {
        return true;
    }
    const QString normalizedMode = dropoutMode.trimmed().toLower();
    return normalizedMode == QStringLiteral("disabled");
}
bool isDefaultActiveAreaFraming(const QString &resolutionMode, bool hasAnyVerticalLineAdjustment)
{
    const QString normalizedMode = resolutionMode.trimmed().toLower();
    return normalizedMode == QStringLiteral("active_area") && !hasAnyVerticalLineAdjustment;
}
QString sanitizeOutputBasePath(const QString &path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) {
        return QString();
    }

    const QFileInfo info(trimmedPath);
    const QString fileName = info.fileName().trimmed();
    if (fileName.isEmpty()) {
        return QDir::cleanPath(info.absoluteFilePath());
    }

    const QString sanitizedFileName = stripKnownOutputContainerSuffix(fileName);
    return QDir(info.absolutePath()).filePath(sanitizedFileName);
}
// MONO has no chroma pass, so a split (Y+C) source exports its luma TBC alone
bool shouldExportLumaOnly(bool isSplitSource, const QString &chromaDecoderName)
{
    return isSplitSource && chromaDecoderName.trimmed().toLower() == QStringLiteral("mono");
}
// ProRes 422 can't encode gray; left to ffmpeg it becomes 4:4:4 under a 422 profile
bool lumaOnlyNeedsYuv422(const QString &profileName)
{
    const QString normalizedName = profileName.trimmed().toLower();
    return normalizedName == QStringLiteral("prores")
           || normalizedName == QStringLiteral("prores_lt")
           || normalizedName == QStringLiteral("prores_proxy")
           || normalizedName == QStringLiteral("prores_hq");
}

// Directories searched for tbc-video-export, ffmpeg, ffprobe and ffplay before
// PATH, and prepended to the export's PATH. Never the input file's directory or
// the working directory: an executable there named ffmpeg (beside a capture on
// shared storage, say) would run in place of the real one. A directory anyone
// can write to is skipped too: the parent walk below reaches /tmp from an
// AppImage's mount point.
QStringList defaultExecutableSearchDirs()
{
    QStringList candidateDirs;
    auto appendUniqueDir = [&candidateDirs](const QString &dirPath) {
        if (dirPath.isEmpty()) {
            return;
        }
#if !defined(Q_OS_WIN)
        if (QFileInfo(dirPath).permission(QFileDevice::WriteOther)) {
            return;
        }
#endif
        for (const QString &existing : candidateDirs) {
            if (existing.compare(dirPath, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        candidateDirs << dirPath;
    };
    const QString appFilePath = QCoreApplication::applicationFilePath();
    if (!appFilePath.isEmpty()) {
        const QFileInfo appFileInfo(appFilePath);
        const QString canonicalAppFilePath = appFileInfo.canonicalFilePath();
        if (!canonicalAppFilePath.isEmpty()) {
            appendUniqueDir(QFileInfo(canonicalAppFilePath).absolutePath());
        }
        appendUniqueDir(appFileInfo.absolutePath());
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        appendUniqueDir(appDir);

        QDir parentDir(appDir);
        for (int i = 0; i < 4; ++i) {
            if (!parentDir.cdUp()) {
                break;
            }
            appendUniqueDir(parentDir.absolutePath());
        }
    }

    const QString homeDir = QDir::homePath();
    if (!homeDir.isEmpty()) {
        const QString localBin = QDir(homeDir).filePath(QStringLiteral(".local/bin"));
        const QString userBin = QDir(homeDir).filePath(QStringLiteral("bin"));
        appendUniqueDir(localBin);
        appendUniqueDir(userBin);
    }

#if defined(Q_OS_MACOS)
    const QStringList platformDirs = {
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/opt/homebrew/sbin"),
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/usr/local/sbin"),
        QStringLiteral("/usr/bin"),
        QStringLiteral("/bin")
    };
#elif defined(Q_OS_UNIX)
    const QStringList platformDirs = {
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/usr/local/sbin"),
        QStringLiteral("/usr/bin"),
        QStringLiteral("/bin"),
        QStringLiteral("/snap/bin")
    };
#else
    const QStringList platformDirs;
#endif

    for (const QString &dir : platformDirs) {
        appendUniqueDir(dir);
    }

    return candidateDirs;
}

}
