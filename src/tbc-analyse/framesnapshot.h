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
#include <functional>

#include "tbcmetadata.h"

// "Save frame as PNG" pipeline, shared by MainWindow and the headless
// --save-frame CLI: frame the picture, optionally pick the best frame of a
// held still (slideshow tapes), upscale and correct the aspect ratio. Nothing here touches TbcSource, so it is safe off the UI thread.
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

// What to do about a still held on tape (a slideshow photo shown for seconds)
enum class StillMode {
    Off,      // save the current frame
    Cleanest, // save the nearby frame closest to the still's per-pixel median
    Average,  // save the mean of the nearby frames that pass the outlier checks
};

struct Options {
    Framing framing = Framing::Active;
    // Trimmed from the framed rectangle, in frame samples (x) and frame lines (y)
    qint32 marginLeft = 0;
    qint32 marginTop = 0;
    qint32 marginRight = 0;
    // VHS head switching tears the last few lines, so they are trimmed by default
    qint32 marginBottom = 12;
    QRect customRect; // frame coordinates; used when framing == Custom

    AspectMode aspectMode = AspectMode::Exact;

    StillMode stillMode = StillMode::Off;
    qint32 searchRadius = 60; // frames either side of the current one

    qint32 upscaleFactor = 1; // 1 = off, else 2, 3 or 4
    // Resampling for the upscale and the aspect correction; see upscaleMethods()
    QString upscaleMethod = QStringLiteral("lanczos4");
};

QString framingName(Framing framing);
Framing framingFromName(const QString &name, Framing fallback);
QString aspectModeName(AspectMode mode);
QString stillModeName(StillMode mode);
StillMode stillModeFromName(const QString &name, StillMode fallback);
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

// Resampling ----------------------------------------------------------------

struct UpscaleMethod {
    QString name;  // stored in settings and taken by --upscale-method
    QString label; // shown in the save dialog
    bool learned;  // an OpenCV dnn_superres model rather than an interpolation filter
};

// Methods this build and install can run. With OpenCV: its interpolation
// filters, then each dnn_superres model whose files are installed. Without
// OpenCV: Qt's smooth (bilinear) scaling only.
QVector<UpscaleMethod> upscaleMethods();
bool isUpscaleMethodAvailable(const QString &name);

// Upscales by 2, 3 or 4. A learned method is blocking and slow; call it off
// the UI thread.
QImage upscale(const QImage &image, qint32 factor, const QString &method, QString *errorMessage);

// Crop, upscale and aspect-correct a full frame image.
QImage process(const QImage &frameImage, const Options &options,
               const TbcMetaData::VideoParameters &videoParameters, QString *errorMessage);

// Still-picture search -----------------------------------------------------
//
// Walks out from the anchor frame while the picture stays the same (a held
// slideshow photo), builds the per-pixel median of those frames' luma, and
// rejects frames with visible dropouts or far from that median (tears,
// dropouts the metadata missed). The frame closest to the median is the
// cleanest; the survivors are what "average" mixes.
//
// Each survivor's misalignment against the median is measured per field: a
// horizontal shift for every 16-line band (smoothed down the picture) and a
// vertical shift. Averaging resamples a frame only when it is off by more
// than ALIGN_THRESHOLD; a TBC'd tape is normally within a tenth of a sample,
// and resampling that little would cost more detail than it recovers.

constexpr double ALIGN_THRESHOLD = 0.25; // samples horizontally, field lines vertically

struct FrameAlignment {
    qint32 frame = 0;
    qint32 firstFieldLine = 0;       // field line (0-based) where band 0 starts
    qint32 bandHeight = 16;          // field lines per band
    QVector<float> shiftX[2];        // per field, per band: samples the frame sits right of the median
    float shiftY[2] = {0.0f, 0.0f};  // per field: field lines the frame sits below the median
    bool apply = false;              // beyond ALIGN_THRESHOLD somewhere, so averaging resamples it
};

struct FrameScore {
    qint32 frame = 0;
    double anchorDiff = 0.0; // thumbnail difference from the anchor frame, 0..1
    double distance = 0.0;   // RMS luma difference from the run's median after any realignment, 0..1
    double rawDistance = 0.0; // the same before realignment: the frame as "cleanest" saves it
    double dropouts = 0.0;   // visible dropout samples from the metadata
    bool inRun = false;      // same still as the anchor
    bool eligible = false;   // passed the dropout and distance checks
    // Eligible frames only: median and largest band shift (samples), largest
    // field shift (lines), and whether averaging resamples the frame
    double shiftX = 0.0;
    double shiftXMax = 0.0;
    double shiftYMax = 0.0;
    bool aligned = false;
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
    qint32 bestFrame = -1;           // eligible frame closest to the median
    QVector<qint32> eligibleFrames;  // what "average" mixes, in frame order
    QVector<FrameAlignment> alignments; // one per eligible frame, same order
    QVector<FrameScore> scores;      // every frame read, in frame order
    QString errorMessage;
    bool cancelled = false;
};

SearchResult findStillFrames(const SearchInput &input, std::atomic<bool> *cancel = nullptr,
                             std::atomic<qint32> *progress = nullptr);

bool writeScoreReport(const QString &filename, const SearchResult &result, QString *errorMessage);

// Resamples a full frame image (both fields, first field on even rows) so it
// lines up with the median the alignment was measured against.
QImage alignFrame(const QImage &frameImage, const FrameAlignment &alignment);

// Mean of rendered frames, per channel. render() returns a full frame image;
// frames that render null, or at another size, are skipped. A frame with an
// alignment marked apply is resampled by alignFrame() first. Blocking.
QImage averageFrames(const QVector<qint32> &frames, const std::function<QImage(qint32)> &render,
                     std::atomic<bool> *cancel = nullptr, std::atomic<qint32> *progress = nullptr,
                     const QVector<FrameAlignment> &alignments = {});

} // namespace FrameSnapshot

#endif // FRAMESNAPSHOT_H
