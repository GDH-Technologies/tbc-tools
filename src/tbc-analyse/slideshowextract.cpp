/******************************************************************************
 * slideshowextract.cpp
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "slideshowextract.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "framesnapshot.h"

namespace SlideshowExtract {

namespace {

// Per-block median of the thumbnails of samples[from..to]
QVector<quint8> medianThumbnail(const QVector<FrameSample> &samples, qint32 from, qint32 to)
{
    const qint32 blocks = samples[from].thumbnail.size();
    QVector<quint8> result(blocks);
    std::vector<quint8> values(to - from + 1);
    const size_t middle = values.size() / 2;
    for (qint32 b = 0; b < blocks; b++) {
        for (qint32 i = from; i <= to; i++) values[i - from] = samples[i].thumbnail.value(b);
        std::nth_element(values.begin(), values.begin() + middle, values.end());
        result[b] = values[middle];
    }
    return result;
}

double deviationOf(const QVector<quint8> &thumbnail)
{
    if (thumbnail.isEmpty()) return 0.0;
    double sum = 0.0;
    double sumSquares = 0.0;
    for (const quint8 value : thumbnail) {
        sum += value;
        sumSquares += double(value) * value;
    }
    const double mean = sum / thumbnail.size();
    return std::sqrt(std::max(0.0, sumSquares / thumbnail.size() - mean * mean)) / 255.0;
}

// The sample of from..to nearest the middle that is at least as close to the
// median thumbnail as the hold's typical frame. The still search starts
// there: in a clean hold that is at or beside the middle, so the search has
// room both ways; a glitch in the middle is passed over for the nearest clean frame.
qint32 typicalFrame(const QVector<FrameSample> &samples, qint32 from, qint32 to, const QVector<quint8> &median)
{
    std::vector<double> differences(to - from + 1);
    for (qint32 i = from; i <= to; i++) differences[i - from] = thumbnailDifference(samples[i].thumbnail, median);
    std::vector<double> sorted = differences;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double typical = sorted[sorted.size() / 2];

    const qint32 middle = from + (to - from) / 2;
    for (qint32 distance = 0; from <= middle - distance || middle + distance <= to; distance++) {
        for (const qint32 i : {middle - distance, middle + distance}) {
            if (i >= from && i <= to && differences[i - from] <= typical) return i;
        }
    }
    return middle;
}

// First quarter against last quarter, each by its median so a torn frame does
// not count as movement
double driftOf(const QVector<FrameSample> &samples, qint32 from, qint32 to)
{
    const qint32 quarter = std::max(1, (to - from + 1) / 4);
    return thumbnailDifference(medianThumbnail(samples, from, from + quarter - 1),
                               medianThumbnail(samples, to - quarter + 1, to));
}

QString csvField(const QString &text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')) && !text.contains(QLatin1Char('\n'))) {
        return text;
    }
    QString quoted = text;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

} // namespace

ScanResult scan(const ScanInput &input, std::atomic<bool> *cancel, std::atomic<qint32> *progress)
{
    ScanResult result;
    FrameSnapshot::LumaReader reader;
    if (!reader.open(input.tbcFilename, input.videoParameters, input.cropRect, &result.errorMessage)) return result;
    if (reader.width() < SCAN_BLOCK || reader.height() < SCAN_BLOCK) {
        result.errorMessage = QStringLiteral("The framed area is too small to scan.");
        return result;
    }

    std::vector<float> field(reader.fieldPixels());
    result.samples.reserve(input.fieldNumbers.size());
    for (const QPair<qint32, qint32> &fields : input.fieldNumbers) {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            return result;
        }
        reader.readField(fields.first, field.data());
        const std::vector<double> blocks = reader.thumbnail(field.data(), SCAN_BLOCK);

        FrameSample sample;
        sample.thumbnail.resize(static_cast<qint32>(blocks.size()));
        double sum = 0.0;
        double sumSquares = 0.0;
        for (size_t b = 0; b < blocks.size(); b++) {
            const double value = std::clamp(blocks[b], 0.0, 1.0);
            sample.thumbnail[static_cast<qint32>(b)] = static_cast<quint8>(std::lround(value * 255.0));
            sum += value;
            sumSquares += value * value;
        }
        sample.mean = sum / blocks.size();
        sample.deviation = std::sqrt(std::max(0.0, sumSquares / blocks.size() - sample.mean * sample.mean));
        if (!result.samples.isEmpty()) {
            sample.difference = thumbnailDifference(sample.thumbnail, result.samples.last().thumbnail);
        }
        result.samples.append(std::move(sample));
        if (progress) progress->fetch_add(1);
    }
    return result;
}

QVector<QImage> framePreviews(const ScanInput &input, const QVector<qint32> &frames, qint32 width,
                              std::atomic<bool> *cancel)
{
    QVector<QImage> previews(frames.size());
    const TbcMetaData::VideoParameters &videoParameters = input.videoParameters;
    FrameSnapshot::LumaReader reader;
    const QRect wholeFrame(0, 0, videoParameters.fieldWidth, videoParameters.fieldHeight * 2 - 1);
    if (!reader.open(input.tbcFilename, videoParameters, wholeFrame, nullptr)) return previews;

    const qint32 fieldPixels = reader.fieldPixels();
    std::vector<float> fields(2 * fieldPixels);
    for (qint32 i = 0; i < frames.size(); i++) {
        if (cancel && cancel->load()) break;
        const qint32 index = frames[i] - input.firstFrame;
        if (index < 0 || index >= input.fieldNumbers.size()) continue;
        reader.readField(input.fieldNumbers[index].first, fields.data());
        reader.readField(input.fieldNumbers[index].second, fields.data() + fieldPixels);

        // Frame row r is field r % 2, line r / 2
        QImage frame(reader.width(), reader.height() * 2, QImage::Format_Grayscale8);
        for (qint32 row = 0; row < frame.height(); row++) {
            const float *source = fields.data() + (row % 2) * fieldPixels + (row / 2) * reader.width();
            uchar *line = frame.scanLine(row);
            for (qint32 x = 0; x < frame.width(); x++) {
                line[x] = static_cast<uchar>(std::clamp(std::lround(source[x] * 255.0f), 0L, 255L));
            }
        }
        const qint32 height = std::max(1, qRound(double(frame.height()) * width / frame.width()));
        previews[i] = frame.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return previews;
}

double thumbnailDifference(const QVector<quint8> &a, const QVector<quint8> &b)
{
    if (a.isEmpty() || a.size() != b.size()) return 1.0;
    qint64 difference = 0;
    for (qint32 i = 0; i < a.size(); i++) difference += std::abs(int(a[i]) - int(b[i]));
    return double(difference) / (255.0 * a.size());
}

QString holdKindName(HoldKind kind)
{
    switch (kind) {
    case HoldKind::Moving:
        return QStringLiteral("moving");
    case HoldKind::Unsteady:
        return QStringLiteral("unsteady");
    case HoldKind::Photo:
        break;
    }
    return QStringLiteral("photo");
}

QVector<Hold> findHolds(const QVector<FrameSample> &samples, qint32 firstFrame, qint32 minHoldFrames)
{
    QVector<Hold> holds;
    qint32 start = 0;

    auto matchesStart = [&](qint32 i) {
        return thumbnailDifference(samples[i].thumbnail, samples[start].thumbnail) < FrameSnapshot::RUN_BREAK_DIFFERENCE;
    };

    // Samples start..end are one hold: keep it if long enough and not blank
    auto close = [&](qint32 end) {
        if (end - start + 1 < std::max(1, minHoldFrames)) return;
        Hold hold;
        hold.first = firstFrame + start;
        hold.last = firstFrame + end;
        hold.hardStart = start == 0 || samples[start].difference >= FrameSnapshot::RUN_BREAK_DIFFERENCE;
        const QVector<quint8> median = medianThumbnail(samples, start, end);
        hold.deviation = deviationOf(median);
        if (hold.deviation < BLANK_DEVIATION) return;
        hold.anchor = firstFrame + typicalFrame(samples, start, end, median);
        hold.drift = driftOf(samples, start, end);
        qint32 steady = 0;
        qint32 run = 0;
        for (qint32 i = start; i <= end; i++) {
            if (matchesStart(i)) {
                steady++;
                run++;
                hold.longestSteadyRun = std::max(hold.longestSteadyRun, run);
            } else {
                if (run > 0) hold.interruptions++;
                run = 0;
            }
        }
        hold.steadiness = double(steady) / (end - start + 1);
        hold.kind = (hold.steadiness < MIN_STEADINESS) ? HoldKind::Unsteady
                    : (hold.drift > MOVING_DRIFT)      ? HoldKind::Moving
                                                       : HoldKind::Photo;

        // A slow pan breaks into several holds, one per RUN_BREAK_DIFFERENCE
        // of travel, with no cut between them: list it once
        if (!holds.isEmpty()) {
            Hold &previous = holds.last();
            if (hold.kind == HoldKind::Moving && previous.kind == HoldKind::Moving && !hold.hardStart
                && previous.last + 1 == hold.first) {
                const qint32 previousStart = previous.first - firstFrame;
                previous.last = hold.last;
                previous.drift = driftOf(samples, previousStart, end);
                const QVector<quint8> merged = medianThumbnail(samples, previousStart, end);
                previous.deviation = deviationOf(merged);
                previous.anchor = firstFrame + typicalFrame(samples, previousStart, end, merged);
                return;
            }
        }
        holds.append(hold);
    };

    // The picture is back at j: RESUME_FRAMES in a row (or to the end) match
    auto resumesAt = [&](qint32 j) {
        for (qint32 k = j; k < j + RESUME_FRAMES && k < samples.size(); k++) {
            if (!matchesStart(k)) return false;
        }
        return true;
    };

    for (qint32 i = 1; i < samples.size(); i++) {
        if (matchesStart(i)) continue;
        // An interruption the picture comes back from stays inside the hold
        qint32 resume = -1;
        for (qint32 j = i + 1; j < samples.size() && j <= i + FrameSnapshot::MAX_INTERRUPTION_FRAMES; j++) {
            if (resumesAt(j)) {
                resume = j;
                break;
            }
        }
        if (resume > 0) {
            i = resume;
            continue;
        }
        close(i - 1);
        start = i;
    }
    if (!samples.isEmpty()) close(samples.size() - 1);
    return holds;
}

bool writeScanReport(const QString &filename, const QVector<FrameSample> &samples, qint32 firstFrame,
                     const QVector<Hold> &holds, QString *errorMessage)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) *errorMessage = QStringLiteral("Could not write %1").arg(filename);
        return false;
    }
    QTextStream stream(&file);
    stream << "frame,difference,mean,deviation,hold,kind,hold_drift,hold_deviation,hold_steadiness,hold_longest_run,hold_interruptions\n";
    qint32 holdIndex = 0;
    for (qint32 i = 0; i < samples.size(); i++) {
        const qint32 frame = firstFrame + i;
        while (holdIndex < holds.size() && holds[holdIndex].last < frame) holdIndex++;
        const bool inHold = holdIndex < holds.size() && holds[holdIndex].first <= frame;
        stream << frame << ',' << samples[i].difference << ',' << samples[i].mean << ',' << samples[i].deviation << ',';
        if (inHold) {
            const Hold &hold = holds[holdIndex];
            stream << holdIndex + 1 << ',' << holdKindName(hold.kind) << ',' << hold.drift << ',' << hold.deviation
                   << ',' << hold.steadiness << ',' << hold.longestSteadyRun << ',' << hold.interruptions;
        } else {
            stream << ",,,,,,";
        }
        stream << '\n';
    }
    return true;
}

double frameRate(VideoSystem system)
{
    const bool is625Line = system == PAL || system == SECAM || system == MESECAM;
    return is625Line ? 25.0 : 30000.0 / 1001.0;
}

QString frameTimecode(qint32 frame, VideoSystem system)
{
    const double rate = frameRate(system);
    const qint32 base = qRound(rate);
    const double seconds = std::max(0, frame - 1) / rate;
    const qint64 wholeSeconds = static_cast<qint64>(std::floor(seconds));
    const qint32 framePart = std::clamp(static_cast<qint32>(std::floor((seconds - wholeSeconds) * base + 1e-9)), 0, base - 1);
    return QStringLiteral("%1:%2:%3:%4")
        .arg(wholeSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((wholeSeconds % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(wholeSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(framePart, 2, 10, QLatin1Char('0'));
}

QString fileStem(const QString &tbcFilename)
{
    QString stem = QFileInfo(tbcFilename).completeBaseName().trimmed().toLower();
    stem.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    stem.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    return stem.isEmpty() ? QStringLiteral("tape") : stem;
}

Capture captureHold(const CaptureInput &input, const Hold &hold, const std::function<QImage(qint32)> &render,
                    std::atomic<bool> *cancel)
{
    Capture capture;
    const FrameSnapshot::Options &options = input.options;
    const qint32 firstIndex = hold.first - input.scan.firstFrame;
    const qint32 lastIndex = hold.last - input.scan.firstFrame;
    if (firstIndex < 0 || lastIndex >= input.scan.fieldNumbers.size() || lastIndex < firstIndex) {
        capture.errorMessage = QStringLiteral("Frames %1-%2 are outside the scanned range.").arg(hold.first).arg(hold.last);
        return capture;
    }

    capture.frame = hold.anchor;
    QImage frameImage;
    if (options.stillMode != FrameSnapshot::StillMode::Off) {
        FrameSnapshot::SearchInput search;
        search.tbcFilename = input.scan.tbcFilename;
        search.videoParameters = input.scan.videoParameters;
        search.anchorFrame = hold.anchor;
        search.radius = options.searchRadius;
        search.cropRect = input.scan.cropRect;
        search.firstFrame = hold.first;
        search.fieldNumbers = input.scan.fieldNumbers.mid(firstIndex, lastIndex - firstIndex + 1);
        for (qint32 index = firstIndex; index <= lastIndex; index++) {
            search.visibleDropouts.append(input.visibleDropouts.value(index, 0.0));
        }
        const FrameSnapshot::SearchResult result = FrameSnapshot::findStillFrames(search, cancel);
        if (result.cancelled) return capture;
        if (!result.errorMessage.isEmpty()) {
            capture.errorMessage = result.errorMessage;
            return capture;
        }
        capture.frame = result.bestFrame;
        if (options.stillMode == FrameSnapshot::StillMode::Average && hold.kind == HoldKind::Photo
            && result.eligibleFrames.size() > 1) {
            frameImage = FrameSnapshot::averageFrames(result.eligibleFrames, render, cancel, nullptr, result.alignments);
            capture.framesAveraged = result.eligibleFrames.size();
        }
    }
    if (cancel && cancel->load()) return capture;
    if (frameImage.isNull()) {
        frameImage = render(capture.frame);
        capture.framesAveraged = 1;
    }
    if (frameImage.isNull()) {
        capture.errorMessage = QStringLiteral("Frame %1 did not render.").arg(capture.frame);
        return capture;
    }
    capture.image = FrameSnapshot::process(frameImage, options, input.scan.videoParameters, &capture.errorMessage);
    return capture;
}

QString stillFileName(const QString &stem, qint32 index, const Capture &capture, const FrameSnapshot::Options &options)
{
    QString name = QStringLiteral("%1_still_%2_f%3").arg(stem).arg(index, 3, 10, QLatin1Char('0')).arg(capture.frame);
    if (capture.framesAveraged > 1) name += QStringLiteral("_avg%1").arg(capture.framesAveraged);
    if (options.upscaleFactor > 1) name += QStringLiteral("_up%1x_%2").arg(options.upscaleFactor).arg(options.upscaleMethod);
    return name + QStringLiteral(".png");
}

bool writeManifest(const QString &filename, const QVector<Still> &stills, QString *errorMessage)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) *errorMessage = QStringLiteral("Could not write %1").arg(filename);
        return false;
    }
    QTextStream stream(&file);
    stream << "index,file,kind,first_frame,last_frame,capture_frame,frames_averaged,start_timecode,duration_s\n";
    for (const Still &still : stills) {
        stream << still.index << ',' << csvField(still.fileName) << ',' << holdKindName(still.hold.kind) << ','
               << still.hold.first << ',' << still.hold.last << ',' << still.captureFrame << ','
               << still.framesAveraged << ',' << csvField(still.startTimecode) << ','
               << QString::number(still.durationSeconds, 'f', 2) << '\n';
    }
    return true;
}

} // namespace SlideshowExtract
