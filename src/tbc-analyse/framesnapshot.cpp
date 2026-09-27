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
#include <QHash>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

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

// Still-picture search thresholds. A frame belongs to the anchor's still
// while its 8x8-block thumbnail differs from the anchor's by less than
// RUN_BREAK_DIFFERENCE (fraction of black-to-white); measured on a VHS
// slideshow, cuts sit at 0.24-0.36 and frames of one photo at <= 0.018.
// Within the run a frame is rejected when its visible dropouts are well above
// the run's median, or its RMS distance from the run's per-pixel median is
// more than DISTANCE_TOLERANCE times the typical distance: a head-switching
// tear measured about 2x.
constexpr double RUN_BREAK_DIFFERENCE = 0.06;
constexpr qint32 THUMBNAIL_BLOCK = 8;
constexpr double DISTANCE_TOLERANCE = 1.5;
constexpr double DROPOUT_ALLOWANCE_SAMPLES = 20.0;
constexpr qint32 ALIGN_BAND_LINES = 16;

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

// Keys cubic convolution weights (a = -0.5) for the four taps around a
// fractional position t in [0, 1)
void cubicWeights(double t, double weights[4])
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    weights[0] = -0.5 * t3 + t2 - 0.5 * t;
    weights[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    weights[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    weights[3] = 0.5 * t3 - 0.5 * t2;
}

// Cubic sample of samples[0..length) (spaced step apart) at a fractional
// position, clamping at the ends
double sampleCubic(const float *samples, qint32 length, qint32 step, double position)
{
    const double floorPosition = std::floor(position);
    const qint32 base = static_cast<qint32>(floorPosition);
    double weights[4];
    cubicWeights(position - floorPosition, weights);
    double value = 0.0;
    for (qint32 tap = 0; tap < 4; tap++) {
        const qint32 index = std::clamp(base - 1 + tap, 0, length - 1);
        value += weights[tap] * samples[static_cast<size_t>(index) * step];
    }
    return value;
}

// Gauss-Newton estimate of s where image(p) ~ reference(p - s) along one
// axis, over `lines` lines of `length` samples (step apart; lines lineStride
// apart). Validated against Fourier-shifted frames of the Christmas 1998 tape:
// injected 0.1-1.0 sample shifts came back exact to 0.001.
double estimateShift(const float *image, const float *reference, qint32 lines, qint32 lineStride,
                     qint32 length, qint32 step)
{
    constexpr qint32 MARGIN = 8;
    constexpr qint32 ITERATIONS = 4;
    constexpr double MAX_SHIFT = 4.0;
    if (length <= 2 * MARGIN + 2) return 0.0;

    double shift = 0.0;
    std::vector<double> shifted(length);
    for (qint32 iteration = 0; iteration < ITERATIONS; iteration++) {
        double numerator = 0.0;
        double denominator = 0.0;
        for (qint32 line = 0; line < lines; line++) {
            const float *referenceLine = reference + static_cast<size_t>(line) * lineStride;
            const float *imageLine = image + static_cast<size_t>(line) * lineStride;
            for (qint32 p = MARGIN - 1; p <= length - MARGIN; p++) {
                shifted[p] = sampleCubic(referenceLine, length, step, p - shift);
            }
            for (qint32 p = MARGIN; p < length - MARGIN; p++) {
                const double gradient = (shifted[p + 1] - shifted[p - 1]) / 2.0;
                const double error = imageLine[static_cast<size_t>(p) * step] - shifted[p];
                numerator += gradient * error;
                denominator += gradient * gradient;
            }
        }
        if (denominator <= 1e-12) return 0.0;
        shift = std::clamp(shift - numerator / denominator, -MAX_SHIFT, MAX_SHIFT);
    }
    return shift;
}

// Horizontal shift for a field line: linear between band centres, held flat
// beyond the first and last band
double shiftForFieldLine(const FrameAlignment &alignment, qint32 field, qint32 fieldLine)
{
    const QVector<float> &shifts = alignment.shiftX[field];
    if (shifts.isEmpty()) return 0.0;
    const double band = (fieldLine - alignment.firstFieldLine + 0.5) / alignment.bandHeight - 0.5;
    if (band <= 0.0) return shifts.constFirst();
    if (band >= shifts.size() - 1) return shifts.constLast();
    const qint32 lower = static_cast<qint32>(band);
    const double t = band - lower;
    return shifts[lower] * (1.0 - t) + shifts[lower + 1] * t;
}

// Resamples one field (rows contiguous, `channels` interleaved floats per
// sample) so it lines up with the reference the alignment was measured
// against: output(row, x) = input(row + shiftY, x + shiftX(row)), cubic in
// both directions within the field. firstRowFieldLine is the field line of
// row 0, for looking up the band shifts.
std::vector<float> resampleField(const float *data, qint32 width, qint32 rows, qint32 channels,
                                 const FrameAlignment &alignment, qint32 field, qint32 firstRowFieldLine)
{
    std::vector<float> output(static_cast<size_t>(width) * rows * channels);
    std::vector<float> line(static_cast<size_t>(width) * channels);
    const size_t rowLength = static_cast<size_t>(width) * channels;
    for (qint32 row = 0; row < rows; row++) {
        const double position = row + alignment.shiftY[field];
        const double floorPosition = std::floor(position);
        double weights[4];
        cubicWeights(position - floorPosition, weights);
        std::fill(line.begin(), line.end(), 0.0f);
        for (qint32 tap = 0; tap < 4; tap++) {
            const qint32 sourceRow = std::clamp(static_cast<qint32>(floorPosition) - 1 + tap, 0, rows - 1);
            const float *source = data + sourceRow * rowLength;
            for (size_t i = 0; i < rowLength; i++) line[i] += static_cast<float>(weights[tap] * source[i]);
        }
        const double shift = shiftForFieldLine(alignment, field, firstRowFieldLine + row);
        float *out = output.data() + row * rowLength;
        for (qint32 x = 0; x < width; x++) {
            for (qint32 c = 0; c < channels; c++) {
                out[x * channels + c] = static_cast<float>(sampleCubic(line.data() + c, width, channels, x + shift));
            }
        }
    }
    return output;
}

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

QString stillModeName(StillMode mode)
{
    switch (mode) {
    case StillMode::Off: return QStringLiteral("off");
    case StillMode::Cleanest: return QStringLiteral("cleanest");
    case StillMode::Average: return QStringLiteral("average");
    }
    return QStringLiteral("off");
}

StillMode stillModeFromName(const QString &name, StillMode fallback)
{
    const QString key = name.trimmed().toLower();
    if (key == QLatin1String("off")) return StillMode::Off;
    if (key == QLatin1String("cleanest")) return StillMode::Cleanest;
    if (key == QLatin1String("average")) return StillMode::Average;
    return fallback;
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

SearchResult findStillFrames(const SearchInput &input, std::atomic<bool> *cancel, std::atomic<qint32> *progress)
{
    SearchResult result;
    const TbcMetaData::VideoParameters &videoParameters = input.videoParameters;
    const qint32 fieldWidth = videoParameters.fieldWidth;
    const qint32 fieldHeight = videoParameters.fieldHeight;

    SourceVideo sourceVideo;
    if (fieldWidth <= 0 || fieldHeight <= 0
        || !sourceVideo.open(input.tbcFilename, fieldWidth * fieldHeight, fieldWidth)) {
        result.errorMessage = QStringLiteral("Could not open %1 for the still-picture search.").arg(input.tbcFilename);
        return result;
    }

    // Frame row r comes from the first field (even r) or second field (odd r),
    // field line r / 2. Only the field lines inside the crop are read.
    const QRect crop = input.cropRect.intersected(QRect(0, 0, fieldWidth, fieldHeight * 2 - 1));
    const qint32 x0 = crop.left();
    const qint32 width = crop.width();
    const qint32 line0 = (crop.top() + 1) / 2;
    const qint32 height = std::min(fieldHeight, (crop.bottom() + 1) / 2) - line0;
    if (width < THUMBNAIL_BLOCK || height < THUMBNAIL_BLOCK) {
        result.errorMessage = QStringLiteral("The framed area is too small to search.");
        return result;
    }
    const qint32 lastFrame = input.firstFrame + input.fieldNumbers.size() - 1;
    if (input.anchorFrame < input.firstFrame || input.anchorFrame > lastFrame) {
        result.errorMessage = QStringLiteral("The current frame is outside the search window.");
        return result;
    }

    const float black = videoParameters.black16bIre;
    const float range = (videoParameters.white16bIre > videoParameters.black16bIre)
                            ? videoParameters.white16bIre - videoParameters.black16bIre : 65535.0f;

    // Both fields of a frame, one after the other, scaled 0 (black) to 1 (white)
    const qint32 fieldPixels = width * height;
    auto readFrame = [&](qint32 frame) {
        std::vector<float> pixels(2 * fieldPixels);
        const QPair<qint32, qint32> &fields = input.fieldNumbers[frame - input.firstFrame];
        for (qint32 f = 0; f < 2; f++) {
            const SourceVideo::Data field = sourceVideo.getVideoField(f == 0 ? fields.first : fields.second);
            for (qint32 y = 0; y < height; y++) {
                const quint16 *source = field.constData() + (line0 + y) * fieldWidth + x0;
                float *target = pixels.data() + f * fieldPixels + y * width;
                for (qint32 x = 0; x < width; x++) target[x] = (source[x] - black) / range;
            }
        }
        return pixels;
    };

    // 8x8-block means of the first field
    const qint32 blocksX = width / THUMBNAIL_BLOCK;
    const qint32 blocksY = height / THUMBNAIL_BLOCK;
    auto thumbnail = [&](const std::vector<float> &pixels) {
        std::vector<double> blocks(blocksX * blocksY, 0.0);
        for (qint32 y = 0; y < blocksY * THUMBNAIL_BLOCK; y++) {
            for (qint32 x = 0; x < blocksX * THUMBNAIL_BLOCK; x++) {
                blocks[(y / THUMBNAIL_BLOCK) * blocksX + x / THUMBNAIL_BLOCK] += pixels[y * width + x];
            }
        }
        for (double &block : blocks) block /= THUMBNAIL_BLOCK * THUMBNAIL_BLOCK;
        return blocks;
    };

    // Walk outwards from the anchor until the picture changes, keeping the
    // pixels of every frame of the run
    std::vector<double> anchorThumbnail;
    QVector<FrameScore> scores;
    std::vector<std::vector<float>> runPixels;
    QHash<qint32, size_t> runIndexOfFrame;
    auto visit = [&](qint32 frame) {
        FrameScore score;
        score.frame = frame;
        const qint32 index = frame - input.firstFrame;
        score.dropouts = (index < input.visibleDropouts.size()) ? input.visibleDropouts[index] : 0.0;
        std::vector<float> pixels = readFrame(frame);
        const std::vector<double> frameThumbnail = thumbnail(pixels);
        if (anchorThumbnail.empty()) anchorThumbnail = frameThumbnail;
        double difference = 0.0;
        for (size_t i = 0; i < frameThumbnail.size(); i++) difference += std::fabs(frameThumbnail[i] - anchorThumbnail[i]);
        score.anchorDiff = difference / frameThumbnail.size();
        score.inRun = score.anchorDiff < RUN_BREAK_DIFFERENCE;
        if (score.inRun) {
            runIndexOfFrame.insert(frame, runPixels.size());
            runPixels.push_back(std::move(pixels));
        }
        scores.append(score);
        if (progress) progress->fetch_add(1);
        return score.inRun;
    };

    visit(input.anchorFrame);
    for (const qint32 step : {-1, 1}) {
        for (qint32 distance = 1; distance <= input.radius; distance++) {
            if (cancel && cancel->load()) {
                result.cancelled = true;
                return result;
            }
            const qint32 frame = input.anchorFrame + step * distance;
            if (frame < input.firstFrame || frame > lastFrame) break;
            if (!visit(frame)) break;
        }
    }

    // Per-pixel median of the run: with dozens of frames of one photo it is
    // close to the noise-free picture
    const size_t runLength = runPixels.size();
    std::vector<float> medianPixels(2 * fieldPixels);
    std::vector<float> samples(runLength);
    for (size_t i = 0; i < medianPixels.size(); i++) {
        for (size_t f = 0; f < runLength; f++) samples[f] = runPixels[f][i];
        std::nth_element(samples.begin(), samples.begin() + runLength / 2, samples.end());
        medianPixels[i] = samples[runLength / 2];
    }

    // Misalignment of every frame of the run against the median, per field:
    // one horizontal shift per band (3-band median smoothed) and one vertical
    // shift. A frame beyond ALIGN_THRESHOLD is compensated before its distance
    // is taken, so rejection judges damage, not position.
    QHash<qint32, FrameAlignment> alignmentOfFrame;
    const qint32 bandHeight = std::min(ALIGN_BAND_LINES, height);
    const qint32 bands = height / bandHeight;
    QVector<FrameScore *> runScores;
    for (FrameScore &score : scores) {
        if (score.inRun) runScores.append(&score);
    }
    std::vector<FrameAlignment> alignments(runScores.size());
    auto alignOne = [&](qint32 index) {
        FrameScore &score = *runScores[index];
        std::vector<float> &pixels = runPixels[runIndexOfFrame.value(score.frame)];
        FrameAlignment &alignment = alignments[index];
        alignment.frame = score.frame;
        alignment.firstFieldLine = line0;
        alignment.bandHeight = bandHeight;
        QVector<double> bandShifts;
        for (qint32 f = 0; f < 2; f++) {
            const float *image = pixels.data() + static_cast<size_t>(f) * fieldPixels;
            const float *reference = medianPixels.data() + static_cast<size_t>(f) * fieldPixels;
            QVector<float> raw(bands);
            for (qint32 b = 0; b < bands; b++) {
                const size_t offset = static_cast<size_t>(b) * bandHeight * width;
                raw[b] = static_cast<float>(estimateShift(image + offset, reference + offset, bandHeight, width, width, 1));
            }
            alignment.shiftX[f].resize(bands);
            for (qint32 b = 0; b < bands; b++) {
                float window[3] = {raw[std::max(0, b - 1)], raw[b], raw[std::min(bands - 1, b + 1)]};
                std::sort(window, window + 3);
                alignment.shiftX[f][b] = window[1];
                bandShifts.append(window[1]);
                score.shiftXMax = std::max(score.shiftXMax, static_cast<double>(std::fabs(window[1])));
            }
            alignment.shiftY[f] = static_cast<float>(estimateShift(image, reference, width, 1, height, width));
            score.shiftYMax = std::max(score.shiftYMax, static_cast<double>(std::fabs(alignment.shiftY[f])));
        }
        score.shiftX = median(bandShifts);
        alignment.apply = score.shiftXMax > ALIGN_THRESHOLD || score.shiftYMax > ALIGN_THRESHOLD;
        score.aligned = alignment.apply;
        if (alignment.apply) {
            for (qint32 f = 0; f < 2; f++) {
                float *field = pixels.data() + static_cast<size_t>(f) * fieldPixels;
                const std::vector<float> resampled = resampleField(field, width, height, 1, alignment, f, line0);
                std::copy(resampled.begin(), resampled.end(), field);
            }
        }
    };
    // Frames are independent: each thread takes every n-th one
    const qint32 threadCount = std::max(1, std::min(static_cast<qint32>(std::thread::hardware_concurrency()),
                                                    static_cast<qint32>(runScores.size())));
    std::vector<std::thread> threads;
    for (qint32 t = 0; t < threadCount; t++) {
        threads.emplace_back([&, t]() {
            for (qint32 index = t; index < runScores.size(); index += threadCount) alignOne(index);
        });
    }
    for (std::thread &thread : threads) thread.join();
    for (const FrameAlignment &alignment : alignments) alignmentOfFrame.insert(alignment.frame, alignment);

    // Distance of each frame of the run from the median
    QVector<double> runDistances;
    QVector<double> runDropouts;
    for (FrameScore &score : scores) {
        if (!score.inRun) continue;
        const std::vector<float> &pixels = runPixels[runIndexOfFrame.value(score.frame)];
        double sum = 0.0;
        for (size_t i = 0; i < pixels.size(); i++) {
            const double difference = pixels[i] - medianPixels[i];
            sum += difference * difference;
        }
        score.distance = std::sqrt(sum / pixels.size());
        runDistances.append(score.distance);
        runDropouts.append(score.dropouts);
    }

    const double distanceLimit = median(runDistances) * DISTANCE_TOLERANCE;
    const double runDropoutMedian = median(runDropouts);
    const double dropoutLimit = runDropoutMedian + std::max(DROPOUT_ALLOWANCE_SAMPLES, runDropoutMedian);

    std::sort(scores.begin(), scores.end(), [](const FrameScore &a, const FrameScore &b) { return a.frame < b.frame; });
    double bestDistance = -1.0;
    result.bestFrame = input.anchorFrame;
    for (FrameScore &score : scores) {
        score.eligible = score.inRun && score.distance <= distanceLimit && score.dropouts <= dropoutLimit;
        if (!score.eligible) continue;
        result.eligibleFrames.append(score.frame);
        result.alignments.append(alignmentOfFrame.value(score.frame));
        if (bestDistance < 0.0 || score.distance < bestDistance) {
            bestDistance = score.distance;
            result.bestFrame = score.frame;
        }
    }
    result.scores = scores;
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
    stream << "frame,in_run,eligible,anchor_diff,distance,dropouts,shift_x,shift_x_max,shift_y_max,aligned,best\n";
    for (const FrameScore &score : result.scores) {
        stream << score.frame << ',' << int(score.inRun) << ',' << int(score.eligible) << ','
               << score.anchorDiff << ',' << score.distance << ',' << score.dropouts << ','
               << score.shiftX << ',' << score.shiftXMax << ',' << score.shiftYMax << ',' << int(score.aligned) << ','
               << int(score.frame == result.bestFrame) << '\n';
    }
    return true;
}

QImage alignFrame(const QImage &frameImage, const FrameAlignment &alignment)
{
    const QImage source = frameImage.convertToFormat(QImage::Format_RGB32);
    const qint32 width = source.width();
    const qint32 height = source.height();
    QImage aligned(source.size(), QImage::Format_RGB32);

    // Each field on its own: frame row = 2 * field line + field
    for (qint32 field = 0; field < 2; field++) {
        const qint32 rows = (height - field + 1) / 2;
        std::vector<float> data(static_cast<size_t>(width) * rows * 3);
        for (qint32 row = 0; row < rows; row++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(2 * row + field));
            float *target = data.data() + static_cast<size_t>(row) * width * 3;
            for (qint32 x = 0; x < width; x++) {
                target[3 * x + 0] = qRed(line[x]);
                target[3 * x + 1] = qGreen(line[x]);
                target[3 * x + 2] = qBlue(line[x]);
            }
        }
        const std::vector<float> resampled = resampleField(data.data(), width, rows, 3, alignment, field, 0);
        for (qint32 row = 0; row < rows; row++) {
            QRgb *line = reinterpret_cast<QRgb *>(aligned.scanLine(2 * row + field));
            const float *value = resampled.data() + static_cast<size_t>(row) * width * 3;
            for (qint32 x = 0; x < width; x++) {
                line[x] = qRgb(std::clamp(static_cast<int>(std::lround(value[3 * x + 0])), 0, 255),
                               std::clamp(static_cast<int>(std::lround(value[3 * x + 1])), 0, 255),
                               std::clamp(static_cast<int>(std::lround(value[3 * x + 2])), 0, 255));
            }
        }
    }
    return aligned;
}

QImage averageFrames(const QVector<qint32> &frames, const std::function<QImage(qint32)> &render,
                     std::atomic<bool> *cancel, std::atomic<qint32> *progress,
                     const QVector<FrameAlignment> &alignments)
{
    QHash<qint32, const FrameAlignment *> alignmentOfFrame;
    for (const FrameAlignment &alignment : alignments) {
        if (alignment.apply) alignmentOfFrame.insert(alignment.frame, &alignment);
    }

    // Averaging the decoded pictures, not the TBC samples: NTSC's subcarrier
    // phase inverts every frame, so averaging raw samples cancels the colour
    QSize size;
    std::vector<quint32> sums;
    qint32 count = 0;
    for (const qint32 frame : frames) {
        if (cancel && cancel->load()) return QImage();
        QImage image = render(frame).convertToFormat(QImage::Format_RGB32);
        if (!image.isNull() && alignmentOfFrame.contains(frame)) {
            image = alignFrame(image, *alignmentOfFrame.value(frame));
        }
        if (progress) progress->fetch_add(1);
        if (image.isNull()) continue;
        if (size.isEmpty()) {
            size = image.size();
            sums.assign(static_cast<size_t>(size.width()) * size.height() * 3, 0);
        }
        if (image.size() != size) continue;

        quint32 *sum = sums.data();
        for (qint32 y = 0; y < size.height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (qint32 x = 0; x < size.width(); x++) {
                *sum++ += qRed(line[x]);
                *sum++ += qGreen(line[x]);
                *sum++ += qBlue(line[x]);
            }
        }
        count++;
    }
    if (count == 0) return QImage();

    QImage average(size, QImage::Format_RGB32);
    const quint32 half = static_cast<quint32>(count) / 2;
    const quint32 *sum = sums.data();
    for (qint32 y = 0; y < size.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(average.scanLine(y));
        for (qint32 x = 0; x < size.width(); x++) {
            const quint32 red = (sum[0] + half) / count;
            const quint32 green = (sum[1] + half) / count;
            const quint32 blue = (sum[2] + half) / count;
            line[x] = qRgb(int(red), int(green), int(blue));
            sum += 3;
        }
    }
    return average;
}

} // namespace FrameSnapshot
