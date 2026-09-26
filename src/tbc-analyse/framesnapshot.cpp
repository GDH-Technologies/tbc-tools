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
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include "sourcevideo.h"

#if defined(TBC_HAVE_OPENCV)
#include <opencv2/imgproc.hpp>
#endif
#if defined(TBC_HAVE_OPENCV_SUPERRES)
#include <opencv2/dnn_superres.hpp>
#endif

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

#if defined(TBC_HAVE_OPENCV)
struct Interpolation {
    const char *name;
    const char *label;
    int flag;
};

const Interpolation INTERPOLATIONS[] = {
    {"lanczos4", "Lanczos-4 (sharpest)", cv::INTER_LANCZOS4},
    {"cubic", "Bicubic", cv::INTER_CUBIC},
    {"linear", "Bilinear", cv::INTER_LINEAR},
    {"area", "Area", cv::INTER_AREA},
    {"nearest", "Nearest neighbour", cv::INTER_NEAREST},
};

// Wraps a QImage as a 3-channel 8-bit cv::Mat (RGB order). The QImage must
// outlive the Mat.
cv::Mat rgbMat(const QImage &rgb888)
{
    return cv::Mat(rgb888.height(), rgb888.width(), CV_8UC3,
                   const_cast<uchar *>(rgb888.constBits()), static_cast<size_t>(rgb888.bytesPerLine()));
}

QImage toQImage(const cv::Mat &rgb)
{
    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<qsizetype>(rgb.step), QImage::Format_RGB888).copy();
}

int interpolationFlag(const QString &method)
{
    for (const Interpolation &interpolation : INTERPOLATIONS) {
        if (method == QLatin1String(interpolation.name)) return interpolation.flag;
    }
    return cv::INTER_LANCZOS4; // learned methods resample for aspect with Lanczos
}
#endif

#if defined(TBC_HAVE_OPENCV_SUPERRES)
struct SuperResModel {
    const char *name;   // dnn_superres algorithm name, also the method name
    const char *prefix; // model file prefix: <prefix>_x<scale>.pb
    const char *label;
    QVector<qint32> scales;
};

const SuperResModel SUPERRES_MODELS[] = {
    {"edsr", "EDSR", "EDSR (learned, slow)", {2, 3, 4}},
    {"espcn", "ESPCN", "ESPCN (learned, fast)", {2, 3, 4}},
    {"fsrcnn", "FSRCNN", "FSRCNN (learned, fast)", {2, 3, 4}},
    {"lapsrn", "LapSRN", "LapSRN (learned)", {2, 4}},
};

// The flake's wrapper points TBC_SUPERRES_MODEL_DIR at the pinned model files;
// other installs keep them in share/tbc-tools/superres beside bin/.
QString superResModelDirectory()
{
    const QString fromEnvironment = qEnvironmentVariable("TBC_SUPERRES_MODEL_DIR");
    if (!fromEnvironment.isEmpty()) return fromEnvironment;
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../share/tbc-tools/superres"));
}

QString superResModelFile(const SuperResModel &model, qint32 scale)
{
    return QDir(superResModelDirectory()).filePath(QStringLiteral("%1_x%2.pb").arg(QLatin1String(model.prefix)).arg(scale));
}

const SuperResModel *findSuperResModel(const QString &method)
{
    for (const SuperResModel &model : SUPERRES_MODELS) {
        if (method == QLatin1String(model.name)) return &model;
    }
    return nullptr;
}
#endif

// Resize with an interpolation method (or Qt's smooth scaling without OpenCV)
QImage resampled(const QImage &image, const QSize &size, const QString &method)
{
    if (image.size() == size) return image;
#if defined(TBC_HAVE_OPENCV)
    if (isUpscaleMethodAvailable(method)) {
        const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
        cv::Mat output;
        cv::resize(rgbMat(rgb), output, cv::Size(size.width(), size.height()), 0, 0, interpolationFlag(method));
        return toQImage(output);
    }
#else
    Q_UNUSED(method)
#endif
    return image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

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

QVector<UpscaleMethod> upscaleMethods()
{
    QVector<UpscaleMethod> methods;
#if defined(TBC_HAVE_OPENCV)
    for (const Interpolation &interpolation : INTERPOLATIONS) {
        methods.append({QLatin1String(interpolation.name), QLatin1String(interpolation.label), false});
    }
#else
    methods.append({QStringLiteral("smooth"), QStringLiteral("Qt smooth (bilinear)"), false});
#endif
#if defined(TBC_HAVE_OPENCV_SUPERRES)
    for (const SuperResModel &model : SUPERRES_MODELS) {
        bool installed = true;
        for (const qint32 scale : model.scales) installed = installed && QFileInfo::exists(superResModelFile(model, scale));
        if (installed) methods.append({QLatin1String(model.name), QLatin1String(model.label), true});
    }
#endif
    return methods;
}

bool isUpscaleMethodAvailable(const QString &name)
{
    for (const UpscaleMethod &method : upscaleMethods()) {
        if (method.name == name) return true;
    }
    return false;
}

QImage upscale(const QImage &image, qint32 factor, const QString &method, QString *errorMessage)
{
    if (factor < 2 || factor > 4) {
        if (errorMessage) *errorMessage = QStringLiteral("Upscale factor must be 2, 3 or 4.");
        return QImage();
    }
    const QSize target = image.size() * factor;

#if defined(TBC_HAVE_OPENCV_SUPERRES)
    if (const SuperResModel *model = findSuperResModel(method)) {
        // A model without this scale (LapSRN has no 3x) runs at the next one
        // up and is brought down to size with Lanczos
        qint32 modelScale = factor;
        while (!model->scales.contains(modelScale)) modelScale++;

        try {
            cv::dnn_superres::DnnSuperResImpl superRes;
            superRes.readModel(superResModelFile(*model, modelScale).toStdString());
            superRes.setModel(model->name, modelScale);

            // The models take BGR
            const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
            cv::Mat bgr;
            cv::cvtColor(rgbMat(rgb), bgr, cv::COLOR_RGB2BGR);
            cv::Mat upscaledBgr;
            superRes.upsample(bgr, upscaledBgr);
            cv::Mat upscaledRgb;
            cv::cvtColor(upscaledBgr, upscaledRgb, cv::COLOR_BGR2RGB);
            return resampled(toQImage(upscaledRgb), target, QStringLiteral("lanczos4"));
        } catch (const cv::Exception &exception) {
            if (errorMessage) *errorMessage = QStringLiteral("%1 upscaling failed: %2").arg(QLatin1String(model->label), QString::fromStdString(exception.what()));
            return QImage();
        }
    }
#endif
    return resampled(image, target, method);
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

    // A method this build or install cannot run falls back to the first one it can
    const QString method = isUpscaleMethodAvailable(options.upscaleMethod)
                               ? options.upscaleMethod : upscaleMethods().constFirst().name;
    if (options.upscaleFactor > 1) {
        image = upscale(image, options.upscaleFactor, method, errorMessage);
        if (image.isNull()) return image;
    }

    const double aspect = pixelAspect(options.aspectMode, videoParameters);
    const qint32 width = std::max(1, static_cast<qint32>(std::lround(image.width() * aspect)));
    return resampled(image, QSize(width, image.height()), method);
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
