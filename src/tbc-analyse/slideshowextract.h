/******************************************************************************
 * slideshowextract.h
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef SLIDESHOWEXTRACT_H
#define SLIDESHOWEXTRACT_H

#include <QImage>
#include <QPair>
#include <QRect>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>

#include "framesnapshot.h"
#include "tbcmetadata.h"

// "Extract slideshow stills": find every photo held on a stretch of tape so
// each can be saved through the FrameSnapshot pipeline. Shared by MainWindow
// and the headless --extract-stills CLI; nothing here touches TbcSource.
//
// The scan reads the first field of every frame (framed area only) into a
// coarse luma thumbnail. findHolds() then splits the frames into holds with
// the still search's rule: a hold lasts while each frame stays within
// FrameSnapshot::RUN_BREAK_DIFFERENCE of the hold's first frame, or comes
// back within MAX_INTERRUPTION_FRAMES (for RESUME_FRAMES in a row). A
// dissolve therefore breaks into short holds, which the minimum hold drops,
// while a glitch burst does not end one.
namespace SlideshowExtract {

// A hold is dropped as blank (black, grey, blue screen) when its median
// thumbnail's spatial standard deviation is below this (fraction of black to
// white). Text on a plain card still has detail and stays.
constexpr double BLANK_DEVIATION = 0.02;
// A hold is a pan or zoom, not a still, when the median thumbnails of its first
// and last quarters differ by more than this.
constexpr double MOVING_DRIFT = 0.02;
// An interruption is over only once the picture is back for this many frames
// in a row. Mistracked tape tears between photos and flickers back to one for
// a frame or two at a time; real glitch bursts leave 6+ clean frames between.
constexpr qint32 RESUME_FRAMES = 4;
// A hold is unsteady, not a still, when fewer than this share of its frames
// match its first frame: the interruptions are the picture, as on a stretch
// of mistracked tape that keeps tearing between photos.
constexpr double MIN_STEADINESS = 0.6;
// Samples (and field lines) per thumbnail block. Coarser than the still
// search's 8, so a whole tape's thumbnails fit in memory.
constexpr qint32 SCAN_BLOCK = 16;

struct ScanInput {
    QString tbcFilename;
    TbcMetaData::VideoParameters videoParameters;
    QRect cropRect; // frame-image coordinates
    qint32 firstFrame = 1;
    QVector<QPair<qint32, qint32>> fieldNumbers; // first/second field of each frame, indexed by frame - firstFrame
};

struct FrameSample {
    QVector<quint8> thumbnail; // block means of the first field, 0 (black) to 255 (white)
    double difference = 1.0;   // thumbnail difference from the previous frame, 0..1 (1 for the first)
    double mean = 0.0;         // of the thumbnail, 0..1
    double deviation = 0.0;    // spatial standard deviation of the thumbnail, 0..1
};

struct ScanResult {
    QVector<FrameSample> samples; // one per frame, from input.firstFrame
    QString errorMessage;
    bool cancelled = false;
};

// Blocking; progress counts frames read.
ScanResult scan(const ScanInput &input, std::atomic<bool> *cancel = nullptr,
                std::atomic<qint32> *progress = nullptr);

// Greyscale luma of whole frames (both fields, interleaved) from the .tbc,
// scaled to width pixels across; the frame's sample aspect is kept, so a
// preview maps onto frame coordinates by its size ratio. Null for a frame
// outside input's range. For the review list: no chroma decoding, no TbcSource.
QVector<QImage> framePreviews(const ScanInput &input, const QVector<qint32> &frames, qint32 width,
                              std::atomic<bool> *cancel = nullptr);

// Mean absolute difference of two thumbnails, 0..1
double thumbnailDifference(const QVector<quint8> &a, const QVector<quint8> &b);

enum class HoldKind {
    Photo,    // a still picture
    Moving,   // drifts from start to end: a slow pan or zoom over a photo
    Unsteady, // keeps breaking up (tracking or tape damage)
};

QString holdKindName(HoldKind kind);

struct Hold {
    qint32 first = 0; // frame numbers
    qint32 last = 0;
    qint32 anchor = 0; // the frame most like the hold's median picture, nearest the middle: not in a glitch
    HoldKind kind = HoldKind::Photo;
    double drift = 0.0;     // first-quarter vs last-quarter median thumbnails, 0..1
    double deviation = 0.0; // spatial standard deviation of the median thumbnail, 0..1
    double steadiness = 1.0; // share of its frames that match its first frame
    qint32 longestSteadyRun = 0; // most frames in a row that match its first frame
    qint32 interruptions = 0;    // times the picture broke off and came back
    bool hardStart = true;  // entered by a cut (or the start of the range) rather than a gradual change
    qint32 length() const { return last - first + 1; }

};

// Holds of at least minHoldFrames that are not blank, in frame order.
// Adjacent moving holds joined by a gradual change are one pan and merge.
// Only a Photo is averaged; the other kinds are saved as their cleanest frame.
QVector<Hold> findHolds(const QVector<FrameSample> &samples, qint32 firstFrame, qint32 minHoldFrames);

bool writeScanReport(const QString &filename, const QVector<FrameSample> &samples, qint32 firstFrame,
                     const QVector<Hold> &holds, QString *errorMessage);

// Capture ---------------------------------------------------------------------

struct CaptureInput {
    ScanInput scan;                  // covering at least the hold
    QVector<double> visibleDropouts; // per frame, indexed like scan.fieldNumbers
    FrameSnapshot::Options options;  // stillMode: Average, Cleanest or Off (the anchor frame)
};

struct Capture {
    QImage image;              // framed, upscaled and aspect-corrected: ready to save
    qint32 frame = 0;          // the frame saved, or the most typical frame of an average
    qint32 framesAveraged = 1; // 1 = a single frame
    QString errorMessage;
};

// One hold's still. The still search runs inside the hold only, so it cannot
// reach the next photo; a moving hold is never averaged. render() returns a
// full, decoded frame image. Blocking.
Capture captureHold(const CaptureInput &input, const Hold &hold, const std::function<QImage(qint32)> &render,
                    std::atomic<bool> *cancel = nullptr);

// One saved still, as the manifest lists it
struct Still {
    qint32 index = 0; // 1-based, as in the file name
    QString fileName;
    Hold hold;
    qint32 captureFrame = 0;   // the frame saved, or the most typical frame of an average
    qint32 framesAveraged = 1; // 1 = a single frame
    QString startTimecode;
    double durationSeconds = 0.0;
};

// Frames per second, and a frame's position as the viewer's timecode shows it
// (HH:MM:SS:FF, frame 1 = 00:00:00:00)
double frameRate(VideoSystem system);
QString frameTimecode(qint32 frame, VideoSystem system);

// The tape's name as file names carry it: the .tbc's base name, lower case,
// runs of anything but a-z and 0-9 as "_" (as "Save frame as PNG" does)
QString fileStem(const QString &tbcFilename);

// "<stem>_still_001_f12345[_avg<N>][_up<F>x_<method>].png", the suffixes as
// "Save frame as PNG" names them
QString stillFileName(const QString &stem, qint32 index, const Capture &capture,
                      const FrameSnapshot::Options &options);

// <stem>_stills.csv: index, file, kind, first_frame, last_frame, capture_frame,
// frames_averaged, start_timecode, duration_s
bool writeManifest(const QString &filename, const QVector<Still> &stills, QString *errorMessage);

} // namespace SlideshowExtract

#endif // SLIDESHOWEXTRACT_H
