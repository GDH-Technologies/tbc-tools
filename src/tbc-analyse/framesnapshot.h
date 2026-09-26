/******************************************************************************
 * framesnapshot.h
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef FRAMESNAPSHOT_H
#define FRAMESNAPSHOT_H

#include <QImage>
#include <QPair>
#include <QRect>
#include <QString>
#include <QVector>
#include <atomic>

#include "tbcmetadata.h"

// "Save frame as PNG" pipeline, shared by MainWindow and the headless
// --save-frame CLI: frame the picture, optionally pick the best frame of a
// held still (slideshow tapes), upscale with Real-ESRGAN and correct the
// aspect ratio. Nothing here touches TbcSource, so it is safe off the UI thread.
namespace FrameSnapshot {

enum class Framing {
    Full,    // the whole frame, blanking and sync included
    Active,  // the metadata's active picture area
    Custom,  // a rectangle chosen in the save dialog
};

enum class AspectMode {
    Exact,  // square pixels from the decoded sample rate
    Viewer, // the stretch tbc-analyse's viewer applies in DAR mode (fixed width delta)
};

struct Options {
    Framing framing = Framing::Active;
    // Trimmed from the framed rectangle, in frame samples (x) and frame lines (y)
    qint32 marginLeft = 0;
    qint32 marginTop = 0;
    qint32 marginRight = 0;
    qint32 marginBottom = 0;
    QRect customRect; // frame coordinates; used when framing == Custom

    AspectMode aspectMode = AspectMode::Exact;

    bool bestFrameSearch = false;
    qint32 searchRadius = 60; // frames either side of the current one

    qint32 upscaleFactor = 1; // 1 = off, else 2, 3 or 4
    QString upscaleModel = QStringLiteral("realesrgan-x4plus");
};

QString framingName(Framing framing);
Framing framingFromName(const QString &name, Framing fallback);
QString aspectModeName(AspectMode mode);
AspectMode aspectModeFromName(const QString &name, AspectMode fallback);

// Active picture area in frame-image coordinates, or a null rect if the
// metadata does not define one.
QRect activeFrameRect(const TbcMetaData::VideoParameters &videoParameters, const QSize &frameSize);

// The rectangle that will be cut from the frame image: framing plus margins,
// clamped to the frame. Falls back to the full frame if the result is empty.
QRect outputRect(const Options &options, const TbcMetaData::VideoParameters &videoParameters,
                 const QSize &frameSize);

// Width delta tbc-analyse's viewer adds to a full 4fsc frame for DAR display.
qint32 viewerAspectAdjustment(const TbcMetaData::VideoParameters &videoParameters);

// Pixel aspect ratio (display width / sample width) for a mode.
double pixelAspect(AspectMode mode, const TbcMetaData::VideoParameters &videoParameters);

// Final PNG size for a framed rectangle of the given size.
QSize outputSize(const Options &options, const TbcMetaData::VideoParameters &videoParameters,
                 const QSize &framedSize);

// Real-ESRGAN ---------------------------------------------------------------

QString upscalerExecutable();
QStringList upscalerModels();

// Runs realesrgan-ncnn-vulkan on image. Blocking; call it off the UI thread.
QImage upscale(const QImage &image, qint32 factor, const QString &model, QString *errorMessage);

// Crop, upscale and aspect-correct a full frame image.
QImage process(const QImage &frameImage, const Options &options,
               const TbcMetaData::VideoParameters &videoParameters, QString *errorMessage);

// Best-frame search ---------------------------------------------------------

struct FieldMetrics {
    double sharpness = 0.0; // Tenengrad: mean squared Sobel magnitude
    double noise = 0.0;     // Immerkaer sigma estimate
};

// Per-field metrics on a luma plane (row-major, width x height).
FieldMetrics measureField(const QVector<double> &plane, qint32 width, qint32 height);

struct FrameScore {
    qint32 frame = 0;
    double sharpness = 0.0;  // mean of both fields
    double noise = 0.0;      // mean of both fields
    double combing = 0.0;    // inter-field mismatch on the woven frame
    double dropouts = 0.0;   // visible dropout samples from the metadata
    double anchorDiff = 0.0; // thumbnail difference from the anchor frame, 0..1
    double score = 0.0;
    bool inRun = false;      // same still as the anchor
    bool eligible = false;   // passed the combing/dropout filters
};

struct SearchInput {
    QString tbcFilename;
    TbcMetaData::VideoParameters videoParameters;
    qint32 anchorFrame = 1;
    qint32 radius = 60;
    QRect cropRect; // frame-image coordinates
    // Indexed by frame - firstFrame
    qint32 firstFrame = 1;
    QVector<QPair<qint32, qint32>> fieldNumbers; // first/second field per frame
    QVector<double> visibleDropouts;
};

struct SearchResult {
    qint32 bestFrame = -1;
    QVector<FrameScore> scores; // every frame read, in frame order
    QString errorMessage;
    bool cancelled = false;
};

SearchResult findBestFrame(const SearchInput &input, std::atomic<bool> *cancel = nullptr,
                           std::atomic<qint32> *progress = nullptr);

bool writeScoreReport(const QString &filename, const SearchResult &result, QString *errorMessage);

} // namespace FrameSnapshot

#endif // FRAMESNAPSHOT_H
