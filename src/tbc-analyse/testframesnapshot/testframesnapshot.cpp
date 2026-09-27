/******************************************************************************
 * testframesnapshot.cpp
 * tbc-analyse - "Save frame as PNG" pipeline unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <QFile>
#include <QCoreApplication>
#include <QTemporaryDir>

#include "framesnapshot.h"

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

using FrameSnapshot::AspectMode;
using FrameSnapshot::Framing;
using FrameSnapshot::Options;

bool near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

TbcMetaData::VideoParameters ntscParameters()
{
    TbcMetaData::VideoParameters parameters;
    parameters.system = NTSC;
    parameters.fieldWidth = 910;
    parameters.fieldHeight = 263;
    parameters.sampleRate = 4.0 * 315.0e6 / 88.0; // 4fsc NTSC, 14.318 MHz
    parameters.activeVideoStart = 134;
    parameters.activeVideoEnd = 894;
    parameters.firstActiveFrameLine = 40;
    parameters.lastActiveFrameLine = 525;
    parameters.black16bIre = 16384;
    parameters.white16bIre = 54016;
    return parameters;
}

TbcMetaData::VideoParameters palParameters()
{
    TbcMetaData::VideoParameters parameters;
    parameters.system = PAL;
    parameters.fieldWidth = 1135;
    parameters.fieldHeight = 313;
    parameters.sampleRate = 17734475.0;
    parameters.activeVideoStart = 185;
    parameters.activeVideoEnd = 1107;
    parameters.firstActiveFrameLine = 44;
    parameters.lastActiveFrameLine = 620;
    return parameters;
}

void testActiveRectAndMargins()
{
    const TbcMetaData::VideoParameters ntsc = ntscParameters();
    const QSize ntscFrame(910, 525);
    CHECK(FrameSnapshot::activeFrameRect(ntsc, ntscFrame) == QRect(134, 40, 760, 485));

    const TbcMetaData::VideoParameters pal = palParameters();
    CHECK(FrameSnapshot::activeFrameRect(pal, QSize(1135, 625)) == QRect(185, 44, 922, 576));

    // The default trims 12 lines of head switching off the bottom
    Options options;
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(134, 40, 760, 473));

    options.framing = Framing::Full;
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(0, 0, 910, 513));

    options.framing = Framing::Active;
    options.marginLeft = 10;
    options.marginTop = 4;
    options.marginRight = 20;
    options.marginBottom = 6;
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(144, 44, 730, 475));

    // Margins that eat the whole picture fall back to the untrimmed framing
    options.marginLeft = 800;
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(134, 40, 760, 485));

    // A custom rectangle is clamped to the frame; an empty one means active
    options = Options();
    options.framing = Framing::Custom;
    options.customRect = QRect(800, 500, 400, 400);
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(800, 500, 110, 13));
    options.customRect = QRect();
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(134, 40, 760, 473));

    // No active area in the metadata: active framing is the full frame
    TbcMetaData::VideoParameters unknown = ntsc;
    unknown.firstActiveFrameLine = -1;
    CHECK(FrameSnapshot::activeFrameRect(unknown, ntscFrame).isNull());
    CHECK(FrameSnapshot::outputRect(Options(), unknown, ntscFrame) == QRect(0, 0, 910, 513));
}

void testPixelAspect()
{
    const TbcMetaData::VideoParameters ntsc = ntscParameters();
    CHECK(near(FrameSnapshot::pixelAspect(AspectMode::Exact, ntsc), 6.0 / 7.0, 1e-9));
    CHECK(near(FrameSnapshot::pixelAspect(AspectMode::Viewer, ntsc), 760.0 / 910.0, 1e-9));

    const TbcMetaData::VideoParameters pal = palParameters();
    CHECK(near(FrameSnapshot::pixelAspect(AspectMode::Exact, pal), 14.75e6 / 17734475.0, 1e-9));

    TbcMetaData::VideoParameters wide = pal;
    wide.isWidescreen = true;
    CHECK(near(FrameSnapshot::pixelAspect(AspectMode::Exact, wide), 14.75e6 / 17734475.0 * 4.0 / 3.0, 1e-9));

    // SECAM is 625-line
    TbcMetaData::VideoParameters secam = pal;
    secam.system = SECAM;
    CHECK(near(FrameSnapshot::pixelAspect(AspectMode::Exact, secam), 14.75e6 / 17734475.0, 1e-9));
}

void testOutputSize()
{
    const TbcMetaData::VideoParameters ntsc = ntscParameters();
    const QSize active(760, 480);

    Options options;
    CHECK(FrameSnapshot::outputSize(options, ntsc, active) == QSize(651, 480));
    options.aspectMode = AspectMode::Viewer;
    CHECK(FrameSnapshot::outputSize(options, ntsc, active) == QSize(635, 480));

    options.aspectMode = AspectMode::Exact;
    options.upscaleFactor = 4;
    CHECK(FrameSnapshot::outputSize(options, ntsc, active) == QSize(2606, 1920));
}

void testProcessWithoutUpscale()
{
    const TbcMetaData::VideoParameters ntsc = ntscParameters();
    QImage frame(910, 525, QImage::Format_RGB32);
    frame.fill(Qt::gray);

    QString error;
    const QImage exact = FrameSnapshot::process(frame, Options(), ntsc, &error);
    CHECK(exact.size() == QSize(651, 473));

    Options viewer;
    viewer.aspectMode = AspectMode::Viewer;
    CHECK(FrameSnapshot::process(frame, viewer, ntsc, &error).size() == QSize(635, 473));
}

void testUpscale()
{
    const TbcMetaData::VideoParameters ntsc = ntscParameters();
    QImage frame(910, 525, QImage::Format_RGB32);
    for (qint32 y = 0; y < frame.height(); y++) {
        for (qint32 x = 0; x < frame.width(); x++) {
            frame.setPixel(x, y, ((x / 4 + y / 4) % 2) ? qRgb(230, 200, 40) : qRgb(20, 40, 160));
        }
    }

    const QVector<FrameSnapshot::UpscaleMethod> methods = FrameSnapshot::upscaleMethods();
    CHECK(!methods.isEmpty());
#if defined(TBC_HAVE_OPENCV)
    CHECK(methods.constFirst().name == QLatin1String("lanczos4"));
#endif

    // 760x473 trimmed active area, 2x, then 6/7 for square pixels
    QString error;
    Options options;
    options.upscaleFactor = 2;
    CHECK(FrameSnapshot::process(frame, options, ntsc, &error).size() == QSize(1303, 946));

    // An unknown method falls back to the first available one
    options.upscaleMethod = QStringLiteral("no-such-method");
    CHECK(FrameSnapshot::process(frame, options, ntsc, &error).size() == QSize(1303, 946));

    // Every method, including each installed learned model, gives the exact size
    const QImage small = frame.copy(200, 100, 64, 48);
    for (const FrameSnapshot::UpscaleMethod &method : methods) {
        for (const qint32 factor : {2, 3, 4}) {
            const QImage upscaled = FrameSnapshot::upscale(small, factor, method.name, &error);
            if (upscaled.size() != small.size() * factor) {
                std::cerr << "upscale " << method.name.toStdString() << " x" << factor << ": "
                          << error.toStdString() << "\n";
            }
            CHECK(upscaled.size() == small.size() * factor);
        }
    }
    std::cerr << "Upscale methods checked: " << methods.size() << "\n";
}

// Frames 1-9 hold picture A, frames 10-12 picture B. Frame 4 is the quietest
// copy of A; frame 6 is quieter still but carries a head-switching-style
// tear, the high-contrast artifact a sharpness score would have chosen.
void testStillSearch()
{
    const qint32 fieldWidth = 96;
    const qint32 fieldHeight = 48;
    const qint32 frames = 12;
    const double black = 16384.0;
    const double white = 54016.0;

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString tbcPath = directory.filePath(QStringLiteral("synthetic.tbc"));
    QFile file(tbcPath);
    CHECK(file.open(QIODevice::WriteOnly));
    for (qint32 frame = 1; frame <= frames; frame++) {
        const bool pictureB = frame >= 10;
        const double sigma = (frame == 4 || frame == 6 || pictureB) ? 0.004 : 0.02;
        for (qint32 field = 0; field < 2; field++) {
            std::mt19937 generator(frame * 2 + field + 11);
            std::normal_distribution<double> noise(0.0, sigma);
            QVector<quint16> samples(fieldWidth * fieldHeight);
            for (qint32 line = 0; line < fieldHeight; line++) {
                for (qint32 x = 0; x < fieldWidth; x++) {
                    const qint32 frameLine = line * 2 + field;
                    double base = pictureB ? ((x / 12 + frameLine / 12) % 2 ? 0.85 : 0.15)
                                           : ((x / 8) % 2 ? 0.7 : 0.3) + 0.1 * std::sin(frameLine * 0.3);
                    if (frame == 6 && line >= 40 && line < 43) base = (x / 3) % 2 ? 0.95 : 0.05;
                    const double value = black + (base + noise(generator)) * (white - black);
                    samples[line * fieldWidth + x] = quint16(qBound(0.0, value, 65535.0));
                }
            }
            file.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
        }
    }
    file.close();

    FrameSnapshot::SearchInput input;
    input.tbcFilename = tbcPath;
    input.videoParameters.system = NTSC;
    input.videoParameters.fieldWidth = fieldWidth;
    input.videoParameters.fieldHeight = fieldHeight;
    input.videoParameters.black16bIre = qint32(black);
    input.videoParameters.white16bIre = qint32(white);
    input.anchorFrame = 3;
    input.radius = 8;
    input.cropRect = QRect(0, 0, fieldWidth, fieldHeight * 2 - 1);
    input.firstFrame = 1;
    for (qint32 frame = 1; frame <= frames; frame++) {
        input.fieldNumbers.append({frame * 2 - 1, frame * 2});
        input.visibleDropouts.append(0.0);
    }

    const FrameSnapshot::SearchResult result = FrameSnapshot::findStillFrames(input);
    CHECK(result.errorMessage.isEmpty());
    for (const FrameSnapshot::FrameScore &score : result.scores) {
        CHECK(score.inRun == (score.frame <= 9));
    }
    // The walk stops at the first frame of picture B
    CHECK(result.scores.last().frame == 10);
    // The torn frame is rejected; the quietest untorn frame is the cleanest
    CHECK(!result.eligibleFrames.contains(6));
    CHECK(result.bestFrame == 4);
    CHECK(result.eligibleFrames == QVector<qint32>({1, 2, 3, 4, 5, 7, 8, 9}));

    // A frame with heavy visible dropouts is rejected however clean it looks
    input.visibleDropouts[3] = 500.0;
    const FrameSnapshot::SearchResult withDropouts = FrameSnapshot::findStillFrames(input);
    CHECK(withDropouts.bestFrame != 4);
    CHECK(!withDropouts.eligibleFrames.contains(4));
}

// Smooth per-field pattern, so sub-sample shifts can be synthesised exactly
double pattern(double x, double fieldLine, qint32 field)
{
    return 0.5 + 0.2 * std::sin(2.0 * M_PI * x / 17.0 + field) + 0.15 * std::sin(2.0 * M_PI * fieldLine / 9.0);
}

// Frames 1-8 show one picture. Frame 3 sits 0.8 samples right, frame 5 only
// 0.1 (within tolerance), frame 6 0.6 field lines down. The search must
// measure each, realign only 3 and 6, and keep them all: misalignment is
// compensated before the damage check.
void testStillAlignment()
{
    const qint32 fieldWidth = 160;
    const qint32 fieldHeight = 72;
    const qint32 frames = 8;
    const double black = 16384.0;
    const double white = 54016.0;

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString tbcPath = directory.filePath(QStringLiteral("shifted.tbc"));
    QFile file(tbcPath);
    CHECK(file.open(QIODevice::WriteOnly));
    for (qint32 frame = 1; frame <= frames; frame++) {
        const double dx = frame == 3 ? 0.8 : frame == 5 ? 0.1 : 0.0;
        const double dy = frame == 6 ? 0.6 : 0.0;
        for (qint32 field = 0; field < 2; field++) {
            std::mt19937 generator(frame * 2 + field + 101);
            std::normal_distribution<double> noise(0.0, 0.005);
            QVector<quint16> samples(fieldWidth * fieldHeight);
            for (qint32 line = 0; line < fieldHeight; line++) {
                for (qint32 x = 0; x < fieldWidth; x++) {
                    const double value = black + (pattern(x - dx, line - dy, field) + noise(generator)) * (white - black);
                    samples[line * fieldWidth + x] = quint16(qBound(0.0, value, 65535.0));
                }
            }
            file.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
        }
    }
    file.close();

    FrameSnapshot::SearchInput input;
    input.tbcFilename = tbcPath;
    input.videoParameters.system = NTSC;
    input.videoParameters.fieldWidth = fieldWidth;
    input.videoParameters.fieldHeight = fieldHeight;
    input.videoParameters.black16bIre = qint32(black);
    input.videoParameters.white16bIre = qint32(white);
    input.anchorFrame = 1;
    input.radius = 7;
    input.cropRect = QRect(0, 0, fieldWidth, fieldHeight * 2 - 1);
    input.firstFrame = 1;
    for (qint32 frame = 1; frame <= frames; frame++) {
        input.fieldNumbers.append({frame * 2 - 1, frame * 2});
        input.visibleDropouts.append(0.0);
    }

    const FrameSnapshot::SearchResult result = FrameSnapshot::findStillFrames(input);
    CHECK(result.errorMessage.isEmpty());
    CHECK(result.eligibleFrames.size() == frames);
    CHECK(result.alignments.size() == frames);
    for (const FrameSnapshot::FrameScore &score : result.scores) {
        const double expectedX = score.frame == 3 ? 0.8 : score.frame == 5 ? 0.1 : 0.0;
        const double expectedY = score.frame == 6 ? 0.6 : 0.0;
        if (!near(score.shiftX, expectedX, 0.05) || !near(score.shiftYMax, expectedY, 0.05)) {
            std::cerr << "frame " << score.frame << " shiftX " << score.shiftX << " shiftYMax " << score.shiftYMax << "\n";
        }
        CHECK(near(score.shiftX, expectedX, 0.05));
        CHECK(near(score.shiftYMax, expectedY, 0.05));
        CHECK(score.aligned == (score.frame == 3 || score.frame == 6));
    }
}

// Shifting a frame image and aligning it back recovers the original
void testAlignFrame()
{
    const qint32 width = 160;
    const qint32 height = 101;
    auto render = [&](double dx, double dy) {
        QImage image(width, height, QImage::Format_RGB32);
        for (qint32 y = 0; y < height; y++) {
            for (qint32 x = 0; x < width; x++) {
                const double v = pattern(x - dx, y / 2 - dy, y % 2);
                const int grey = std::clamp(int(std::lround(v * 255)), 0, 255);
                image.setPixel(x, y, qRgb(grey, 255 - grey, grey / 2));
            }
        }
        return image;
    };
    FrameSnapshot::FrameAlignment alignment;
    alignment.bandHeight = 16;
    for (qint32 f = 0; f < 2; f++) {
        alignment.shiftX[f] = QVector<float>(3, 0.7f);
        alignment.shiftY[f] = 0.4f;
    }
    alignment.apply = true;

    const QImage original = render(0.0, 0.0);
    const QImage aligned = FrameSnapshot::alignFrame(render(0.7, 0.4), alignment);
    const QImage unaligned = render(0.7, 0.4);
    auto rms = [&](const QImage &a) {
        double sum = 0.0;
        qint32 count = 0;
        for (qint32 y = 8; y < height - 8; y++) {
            for (qint32 x = 8; x < width - 8; x++) {
                const double d = qRed(a.pixel(x, y)) - qRed(original.pixel(x, y));
                sum += d * d;
                count++;
            }
        }
        return std::sqrt(sum / count);
    };
    CHECK(rms(aligned) < 1.0);
    CHECK(rms(unaligned) > 5.0);
}

void testAverageFrames()
{
    auto solid = [](int value, QSize size = QSize(8, 4)) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(qRgb(value, 255 - value, value / 2));
        return image;
    };

    // Per-channel mean with rounding; wrong-sized and null frames are skipped
    const QImage average = FrameSnapshot::averageFrames({1, 2, 3, 4, 5}, [&](qint32 frame) {
        switch (frame) {
        case 1: return solid(10);
        case 2: return solid(20);
        case 3: return solid(31);
        case 4: return solid(200, QSize(4, 4));
        default: return QImage();
        }
    });
    CHECK(average.size() == QSize(8, 4));
    CHECK(average.pixel(3, 2) == qRgb(20, 235, 10));

    std::atomic<bool> cancel(true);
    CHECK(FrameSnapshot::averageFrames({1}, [&](qint32) { return solid(1); }, &cancel).isNull());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    testActiveRectAndMargins();
    testPixelAspect();
    testOutputSize();
    testProcessWithoutUpscale();
    testUpscale();
    testStillSearch();
    testStillAlignment();
    testAlignFrame();
    testAverageFrames();

    std::cerr << "All frame snapshot tests passed\n";
    return 0;
}
