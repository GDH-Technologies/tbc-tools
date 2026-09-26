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

    Options options;
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(134, 40, 760, 485));

    options.framing = Framing::Full;
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(0, 0, 910, 525));

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
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(800, 500, 110, 25));
    options.customRect = QRect();
    CHECK(FrameSnapshot::outputRect(options, ntsc, ntscFrame) == QRect(134, 40, 760, 485));

    // No active area in the metadata: active framing is the full frame
    TbcMetaData::VideoParameters unknown = ntsc;
    unknown.firstActiveFrameLine = -1;
    CHECK(FrameSnapshot::activeFrameRect(unknown, ntscFrame).isNull());
    CHECK(FrameSnapshot::outputRect(Options(), unknown, ntscFrame) == QRect(0, 0, 910, 525));
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
    CHECK(exact.size() == QSize(651, 485));

    Options viewer;
    viewer.aspectMode = AspectMode::Viewer;
    CHECK(FrameSnapshot::process(frame, viewer, ntsc, &error).size() == QSize(635, 485));
}

QVector<double> texturedPlane(qint32 width, qint32 height, double blur, double noiseSigma, unsigned seed)
{
    std::mt19937 generator(seed);
    std::normal_distribution<double> noise(0.0, noiseSigma);
    QVector<double> plane(width * height);
    for (qint32 y = 0; y < height; y++) {
        for (qint32 x = 0; x < width; x++) {
            // Sharp edges every 8 samples, softened by 'blur'
            const double phase = std::sin(x * M_PI / 8.0) * std::cos(y * M_PI / 6.0);
            const double edge = std::tanh(phase / std::max(blur, 1e-3));
            plane[y * width + x] = 0.5 + 0.25 * edge + noise(generator);
        }
    }
    return plane;
}

void testFieldMetrics()
{
    const qint32 width = 256;
    const qint32 height = 128;

    // Immerkaer estimate tracks a known sigma on a flat field
    for (const double sigma : {0.01, 0.03}) {
        QVector<double> flat(width * height);
        std::mt19937 generator(7);
        std::normal_distribution<double> noise(0.0, sigma);
        for (double &value : flat) value = 0.5 + noise(generator);
        const FrameSnapshot::FieldMetrics metrics = FrameSnapshot::measureField(flat, width, height);
        CHECK(near(metrics.noise, sigma, sigma * 0.1));
    }

    // Tenengrad ranks a sharp image above a blurred one
    const FrameSnapshot::FieldMetrics sharp = FrameSnapshot::measureField(texturedPlane(width, height, 0.05, 0.0, 1), width, height);
    const FrameSnapshot::FieldMetrics soft = FrameSnapshot::measureField(texturedPlane(width, height, 1.0, 0.0, 1), width, height);
    CHECK(sharp.sharpness > soft.sharpness * 1.5);
}

// Frames 1-6 hold picture A with differing noise, frames 7-10 picture B.
// Searching around frame 3 must stay within A and pick its quietest frame.
void testBestFrameSearch()
{
    const qint32 fieldWidth = 96;
    const qint32 fieldHeight = 48;
    const qint32 frames = 10;
    const double black = 16384.0;
    const double white = 54016.0;
    const double noisePerFrame[frames] = {0.030, 0.020, 0.025, 0.004, 0.030, 0.015, 0.004, 0.004, 0.004, 0.004};

    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString tbcPath = directory.filePath(QStringLiteral("synthetic.tbc"));
    QFile file(tbcPath);
    CHECK(file.open(QIODevice::WriteOnly));
    for (qint32 frame = 0; frame < frames; frame++) {
        const bool pictureB = frame >= 6;
        for (qint32 field = 0; field < 2; field++) {
            std::mt19937 generator(frame * 2 + field + 11);
            std::normal_distribution<double> noise(0.0, noisePerFrame[frame]);
            QVector<quint16> samples(fieldWidth * fieldHeight);
            for (qint32 line = 0; line < fieldHeight; line++) {
                for (qint32 x = 0; x < fieldWidth; x++) {
                    const qint32 frameLine = line * 2 + field;
                    const double base = pictureB ? ((x / 12 + frameLine / 12) % 2 ? 0.85 : 0.15)
                                                 : ((x / 8) % 2 ? 0.7 : 0.3) + 0.1 * std::sin(frameLine * 0.3);
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
    input.radius = 5;
    input.cropRect = QRect(0, 0, fieldWidth, fieldHeight * 2 - 1);
    input.firstFrame = 1;
    for (qint32 frame = 1; frame <= frames; frame++) {
        input.fieldNumbers.append({frame * 2 - 1, frame * 2});
        input.visibleDropouts.append(0.0);
    }

    const FrameSnapshot::SearchResult result = FrameSnapshot::findBestFrame(input);
    CHECK(result.errorMessage.isEmpty());
    CHECK(result.bestFrame == 4);
    for (const FrameSnapshot::FrameScore &score : result.scores) {
        CHECK(score.inRun == (score.frame <= 6));
    }
    // The walk stops at the first frame of picture B
    CHECK(result.scores.last().frame == 7);

    // A dropout-heavy frame is passed over even when it is the quietest
    input.visibleDropouts[3] = 500.0;
    CHECK(FrameSnapshot::findBestFrame(input).bestFrame != 4);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    testActiveRectAndMargins();
    testPixelAspect();
    testOutputSize();
    testProcessWithoutUpscale();
    testFieldMetrics();
    testBestFrameSearch();

    std::cerr << "All frame snapshot tests passed\n";
    return 0;
}
