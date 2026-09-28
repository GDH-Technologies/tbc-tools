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
#include "slideshowextract.h"

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
    // Picture B never gives way to picture A again: the walk reads on through
    // it (it could be an interruption) to the end of its radius, all out of the run
    CHECK(result.scores.last().frame == 11);
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
    // "Cleanest" saves a frame unresampled, so a frame that needed
    // realignment must not win on its resampled (smoother) distance
    CHECK(result.bestFrame != 3 && result.bestFrame != 6);
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

// Frames the TBC repeats exactly count once; without that, the copies become
// the median and every real frame is rejected as far from it
void testStillDuplicates()
{
    const qint32 fieldWidth = 96;
    const qint32 fieldHeight = 48;
    const qint32 frames = 12;
    const double black = 16384.0;
    const double white = 54016.0;

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString tbcPath = directory.filePath(QStringLiteral("repeats.tbc"));
    QFile file(tbcPath);
    CHECK(file.open(QIODevice::WriteOnly));
    for (qint32 frame = 1; frame <= frames; frame++) {
        // Frames 5 to 9 repeat frame 4
        const qint32 source = (frame >= 5 && frame <= 9) ? 4 : frame;
        for (qint32 field = 0; field < 2; field++) {
            std::mt19937 generator(source * 2 + field + 5);
            std::normal_distribution<double> noise(0.0, 0.02);
            QVector<quint16> samples(fieldWidth * fieldHeight);
            for (qint32 line = 0; line < fieldHeight; line++) {
                for (qint32 x = 0; x < fieldWidth; x++) {
                    const double base = ((x / 8) % 2 ? 0.7 : 0.3) + 0.1 * std::sin((line * 2 + field) * 0.3);
                    samples[line * fieldWidth + x] = quint16(qBound(0.0, black + (base + noise(generator)) * (white - black), 65535.0));
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
    input.anchorFrame = 6;
    input.radius = 8;
    input.cropRect = QRect(0, 0, fieldWidth, fieldHeight * 2 - 1);
    for (qint32 frame = 1; frame <= frames; frame++) {
        input.fieldNumbers.append({frame * 2 - 1, frame * 2});
        input.visibleDropouts.append(0.0);
    }

    const FrameSnapshot::SearchResult result = FrameSnapshot::findStillFrames(input);
    CHECK(result.errorMessage.isEmpty());
    for (const FrameSnapshot::FrameScore &score : result.scores) {
        CHECK(score.duplicate == (score.frame >= 5 && score.frame <= 9));
    }
    CHECK(result.eligibleFrames.size() >= 6);
    for (const qint32 frame : result.eligibleFrames) CHECK(frame < 5 || frame > 9);
}

void testLumaReader()
{
    const qint32 fieldWidth = 64;
    const qint32 fieldHeight = 40;
    const qint32 fields = 4;
    const double black = 16384.0;
    const double white = 54016.0;
    // Luma 0..0.49 that depends on the field, the line and the sample
    auto value = [](qint32 field, qint32 line, qint32 x) { return ((x + line + field * 7) % 50) / 100.0; };

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString tbcPath = directory.filePath(QStringLiteral("ramp.tbc"));
    QFile file(tbcPath);
    CHECK(file.open(QIODevice::WriteOnly));
    for (qint32 field = 0; field < fields; field++) {
        QVector<quint16> samples(fieldWidth * fieldHeight);
        for (qint32 line = 0; line < fieldHeight; line++) {
            for (qint32 x = 0; x < fieldWidth; x++) {
                samples[line * fieldWidth + x] = quint16(qRound(black + value(field, line, x) * (white - black)));
            }
        }
        file.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
    }
    file.close();

    TbcMetaData::VideoParameters parameters;
    parameters.system = NTSC;
    parameters.fieldWidth = fieldWidth;
    parameters.fieldHeight = fieldHeight;
    parameters.black16bIre = qint32(black);
    parameters.white16bIre = qint32(white);

    // Frame rows 10..49 are field lines 5..24; samples 8..39
    FrameSnapshot::LumaReader reader;
    CHECK(reader.open(tbcPath, parameters, QRect(8, 10, 32, 40), nullptr));
    CHECK(reader.width() == 32);
    CHECK(reader.firstFieldLine() == 5);
    CHECK(reader.height() == 20);
    std::vector<float> area(reader.fieldPixels());
    CHECK(reader.readField(3, area.data()));
    for (qint32 y = 0; y < reader.height(); y++) {
        for (qint32 x = 0; x < reader.width(); x++) {
            CHECK(near(area[y * reader.width() + x], value(2, y + 5, x + 8), 1e-4));
        }
    }
    const std::vector<double> blocks = reader.thumbnail(area.data());
    CHECK(blocks.size() == 4 * 2);
    double firstBlock = 0.0;
    for (qint32 y = 0; y < 8; y++) {
        for (qint32 x = 0; x < 8; x++) firstBlock += area[y * reader.width() + x];
    }
    CHECK(near(blocks[0], firstBlock / 64.0, 1e-9));

    // A field past the end of the file reads as zeros, not a crash
    CHECK(!reader.readField(fields + 1, area.data()));
    CHECK(area[0] == 0.0f);

    CHECK(!reader.open(tbcPath, parameters, QRect(0, 0, 4, 4), nullptr));

    // The scan's samples chain their differences from the previous frame
    SlideshowExtract::ScanInput input;
    input.tbcFilename = tbcPath;
    input.videoParameters = parameters;
    input.cropRect = QRect(0, 0, fieldWidth, fieldHeight * 2 - 1);
    input.fieldNumbers = {{1, 2}, {3, 4}};
    const SlideshowExtract::ScanResult scan = SlideshowExtract::scan(input);
    CHECK(scan.errorMessage.isEmpty());
    CHECK(scan.samples.size() == 2);
    CHECK(scan.samples[0].difference == 1.0);
    CHECK(near(scan.samples[1].difference,
               SlideshowExtract::thumbnailDifference(scan.samples[1].thumbnail, scan.samples[0].thumbnail), 1e-12));

    const QVector<QImage> previews = SlideshowExtract::framePreviews(input, {2, 9}, 32);
    CHECK(previews.size() == 2);
    CHECK(previews[0].width() == 32);
    CHECK(previews[1].isNull());
}

// Synthetic scan samples: 8x8-block thumbnails, as the scan would store them
QVector<quint8> pattern(quint32 seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> level(30, 230);
    QVector<quint8> thumbnail(64);
    for (quint8 &block : thumbnail) block = quint8(level(generator));
    return thumbnail;
}

QVector<quint8> blend(const QVector<quint8> &a, const QVector<quint8> &b, double t)
{
    QVector<quint8> mixed(a.size());
    for (qint32 i = 0; i < a.size(); i++) mixed[i] = quint8(qRound(a[i] * (1.0 - t) + b[i] * t));
    return mixed;
}

void testFindHolds()
{
    using SlideshowExtract::FrameSample;
    using SlideshowExtract::Hold;
    using SlideshowExtract::HoldKind;

    std::mt19937 generator(7);
    std::uniform_int_distribution<int> noise(-2, 2);
    QVector<FrameSample> samples;
    auto add = [&](const QVector<quint8> &thumbnail) {
        FrameSample sample;
        sample.thumbnail = thumbnail;
        for (quint8 &block : sample.thumbnail) block = quint8(qBound(0, block + noise(generator), 255));
        if (!samples.isEmpty()) {
            sample.difference = SlideshowExtract::thumbnailDifference(sample.thumbnail, samples.last().thumbnail);
        }
        samples.append(sample);
    };

    const QVector<quint8> a = pattern(1), b = pattern(2), c = pattern(3), e = pattern(5), f = pattern(6),
                          g = pattern(7);
    // A text card: dark with a few bright blocks
    QVector<quint8> card(64, 20);
    for (qint32 i = 20; i < 28; i++) card[i] = 200;
    // A slow pan: a sine pattern sliding sideways
    auto pan = [](qint32 t) {
        QVector<quint8> thumbnail(64);
        for (qint32 i = 0; i < 64; i++) {
            thumbnail[i] = quint8(qRound(128.0 + 90.0 * std::sin(0.9 * (i % 8) + 0.5 * (i / 8) + 0.0067 * t)));
        }
        return thumbnail;
    };

    for (qint32 i = 0; i < 60; i++) add(a);                        // 0-59 photo A
    for (qint32 i = 0; i < 60; i++) add(b);                        // 60-119 cut to B
    for (qint32 i = 1; i <= 30; i++) add(blend(b, c, i / 31.0));   // 120-149 dissolve to C
    for (qint32 i = 0; i < 60; i++) add(c);                        // 150-209 photo C
    for (qint32 i = 0; i < 30; i++) add(QVector<quint8>(64, 3));   // 210-239 black
    for (qint32 i = 0; i < 60; i++) add(card);                     // 240-299 title card
    for (qint32 t = 0; t < 150; t++) add(pan(t));                  // 300-449 pan
    for (qint32 i = 0; i < 20; i++) add(e);                        // 450-469 too short
    for (qint32 i = 0; i < 30; i++) add(f);                        // 470-499 exactly the minimum
    for (qint32 i = 0; i < 29; i++) add(g);                        // 500-528 one short

    const qint32 first = 1001; // frame number of sample 0
    const QVector<Hold> holds = SlideshowExtract::findHolds(samples, first, 30);
    CHECK(holds.size() == 6);

    CHECK(holds[0].first == first && holds[0].last == first + 59);
    CHECK(holds[0].kind == HoldKind::Photo && holds[0].hardStart);

    // B runs into the start of the dissolve, but no further than its first frames
    CHECK(holds[1].first == first + 60 && holds[1].hardStart);
    CHECK(holds[1].last >= first + 119 && holds[1].last <= first + 127);
    CHECK(holds[1].kind == HoldKind::Photo);

    // C picks up the end of the dissolve; the middle of it is no hold
    CHECK(holds[2].last == first + 209 && holds[2].first >= first + 135 && holds[2].first <= first + 150);
    CHECK(holds[2].kind == HoldKind::Photo && !holds[2].hardStart);

    // Black is dropped; the card is kept
    CHECK(holds[3].first == first + 240 && holds[3].last == first + 299);
    CHECK(holds[3].kind == HoldKind::Photo);

    // The pan is listed once, as moving
    CHECK(holds[4].kind == HoldKind::Moving);
    CHECK(holds[4].first == first + 300 && holds[4].last >= first + 400 && holds[4].last <= first + 449);

    // 30 frames is a hold, 20 and 29 are not
    CHECK(holds[5].first == first + 470 && holds[5].last == first + 499);
    CHECK(holds[5].kind == HoldKind::Photo);
    for (const Hold &hold : SlideshowExtract::findHolds(samples, first, 31)) CHECK(hold.first != first + 470);

    CHECK(SlideshowExtract::findHolds({}, first, 30).isEmpty());

    // Glitch bursts the picture comes back from stay inside the hold; a longer
    // break than MAX_INTERRUPTION_FRAMES ends it
    samples.clear();
    const QVector<quint8> glitch = pattern(9);
    for (qint32 i = 0; i < 100; i++) add((i >= 30 && i < 35) || (i >= 60 && i < 70) ? glitch : a); // 0-99
    for (qint32 i = 0; i < 11; i++) add(glitch);                                               // 100-110
    for (qint32 i = 0; i < 40; i++) add(a);                                                    // 111-150
    // A picture that is torn more often than not is unsteady, not a photo
    for (qint32 i = 0; i < 60; i++) add(i % 10 < 4 ? b : glitch);                           // 151-210
    const QVector<Hold> glitched = SlideshowExtract::findHolds(samples, 1, 30);
    CHECK(glitched.size() == 3);
    CHECK(glitched[0].first == 1 && glitched[0].last == 100);
    CHECK(glitched[0].kind == HoldKind::Photo && near(glitched[0].steadiness, 0.85, 1e-9));
    CHECK(glitched[1].first == 112 && glitched[1].last == 151);
    CHECK(glitched[2].first == 152 && glitched[2].kind == HoldKind::Unsteady);

    // A glitch in the middle of a hold is not its anchor
    samples.clear();
    for (qint32 i = 0; i < 60; i++) add(i >= 27 && i < 34 ? glitch : a);
    const QVector<Hold> middleGlitch = SlideshowExtract::findHolds(samples, 1, 30);
    CHECK(middleGlitch.size() == 1);
    CHECK(middleGlitch[0].anchor < 28 || middleGlitch[0].anchor > 34);
    SlideshowExtract::Capture capture;
    capture.frame = 1234;
    capture.framesAveraged = 40;
    Options options;
    options.upscaleFactor = 2;
    CHECK(SlideshowExtract::stillFileName(QStringLiteral("tape"), 7, capture, options)
          == QStringLiteral("tape_still_007_f1234_avg40_up2x_lanczos4.png"));
    CHECK(SlideshowExtract::frameTimecode(1, NTSC) == QStringLiteral("00:00:00:00"));
    CHECK(SlideshowExtract::frameTimecode(25 * 61 + 3, PAL) == QStringLiteral("00:01:01:02"));
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
    testStillDuplicates();
    testLumaReader();
    testFindHolds();

    std::cerr << "All frame snapshot tests passed\n";
    return 0;
}
