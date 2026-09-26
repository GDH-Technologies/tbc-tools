/******************************************************************************
 * framesnapshot.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "framesnapshot.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include "sourcevideo.h"

namespace FrameSnapshot {

namespace {

// Square-pixel sampling rates for a 4:3 picture (ITU-R BT.601 Annex)
constexpr double SQUARE_PIXEL_RATE_525 = 135.0e6 / 11.0;
constexpr double SQUARE_PIXEL_RATE_625 = 14.75e6;

// Best-frame search thresholds. A frame belongs to the anchor's still while
// its 8x8-block thumbnail differs from the anchor's by less than
// RUN_BREAK_DIFFERENCE (fraction of black-to-white). Within that run, a frame
// is only a candidate if its combing and dropouts are near the run's median.
constexpr double RUN_BREAK_DIFFERENCE = 0.06;
constexpr qint32 THUMBNAIL_BLOCK = 8;
constexpr double COMBING_TOLERANCE = 1.25;
constexpr double DROPOUT_ALLOWANCE_SAMPLES = 20.0;

const QStringList UPSCALER_MODELS = {
    QStringLiteral("realesrgan-x4plus"),
    QStringLiteral("realesrgan-x4plus-anime"),
    QStringLiteral("realesr-animevideov3"),
};

bool is625LineSystem(VideoSystem system)
{
    return system == PAL || system == SECAM || system == MESECAM;
}

double median(QVector<double> values)
{
    if (values.isEmpty()) return 0.0;
    std::sort(values.begin(), values.end());
    const qint32 mid = values.size() / 2;
    return (values.size() % 2) ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

#if defined(Q_OS_LINUX)
// realesrgan-ncnn-vulkan from nixpkgs links Nix's Vulkan loader, which on a
// non-NixOS host finds no GPU driver: the host's NVIDIA ICD manifest is not on
// its search path, and libGLX_nvidia.so.0 needs companion libraries from the
// host libdir that a Nix binary never searches. Exposing the whole host libdir
// would mix the host glibc's libraries into a process running Nix's glibc, so
// the child gets a private directory of symlinks to just the NVIDIA driver and
// the X11/xcb/EGL libraries it loads. Returns false when no NVIDIA ICD exists.
bool prepareHostVulkanDriver(const QString &shimDirectory, QProcessEnvironment &environment)
{
    if (environment.contains(QStringLiteral("VK_DRIVER_FILES"))
        || environment.contains(QStringLiteral("VK_ICD_FILENAMES"))) {
        return false; // the operator chose the driver
    }

    QString manifestPath;
    QString libraryPath;
    for (const QString &icdDirectory : {QStringLiteral("/etc/vulkan/icd.d"), QStringLiteral("/usr/share/vulkan/icd.d")}) {
        const QFileInfoList manifests = QDir(icdDirectory).entryInfoList({QStringLiteral("nvidia_icd*.json")}, QDir::Files, QDir::Name);
        for (const QFileInfo &manifest : manifests) {
            // Skip the 32-bit manifest Fedora installs alongside the 64-bit one
            if (manifest.fileName().contains(QStringLiteral("i686"))) continue;
            QFile file(manifest.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QString path = QJsonDocument::fromJson(file.readAll()).object()
                                     .value(QStringLiteral("ICD")).toObject()
                                     .value(QStringLiteral("library_path")).toString();
            if (path.isEmpty()) continue;

            if (QFileInfo(path).isAbsolute()) {
                if (QFileInfo::exists(path)) libraryPath = path;
            } else {
                // Debian-style manifests name a bare soname
                for (const QString &libDirectory : {QStringLiteral("/usr/lib64"), QStringLiteral("/usr/lib/x86_64-linux-gnu"),
                                                    QStringLiteral("/usr/lib/aarch64-linux-gnu"), QStringLiteral("/usr/lib")}) {
                    if (QFileInfo::exists(QDir(libDirectory).filePath(path))) {
                        libraryPath = QDir(libDirectory).filePath(path);
                        break;
                    }
                }
            }
            if (!libraryPath.isEmpty()) {
                manifestPath = manifest.absoluteFilePath();
                break;
            }
        }
        if (!manifestPath.isEmpty()) break;
    }
    if (manifestPath.isEmpty()) return false;

    const QDir hostLibDirectory = QFileInfo(libraryPath).absoluteDir();
    const QStringList patterns = {
        QStringLiteral("libnvidia-*.so*"), QStringLiteral("libGLX_nvidia.so*"), QStringLiteral("libEGL_nvidia.so*"),
        QStringLiteral("libGLdispatch.so*"), QStringLiteral("libEGL.so.1"), QStringLiteral("libX11.so.6"),
        QStringLiteral("libX11-xcb.so.1"), QStringLiteral("libXext.so.6"), QStringLiteral("libXau.so.6"),
        QStringLiteral("libXdmcp.so.6"), QStringLiteral("libxcb*.so.*"),
    };
    for (const QFileInfo &library : hostLibDirectory.entryInfoList(patterns, QDir::Files | QDir::System)) {
        QFile::link(library.absoluteFilePath(), QDir(shimDirectory).filePath(library.fileName()));
    }

    environment.insert(QStringLiteral("VK_DRIVER_FILES"), manifestPath);
    const QString existing = environment.value(QStringLiteral("LD_LIBRARY_PATH"));
    environment.insert(QStringLiteral("LD_LIBRARY_PATH"),
                       existing.isEmpty() ? shimDirectory : shimDirectory + QLatin1Char(':') + existing);
    return true;
}
#endif

} // namespace

QString framingName(Framing framing)
{
    switch (framing) {
    case Framing::Full: return QStringLiteral("full");
    case Framing::Active: return QStringLiteral("active");
    case Framing::Custom: return QStringLiteral("custom");
    }
    return QStringLiteral("active");
}

Framing framingFromName(const QString &name, Framing fallback)
{
    const QString key = name.trimmed().toLower();
    if (key == QLatin1String("full")) return Framing::Full;
    if (key == QLatin1String("active")) return Framing::Active;
    if (key == QLatin1String("custom")) return Framing::Custom;
    return fallback;
}

QString aspectModeName(AspectMode mode)
{
    switch (mode) {
    case AspectMode::Exact: return QStringLiteral("exact");
    case AspectMode::Viewer: return QStringLiteral("viewer");
    }
    return QStringLiteral("exact");
}

AspectMode aspectModeFromName(const QString &name, AspectMode fallback)
{
    const QString key = name.trimmed().toLower();
    if (key == QLatin1String("exact")) return AspectMode::Exact;
    if (key == QLatin1String("viewer")) return AspectMode::Viewer;
    return fallback;
}

QRect activeFrameRect(const TbcMetaData::VideoParameters &videoParameters, const QSize &frameSize)
{
    if (videoParameters.activeVideoStart < 0 || videoParameters.activeVideoEnd <= videoParameters.activeVideoStart
        || videoParameters.firstActiveFrameLine < 0
        || videoParameters.lastActiveFrameLine <= videoParameters.firstActiveFrameLine) {
        return QRect();
    }
    const QRect rect(videoParameters.activeVideoStart, videoParameters.firstActiveFrameLine,
                     videoParameters.activeVideoEnd - videoParameters.activeVideoStart,
                     videoParameters.lastActiveFrameLine - videoParameters.firstActiveFrameLine);
    return rect.intersected(QRect(QPoint(0, 0), frameSize));
}

QRect outputRect(const Options &options, const TbcMetaData::VideoParameters &videoParameters,
                 const QSize &frameSize)
{
    const QRect frameRect(QPoint(0, 0), frameSize);
    QRect base = frameRect;
    if (options.framing != Framing::Full) {
        const QRect active = activeFrameRect(videoParameters, frameSize);
        if (!active.isEmpty()) base = active;
        if (options.framing == Framing::Custom) {
            const QRect custom = options.customRect.intersected(frameRect);
            if (!custom.isEmpty()) base = custom;
        }
    }

    // adjusted() can flip the rectangle inside out, and intersected() would
    // normalise that back into something non-empty
    const QRect trimmed = base.adjusted(options.marginLeft, options.marginTop,
                                        -options.marginRight, -options.marginBottom);
    if (trimmed.width() <= 0 || trimmed.height() <= 0) return base;
    return trimmed.intersected(frameRect);
}

qint32 viewerAspectAdjustment(const TbcMetaData::VideoParameters &videoParameters)
{
    if (videoParameters.system == PAL) {
        // 625 lines
        return videoParameters.isWidescreen ? 103 : -196;
    }
    // 525 lines
    return videoParameters.isWidescreen ? 122 : -150;
}

double pixelAspect(AspectMode mode, const TbcMetaData::VideoParameters &videoParameters)
{
    if (mode == AspectMode::Viewer) {
        if (videoParameters.fieldWidth <= 0) return 1.0;
        return static_cast<double>(videoParameters.fieldWidth + viewerAspectAdjustment(videoParameters))
               / videoParameters.fieldWidth;
    }

    if (videoParameters.sampleRate <= 0.0) return 1.0;
    const double squareRate = is625LineSystem(videoParameters.system) ? SQUARE_PIXEL_RATE_625 : SQUARE_PIXEL_RATE_525;
    const double aspect = squareRate / videoParameters.sampleRate;
    return videoParameters.isWidescreen ? aspect * 4.0 / 3.0 : aspect;
}

QSize outputSize(const Options &options, const TbcMetaData::VideoParameters &videoParameters,
                 const QSize &framedSize)
{
    const qint32 factor = std::max(1, options.upscaleFactor);
    const QSize scaled = framedSize * factor;
    const qint32 width = static_cast<qint32>(std::lround(scaled.width() * pixelAspect(options.aspectMode, videoParameters)));
    return QSize(std::max(1, width), scaled.height());
}

QString upscalerExecutable()
{
    const QString name = QStringLiteral("realesrgan-ncnn-vulkan");
    const QString bundled = QStandardPaths::findExecutable(name, {QCoreApplication::applicationDirPath()});
    return bundled.isEmpty() ? QStandardPaths::findExecutable(name) : bundled;
}

QStringList upscalerModels()
{
    return UPSCALER_MODELS;
}

QImage upscale(const QImage &image, qint32 factor, const QString &model, QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) *errorMessage = message;
        return QImage();
    };

    if (factor < 2 || factor > 4) return fail(QStringLiteral("Upscale factor must be 2, 3 or 4."));
    const QString executable = upscalerExecutable();
    if (executable.isEmpty()) {
        return fail(QStringLiteral("realesrgan-ncnn-vulkan was not found on PATH."));
    }

    QTemporaryDir workDirectory;
    if (!workDirectory.isValid()) return fail(QStringLiteral("Could not create a temporary directory for upscaling."));
    const QString inputPath = workDirectory.filePath(QStringLiteral("in.png"));
    const QString outputPath = workDirectory.filePath(QStringLiteral("out.png"));
    if (!image.save(inputPath)) return fail(QStringLiteral("Could not write the upscaler's input image."));

    // The x4plus models only exist at 4x; other factors are scaled down after.
    const bool nativeScale = model == QLatin1String("realesr-animevideov3");
    const qint32 modelScale = nativeScale ? factor : 4;

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#if defined(Q_OS_LINUX)
    if (QFileInfo(executable).canonicalFilePath().startsWith(QLatin1String("/nix/store/"))
        && !QFileInfo::exists(QStringLiteral("/run/opengl-driver"))) {
        const QString shimDirectory = workDirectory.filePath(QStringLiteral("host-vulkan"));
        if (QDir().mkpath(shimDirectory)) prepareHostVulkanDriver(shimDirectory, environment);
    }
#endif
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(workDirectory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, {QStringLiteral("-i"), inputPath, QStringLiteral("-o"), outputPath,
                               QStringLiteral("-n"), model, QStringLiteral("-s"), QString::number(modelScale),
                               QStringLiteral("-f"), QStringLiteral("png")});
    if (!process.waitForStarted()) return fail(QStringLiteral("Could not start %1.").arg(executable));
    process.waitForFinished(-1);

    const QImage upscaled(outputPath);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || upscaled.isNull()) {
        // Drop the per-tile progress lines ("12.50%") and keep the diagnostics
        QStringList lines;
        for (const QString &line : QString::fromLocal8Bit(process.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (!line.trimmed().endsWith(QLatin1Char('%'))) lines << line.trimmed();
        }
        return fail(QStringLiteral("Real-ESRGAN failed (exit code %1):\n%2")
                        .arg(process.exitCode()).arg(lines.mid(std::max(0, static_cast<int>(lines.size()) - 12)).join(QLatin1Char('\n'))));
    }

    if (modelScale == factor) return upscaled;
    return upscaled.scaled(image.size() * factor, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage process(const QImage &frameImage, const Options &options,
               const TbcMetaData::VideoParameters &videoParameters, QString *errorMessage)
{
    if (frameImage.isNull()) {
        if (errorMessage) *errorMessage = QStringLiteral("No image data is available.");
        return QImage();
    }

    // Crop, then upscale, then resample for aspect: resampling the larger image
    // loses less horizontal detail.
    QImage image = frameImage.copy(outputRect(options, videoParameters, frameImage.size()));

    if (options.upscaleFactor > 1) {
        image = upscale(image, options.upscaleFactor, options.upscaleModel, errorMessage);
        if (image.isNull()) return image;
    }

    const double aspect = pixelAspect(options.aspectMode, videoParameters);
    const qint32 width = std::max(1, static_cast<qint32>(std::lround(image.width() * aspect)));
    if (width == image.width()) return image;
    return image.scaled(width, image.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

FieldMetrics measureField(const QVector<double> &plane, qint32 width, qint32 height)
{
    FieldMetrics metrics;
    if (width < 3 || height < 3 || plane.size() < width * height) return metrics;

    double gradientEnergy = 0.0;
    double laplacianSum = 0.0;
    for (qint32 y = 1; y < height - 1; y++) {
        const double *above = plane.constData() + (y - 1) * width;
        const double *row = plane.constData() + y * width;
        const double *below = plane.constData() + (y + 1) * width;
        for (qint32 x = 1; x < width - 1; x++) {
            const double gx = (above[x + 1] + 2.0 * row[x + 1] + below[x + 1]) - (above[x - 1] + 2.0 * row[x - 1] + below[x - 1]);
            const double gy = (below[x - 1] + 2.0 * below[x] + below[x + 1]) - (above[x - 1] + 2.0 * above[x] + above[x + 1]);
            gradientEnergy += gx * gx + gy * gy;

            // Immerkaer's noise mask: the difference of two Laplacians, which
            // cancels most image structure and leaves the noise
            laplacianSum += std::fabs(above[x - 1] - 2.0 * above[x] + above[x + 1]
                                      - 2.0 * row[x - 1] + 4.0 * row[x] - 2.0 * row[x + 1]
                                      + below[x - 1] - 2.0 * below[x] + below[x + 1]);
        }
    }
    const double interior = static_cast<double>(width - 2) * (height - 2);
    metrics.sharpness = gradientEnergy / interior;
    metrics.noise = std::sqrt(M_PI / 2.0) * laplacianSum / (6.0 * interior);
    return metrics;
}

SearchResult findBestFrame(const SearchInput &input, std::atomic<bool> *cancel, std::atomic<qint32> *progress)
{
    SearchResult result;
    const TbcMetaData::VideoParameters &videoParameters = input.videoParameters;
    const qint32 fieldWidth = videoParameters.fieldWidth;
    const qint32 fieldHeight = videoParameters.fieldHeight;

    SourceVideo sourceVideo;
    if (fieldWidth <= 0 || fieldHeight <= 0
        || !sourceVideo.open(input.tbcFilename, fieldWidth * fieldHeight, fieldWidth)) {
        result.errorMessage = QStringLiteral("Could not open %1 for the best-frame search.").arg(input.tbcFilename);
        return result;
    }

    // Frame row r comes from the first field (even r) or second field (odd r),
    // field line r / 2. Score only the field lines inside the crop.
    const QRect crop = input.cropRect.intersected(QRect(0, 0, fieldWidth, fieldHeight * 2 - 1));
    const qint32 x0 = crop.left();
    const qint32 width = crop.width();
    const qint32 line0 = (crop.top() + 1) / 2;
    const qint32 height = std::min(fieldHeight, (crop.bottom() + 1) / 2) - line0;
    if (width < THUMBNAIL_BLOCK || height < THUMBNAIL_BLOCK) {
        result.errorMessage = QStringLiteral("The framed area is too small to score.");
        return result;
    }

    const double black = videoParameters.black16bIre;
    const double range = (videoParameters.white16bIre > videoParameters.black16bIre)
                             ? videoParameters.white16bIre - videoParameters.black16bIre : 65535.0;

    auto readPlane = [&](qint32 fieldNumber) {
        QVector<double> plane(width * height);
        const SourceVideo::Data field = sourceVideo.getVideoField(fieldNumber);
        for (qint32 y = 0; y < height; y++) {
            const quint16 *source = field.constData() + (line0 + y) * fieldWidth + x0;
            double *target = plane.data() + y * width;
            for (qint32 x = 0; x < width; x++) target[x] = (source[x] - black) / range;
        }
        return plane;
    };

    const qint32 blocksX = width / THUMBNAIL_BLOCK;
    const qint32 blocksY = height / THUMBNAIL_BLOCK;
    auto thumbnail = [&](const QVector<double> &plane) {
        QVector<double> blocks(blocksX * blocksY, 0.0);
        for (qint32 y = 0; y < blocksY * THUMBNAIL_BLOCK; y++) {
            for (qint32 x = 0; x < blocksX * THUMBNAIL_BLOCK; x++) {
                blocks[(y / THUMBNAIL_BLOCK) * blocksX + x / THUMBNAIL_BLOCK] += plane[y * width + x];
            }
        }
        for (double &block : blocks) block /= THUMBNAIL_BLOCK * THUMBNAIL_BLOCK;
        return blocks;
    };

    QVector<double> anchorThumbnail;
    auto scoreFrame = [&](qint32 frame, FrameScore &score) {
        score.frame = frame;
        const qint32 index = frame - input.firstFrame;
        const QVector<double> first = readPlane(input.fieldNumbers[index].first);
        const QVector<double> second = readPlane(input.fieldNumbers[index].second);

        const FieldMetrics firstMetrics = measureField(first, width, height);
        const FieldMetrics secondMetrics = measureField(second, width, height);
        score.sharpness = (firstMetrics.sharpness + secondMetrics.sharpness) / 2.0;
        score.noise = (firstMetrics.noise + secondMetrics.noise) / 2.0;

        // Combing on the woven frame: how far each line sits from the mean of
        // its neighbours in the other field
        double combing = 0.0;
        for (qint32 y = 1; y < height; y++) {
            for (qint32 x = 0; x < width; x++) {
                const double fromFirst = first[y * width + x] - (second[(y - 1) * width + x] + second[y * width + x]) / 2.0;
                const double fromSecond = second[(y - 1) * width + x] - (first[(y - 1) * width + x] + first[y * width + x]) / 2.0;
                combing += std::fabs(fromFirst) + std::fabs(fromSecond);
            }
        }
        score.combing = combing / (2.0 * width * (height - 1));
        score.dropouts = (index < input.visibleDropouts.size()) ? input.visibleDropouts[index] : 0.0;

        const QVector<double> frameThumbnail = thumbnail(first);
        if (anchorThumbnail.isEmpty()) anchorThumbnail = frameThumbnail;
        double difference = 0.0;
        for (qint32 i = 0; i < frameThumbnail.size(); i++) difference += std::fabs(frameThumbnail[i] - anchorThumbnail[i]);
        score.anchorDiff = difference / frameThumbnail.size();
        score.inRun = score.anchorDiff < RUN_BREAK_DIFFERENCE;

        if (progress) progress->fetch_add(1);
    };

    const qint32 lastFrame = input.firstFrame + input.fieldNumbers.size() - 1;
    if (input.anchorFrame < input.firstFrame || input.anchorFrame > lastFrame) {
        result.errorMessage = QStringLiteral("The current frame is outside the search window.");
        return result;
    }

    FrameScore anchorScore;
    scoreFrame(input.anchorFrame, anchorScore);
    result.scores.append(anchorScore);

    // Walk outwards from the anchor until the picture changes
    for (const qint32 step : {-1, 1}) {
        for (qint32 distance = 1; distance <= input.radius; distance++) {
            if (cancel && cancel->load()) {
                result.cancelled = true;
                return result;
            }
            const qint32 frame = input.anchorFrame + step * distance;
            if (frame < input.firstFrame || frame > lastFrame) break;
            FrameScore score;
            scoreFrame(frame, score);
            result.scores.append(score);
            if (!score.inRun) break;
        }
    }
    std::sort(result.scores.begin(), result.scores.end(),
              [](const FrameScore &a, const FrameScore &b) { return a.frame < b.frame; });

    QVector<double> runCombing;
    QVector<double> runDropouts;
    for (const FrameScore &score : result.scores) {
        if (!score.inRun) continue;
        runCombing.append(score.combing);
        runDropouts.append(score.dropouts);
    }
    const double combingLimit = median(runCombing) * COMBING_TOLERANCE + 1e-6;
    const double runDropoutMedian = median(runDropouts);
    const double dropoutLimit = runDropoutMedian + std::max(DROPOUT_ALLOWANCE_SAMPLES, runDropoutMedian);

    double bestScore = -1.0;
    result.bestFrame = input.anchorFrame;
    for (FrameScore &score : result.scores) {
        score.eligible = score.inRun && score.combing <= combingLimit && score.dropouts <= dropoutLimit;
        // Sobel's response to white noise of sigma s is 24 s^2; what is left is
        // picture detail, scored per unit of noise.
        const double detail = std::max(0.0, score.sharpness - 24.0 * score.noise * score.noise);
        score.score = detail / std::max(score.noise, 1e-6);
        if (score.eligible && score.score > bestScore) {
            bestScore = score.score;
            result.bestFrame = score.frame;
        }
    }
    return result;
}

bool writeScoreReport(const QString &filename, const SearchResult &result, QString *errorMessage)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) *errorMessage = QStringLiteral("Could not write %1").arg(filename);
        return false;
    }
    QTextStream stream(&file);
    stream << "frame,in_run,eligible,anchor_diff,sharpness,noise,combing,dropouts,score,best\n";
    for (const FrameScore &score : result.scores) {
        stream << score.frame << ',' << int(score.inRun) << ',' << int(score.eligible) << ','
               << score.anchorDiff << ',' << score.sharpness << ',' << score.noise << ','
               << score.combing << ',' << score.dropouts << ',' << score.score << ','
               << int(score.frame == result.bestFrame) << '\n';
    }
    return true;
}

} // namespace FrameSnapshot
