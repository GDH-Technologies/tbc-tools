/************************************************************************

    segments.cpp

    tbc-segments - Recording-boundary analysis of TBC metadata and fields
    Copyright (C) 2026 GDH-Technologies LLC

    This file is part of tbc-tools.

    tbc-segments is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "segments.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QPair>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const qint64 kRollover = 4294967296LL; // 2^32: legacy JSON fileLoc wraps a signed 32-bit int

double clamp01(double v)
{
    return std::max(0.0, std::min(1.0, v));
}

// Runs of consecutive true values in mask, as [start, end) pairs.
QVector<QPair<qint32, qint32>> runsOf(const QVector<bool> &mask)
{
    QVector<QPair<qint32, qint32>> runs;
    qint32 start = -1;
    for (qint32 i = 0; i < mask.size(); i++) {
        if (mask[i] && start < 0) start = i;
        if (!mask[i] && start >= 0) {
            runs.append(qMakePair(start, i));
            start = -1;
        }
    }
    if (start >= 0) runs.append(qMakePair(start, mask.size()));
    return runs;
}

double medianOf(QVector<double> values)
{
    if (values.isEmpty()) return kNaN;
    std::sort(values.begin(), values.end());
    const qint32 n = values.size();
    return (n % 2 == 1) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

double finiteMedian(const QVector<double> &values)
{
    QVector<double> finite;
    finite.reserve(values.size());
    for (double v : values) if (std::isfinite(v)) finite.append(v);
    return medianOf(finite);
}

// Indices at or above threshold that are the largest within ±window (greedy by value).
QVector<qint32> localMaxima(const QVector<double> &values, double threshold, qint32 window)
{
    QVector<qint32> candidates;
    for (qint32 i = 0; i < values.size(); i++) {
        if (std::isfinite(values[i]) && values[i] >= threshold) candidates.append(i);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&values](qint32 a, qint32 b) { return values[a] > values[b]; });
    QVector<qint32> taken;
    for (qint32 idx : candidates) {
        bool clear = true;
        for (qint32 t : taken) {
            if (std::abs(idx - t) <= window) { clear = false; break; }
        }
        if (clear) taken.append(idx);
    }
    std::sort(taken.begin(), taken.end());
    return taken;
}

void appendRunEvents(QVector<SegmentEvent> &events, const QVector<bool> &mask, qint32 minRun,
                     const QString &kind, const std::function<double(qint32, qint32)> &severityOf,
                     const std::function<QJsonObject(qint32, qint32)> &detailOf)
{
    for (const auto &run : runsOf(mask)) {
        if (run.second - run.first < minRun) continue;
        SegmentEvent ev;
        ev.kind = kind;
        ev.startField = run.first;
        ev.endFieldExclusive = run.second;
        ev.severity = clamp01(severityOf(run.first, run.second));
        ev.detail = detailOf(run.first, run.second);
        events.append(ev);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void FieldMetrics::resize(qint32 numberOfFields)
{
    const qint32 n = std::max(0, numberOfFields);
    for (QVector<double> *v : {&lumaMeanIre, &fieldDiffIre, &blankingDevIre, &syncTipDevIre, &noiseIre, &burstAmpIre}) {
        v->clear();
        v->fill(kNaN, n);
    }
}

double fieldRateForSystem(VideoSystem system)
{
    return (system == PAL || system == SECAM || system == MESECAM) ? 50.0 : 60000.0 / 1001.0;
}

double syncTipIreForSystem(VideoSystem system)
{
    return (system == PAL || system == SECAM || system == MESECAM) ? -300.0 / 7.0 : -40.0;
}

bool resolveFieldRange(const TbcMetaData &metaData, qint32 startFrameOneBased, qint32 lengthFrames, FieldRange *range)
{
    if (!range) return false;
    const qint32 totalFrames = metaData.getNumberOfFrames();
    const qint32 totalFields = metaData.getNumberOfFields();
    if (totalFrames < 1 || totalFields < 1) return false;

    const qint32 resolvedStartFrame = startFrameOneBased > 0 ? startFrameOneBased : 1;
    if (resolvedStartFrame > totalFrames) return false;
    const qint32 maxLength = totalFrames - resolvedStartFrame + 1;
    const qint32 resolvedLength = lengthFrames > 0 ? qMin(lengthFrames, maxLength) : maxLength;
    if (resolvedLength < 1) return false;

    const qint32 resolvedEndFrame = resolvedStartFrame + resolvedLength - 1;
    const qint32 startFieldOneBased = metaData.getFirstFieldNumber(resolvedStartFrame);
    const qint32 endFieldOneBased = metaData.getSecondFieldNumber(resolvedEndFrame);
    if (startFieldOneBased < 1 || endFieldOneBased < 1) return false;

    range->startField = qMax<qint32>(0, startFieldOneBased - 1);
    range->endFieldExclusive = qMin<qint32>(totalFields, endFieldOneBased);
    return range->endFieldExclusive > range->startField;
}

SegmentsAnalysis analyseSegments(const TbcMetaData &metaData,
                                 const FieldRange &range,
                                 const SegmentsThresholds &t,
                                 double rfSampleRateHz,
                                 const FieldMetrics *fieldMetrics)
{
    SegmentsAnalysis a;
    const TbcMetaData::VideoParameters &vp = metaData.getVideoParameters();
    const qint32 n = metaData.getNumberOfFields();
    a.numberOfFields = n;
    a.numberOfFrames = metaData.getNumberOfFrames();
    a.frameOffset = (n / 2) - a.numberOfFrames;
    a.isFirstFieldFirst = metaData.getIsFirstFieldFirst();
    a.fieldRate = fieldRateForSystem(vp.system);
    a.secondsPerField = 1.0 / a.fieldRate;
    if (n < 1) {
        a.gapDetection = QStringLiteral("unavailable");
        a.nominalSource = QStringLiteral("none");
        return a;
    }

    // --- gather per-field values ------------------------------------------------
    QVector<qint64> fileLoc(n, -1);
    QVector<double> diskLoc(n, -1.0);
    QVector<bool> isFirst(n, false);
    a.syncConf.fill(0, n);
    a.decodeFaults.fill(0, n);
    a.dropoutCoverage.fill(0.0, n);

    const qint32 activeStart = std::max(0, vp.activeVideoStart);
    const qint32 activeEnd = std::max(activeStart, vp.activeVideoEnd);
    const qint32 firstLine = std::max(1, vp.firstActiveFieldLine);
    const qint32 lastLine = std::max(firstLine, vp.lastActiveFieldLine);
    a.activeWidth = activeEnd - activeStart;
    a.activeLines = lastLine - firstLine;
    const double activeArea = static_cast<double>(std::max(1, a.activeWidth)) * std::max(1, a.activeLines);

    qint64 rollover = 0;
    qint64 previousRaw = 0;
    bool havePrevious = false;
    for (qint32 i = 0; i < n; i++) {
        const TbcMetaData::Field &f = metaData.getField(i + 1);
        isFirst[i] = f.isFirstField;
        a.syncConf[i] = f.syncConf;
        a.decodeFaults[i] = f.decodeFaults < 0 ? 0 : f.decodeFaults;
        diskLoc[i] = f.diskLoc;

        const qint64 raw = f.fileLoc;
        if (raw >= 0 || havePrevious) {
            if (havePrevious && raw < 0 && previousRaw >= 0) {
                rollover += kRollover;
                a.fileLocRolloverFixups++;
            }
            fileLoc[i] = raw + rollover;
            previousRaw = raw;
            havePrevious = true;
        }

        const DropOuts &dropouts = metaData.getFieldDropOuts(i + 1);
        double covered = 0.0;
        for (qint32 d = 0; d < dropouts.size(); d++) {
            const qint32 line = dropouts.fieldLine(d);
            if (line < firstLine || line >= lastLine) continue;
            const qint32 sx = std::max(activeStart, dropouts.startx(d));
            const qint32 ex = std::min(activeEnd, dropouts.endx(d));
            if (ex > sx) covered += ex - sx;
        }
        a.dropoutCoverage[i] = covered / activeArea;
    }

    // --- timing: nominal samples per field -----------------------------------------
    bool haveFileLoc = false;
    for (qint32 i = 1; i < n; i++) {
        if (fileLoc[i] >= 0 && fileLoc[i - 1] >= 0) { haveFileLoc = true; break; }
    }
    QVector<double> position(n, kNaN); // in whichever unit gap detection uses
    if (haveFileLoc) {
        a.gapDetection = QStringLiteral("fileLoc");
        for (qint32 i = 0; i < n; i++) if (fileLoc[i] >= 0) position[i] = static_cast<double>(fileLoc[i]);
        if (rfSampleRateHz > 0) {
            a.nominalSamplesPerField = rfSampleRateHz / a.fieldRate;
            a.nominalSource = QStringLiteral("rf-sample-rate-hz");
        } else {
            QVector<double> deltas;
            for (qint32 i = 1; i < n; i++) {
                if (std::isfinite(position[i]) && std::isfinite(position[i - 1]) && position[i] > position[i - 1])
                    deltas.append(position[i] - position[i - 1]);
            }
            a.nominalSamplesPerField = medianOf(deltas);
            a.nominalSource = QStringLiteral("median-delta");
        }
        a.impliedRfSampleRateHz = a.nominalSamplesPerField * a.fieldRate;
    } else {
        bool haveDiskLoc = false;
        for (qint32 i = 1; i < n; i++) if (diskLoc[i] >= 0 && diskLoc[i - 1] >= 0) { haveDiskLoc = true; break; }
        if (haveDiskLoc) {
            a.gapDetection = QStringLiteral("diskLoc");
            for (qint32 i = 0; i < n; i++) if (diskLoc[i] >= 0) position[i] = diskLoc[i];
            a.nominalSamplesPerField = 1.0;
            a.nominalSource = QStringLiteral("diskLoc");
        } else {
            a.gapDetection = QStringLiteral("unavailable");
            a.nominalSource = QStringLiteral("none");
        }
    }
    if (!(a.nominalSamplesPerField > 0)) {
        a.gapDetection = QStringLiteral("unavailable");
    }

    // --- events over the whole file ------------------------------------------------
    QVector<SegmentEvent> events;

    if (a.gapDetection != QLatin1String("unavailable")) {
        const double nominal = a.nominalSamplesPerField;
        const double lo = nominal * (1.0 - t.gapTolerance);
        const double hi = nominal * (1.0 + t.gapTolerance);
        for (qint32 i = 1; i < n; i++) {
            if (!std::isfinite(position[i]) || !std::isfinite(position[i - 1])) continue;
            const double delta = position[i] - position[i - 1];
            if (delta > lo && delta < hi) continue;
            const double deltaFields = delta / nominal;
            SegmentEvent ev;
            ev.kind = QStringLiteral("gap");
            ev.startField = i;
            ev.endFieldExclusive = i + 1;
            ev.severity = clamp01(std::abs(deltaFields - 1.0) / 2.0);
            ev.detail.insert("seamAfterField", i - 1);
            ev.detail.insert("deltaSamples", static_cast<double>(delta));
            ev.detail.insert("deltaFields", deltaFields);
            ev.detail.insert("missingFields", std::max(0, static_cast<qint32>(std::lround(deltaFields)) - 1));
            if (a.gapDetection == QLatin1String("fileLoc") && i < diskLoc.size() && diskLoc[i] >= 0 && diskLoc[i - 1] >= 0)
                ev.detail.insert("deltaDiskLoc", diskLoc[i] - diskLoc[i - 1]);
            if (delta < 0) ev.detail.insert("direction", QStringLiteral("backward"));
            events.append(ev);
        }
    }

    {
        QVector<bool> parity(n, false), skipped(n, false);
        for (qint32 i = 0; i < n; i++) {
            if (a.decodeFaults[i] & 4) skipped[i] = true;
            if (a.decodeFaults[i] & 1) parity[i] = true;
            else if (i > 0 && isFirst[i] == isFirst[i - 1] && !skipped[i]) parity[i] = true;
        }
        appendRunEvents(events, parity, 1, QStringLiteral("parity_break"),
                        [](qint32, qint32) { return 0.6; },
                        [&](qint32 s, qint32 e) {
                            QJsonObject d;
                            bool flagged = false;
                            for (qint32 i = s; i < e; i++) if (a.decodeFaults[i] & 1) flagged = true;
                            d.insert("reason", flagged ? QStringLiteral("decodeFaults") : QStringLiteral("same-parity-repeat"));
                            d.insert("fields", e - s);
                            return d;
                        });
        appendRunEvents(events, skipped, 1, QStringLiteral("skipped_field"),
                        [](qint32, qint32) { return 0.7; },
                        [](qint32 s, qint32 e) { QJsonObject d; d.insert("fields", e - s); return d; });
    }

    {
        // The option is a percentage of the file's own median syncConf: vhs-decode
        // writes 45 for a healthy VHS field where ld-decode writes 100, and its
        // forced values on faults (10, 0) sit well below either. A fixed 50 would
        // flag an entire healthy VHS tape as one sync loss.
        QVector<double> confs;
        confs.reserve(n);
        for (qint32 i = 0; i < n; i++) confs.append(a.syncConf[i]);
        a.medianSyncConf = medianOf(confs);
        a.effectiveSyncConfThreshold = (std::isfinite(a.medianSyncConf) && a.medianSyncConf > 0)
                                           ? t.syncConfThreshold * a.medianSyncConf / 100.0
                                           : static_cast<double>(t.syncConfThreshold);
        QVector<bool> low(n, false);
        for (qint32 i = 0; i < n; i++) low[i] = a.syncConf[i] < a.effectiveSyncConfThreshold;
        appendRunEvents(events, low, std::max(1, t.minRunFields), QStringLiteral("sync_loss"),
                        [&](qint32 s, qint32 e) {
                            double sum = 0; for (qint32 i = s; i < e; i++) sum += a.syncConf[i];
                            const double mean = sum / (e - s);
                            const double depth = a.effectiveSyncConfThreshold > 0 ? clamp01(1.0 - mean / a.effectiveSyncConfThreshold) : 1.0;
                            return depth * std::min(1.0, (e - s) / 10.0);
                        },
                        [&](qint32 s, qint32 e) {
                            QJsonObject d;
                            qint32 minimum = 100; double sum = 0;
                            for (qint32 i = s; i < e; i++) { minimum = std::min(minimum, a.syncConf[i]); sum += a.syncConf[i]; }
                            d.insert("minSyncConf", minimum);
                            d.insert("meanSyncConf", sum / (e - s));
                            d.insert("fields", e - s);
                            return d;
                        });
    }

    {
        QVector<bool> storm(n, false);
        for (qint32 i = 0; i < n; i++) storm[i] = a.dropoutCoverage[i] >= t.dropoutStormThreshold;
        appendRunEvents(events, storm, std::max(1, t.minRunFields), QStringLiteral("dropout_storm"),
                        [&](qint32 s, qint32 e) {
                            double sum = 0; for (qint32 i = s; i < e; i++) sum += a.dropoutCoverage[i];
                            return sum / (e - s);
                        },
                        [&](qint32 s, qint32 e) {
                            QJsonObject d; double sum = 0;
                            for (qint32 i = s; i < e; i++) sum += a.dropoutCoverage[i];
                            d.insert("meanCoverage", sum / (e - s));
                            d.insert("fields", e - s);
                            return d;
                        });
    }

    // --- field-data events ------------------------------------------------------------
    QVector<bool> noiseMask, burstMask;
    if (fieldMetrics && fieldMetrics->enabled && fieldMetrics->noiseIre.size() == n) {
        const FieldMetrics &m = *fieldMetrics;
        const qint32 minRun = std::max(1, t.minRunFields);

        // Real tape raises both baselines (an EP VHS decode carries several IRE of
        // back-porch noise, and that noise feeds straight into the field
        // difference), so the configured thresholds act as floors under a
        // multiple of the file's own median.
        a.medianNoiseIre = finiteMedian(m.noiseIre);
        a.medianFieldDiffIre = finiteMedian(m.fieldDiffIre);
        a.effectiveNoiseThresholdIre = std::max(t.noiseThresholdIre, std::isfinite(a.medianNoiseIre) ? 2.5 * a.medianNoiseIre : 0.0);
        a.effectiveSceneThresholdIre = std::max(t.sceneThresholdIre, std::isfinite(a.medianFieldDiffIre) ? 2.5 * a.medianFieldDiffIre : 0.0);

        noiseMask.fill(false, n);
        for (qint32 i = 0; i < n; i++) noiseMask[i] = std::isfinite(m.noiseIre[i]) && m.noiseIre[i] >= a.effectiveNoiseThresholdIre;

        // A scene change needs a stable picture on both sides: snow makes every
        // same-parity difference enormous, so a candidate whose own field or whose
        // n-2 partner is noisy is the noise run's business, not a cut.
        for (qint32 i : localMaxima(m.fieldDiffIre, a.effectiveSceneThresholdIre, std::max(2, minRun))) {
            if (noiseMask[i] || (i >= 2 && noiseMask[i - 2])) continue;
            SegmentEvent ev;
            ev.kind = QStringLiteral("scene_change");
            ev.startField = i;
            ev.endFieldExclusive = i + 1;
            ev.severity = clamp01(m.fieldDiffIre[i] / 40.0);
            ev.detail.insert("fieldDiffIre", m.fieldDiffIre[i]);
            events.append(ev);
        }
        appendRunEvents(events, noiseMask, minRun, QStringLiteral("noise"),
                        [&](qint32 s, qint32 e) { double sum = 0; for (qint32 i = s; i < e; i++) sum += m.noiseIre[i]; return (sum / (e - s)) / (4.0 * t.noiseThresholdIre); },
                        [&](qint32 s, qint32 e) { QJsonObject d; double sum = 0; for (qint32 i = s; i < e; i++) sum += m.noiseIre[i]; d.insert("meanNoiseIre", sum / (e - s)); d.insert("fields", e - s); return d; });

        QVector<bool> blank(n, false);
        for (qint32 i = 0; i < n; i++) {
            blank[i] = std::isfinite(m.lumaMeanIre[i]) && m.lumaMeanIre[i] <= t.blankLumaIre
                       && std::isfinite(m.noiseIre[i]) && m.noiseIre[i] < a.effectiveNoiseThresholdIre;
        }
        appendRunEvents(events, blank, minRun, QStringLiteral("blank_video"),
                        [](qint32, qint32) { return 0.5; },
                        [&](qint32 s, qint32 e) { QJsonObject d; double sum = 0; for (qint32 i = s; i < e; i++) sum += m.lumaMeanIre[i]; d.insert("meanLumaIre", sum / (e - s)); d.insert("fields", e - s); return d; });

        if (m.hasBurst) {
            const double median = finiteMedian(m.burstAmpIre);
            if (std::isfinite(median) && median > 0) {
                burstMask.fill(false, n);
                for (qint32 i = 0; i < n; i++) burstMask[i] = std::isfinite(m.burstAmpIre[i]) && m.burstAmpIre[i] < 0.25 * median;
                appendRunEvents(events, burstMask, minRun, QStringLiteral("no_burst"),
                                [](qint32, qint32) { return 0.6; },
                                [&](qint32 s, qint32 e) { QJsonObject d; d.insert("medianBurstIre", median); d.insert("fields", e - s); return d; });
            }
        }
    }

    // A noise / no-burst run coincident with a metadata gap or sync loss raises that event.
    if (!noiseMask.isEmpty() || !burstMask.isEmpty()) {
        for (SegmentEvent &ev : events) {
            if (ev.kind != QLatin1String("gap") && ev.kind != QLatin1String("sync_loss")) continue;
            bool boosted = false;
            for (qint32 i = std::max(0, ev.startField - 1); i < std::min(n, ev.endFieldExclusive + 1) && !boosted; i++) {
                if ((!noiseMask.isEmpty() && noiseMask[i]) || (!burstMask.isEmpty() && burstMask[i])) boosted = true;
            }
            if (boosted) {
                ev.severity = clamp01(ev.severity + 0.2);
                ev.detail.insert("fieldDataCoincident", true);
            }
        }
    }

    // --- clip to the requested range and order ---------------------------------------
    QVector<SegmentEvent> clipped;
    for (SegmentEvent ev : events) {
        if (ev.endFieldExclusive <= range.startField || ev.startField >= range.endFieldExclusive) continue;
        ev.startField = std::max(ev.startField, range.startField);
        ev.endFieldExclusive = std::min(ev.endFieldExclusive, range.endFieldExclusive);
        clipped.append(ev);
    }
    std::stable_sort(clipped.begin(), clipped.end(), [](const SegmentEvent &x, const SegmentEvent &y) {
        if (x.startField != y.startField) return x.startField < y.startField;
        return x.kind < y.kind;
    });
    a.events = clipped;
    for (const SegmentEvent &ev : clipped) a.counts[ev.kind] += 1;

    // --- gapless sections ----------------------------------------------------------------
    qint32 sectionStart = range.startField;
    for (const SegmentEvent &ev : clipped) {
        if (ev.kind != QLatin1String("gap")) continue;
        if (ev.startField > sectionStart) a.sections.append({sectionStart, ev.startField});
        sectionStart = ev.startField;
    }
    if (range.endFieldExclusive > sectionStart) a.sections.append({sectionStart, range.endFieldExclusive});

    return a;
}

namespace {

QJsonArray sliceDoubles(const QVector<double> &values, const FieldRange &range, int decimals)
{
    QJsonArray out;
    const double scale = std::pow(10.0, decimals);
    for (qint32 i = range.startField; i < range.endFieldExclusive && i < values.size(); i++) {
        const double v = values[i];
        if (std::isfinite(v)) out.append(std::round(v * scale) / scale);
        else out.append(QJsonValue::Null);
    }
    return out;
}

QJsonArray sliceInts(const QVector<qint32> &values, const FieldRange &range)
{
    QJsonArray out;
    for (qint32 i = range.startField; i < range.endFieldExclusive && i < values.size(); i++) out.append(values[i]);
    return out;
}

} // namespace

QJsonObject buildReport(const TbcMetaData &metaData,
                        const QString &inputPath,
                        const QString &inputKind,
                        qint32 startFrameOneBased,
                        qint32 lengthFrames,
                        const FieldRange &range,
                        const SegmentsThresholds &t,
                        const SegmentsAnalysis &a,
                        const FieldMetrics *fieldMetrics,
                        const QJsonObject &fieldDataInfo,
                        bool perField)
{
    const TbcMetaData::VideoParameters &vp = metaData.getVideoParameters();
    const double spf = a.secondsPerField;

    QJsonObject report;
    report.insert("schemaVersion", 1);
    QJsonObject tool;
    tool.insert("name", QStringLiteral("tbc-segments"));
    tool.insert("branch", QStringLiteral(APP_BRANCH));
    tool.insert("commit", QStringLiteral(APP_COMMIT));
    report.insert("tool", tool);

    QJsonObject input;
    input.insert("path", inputPath);
    input.insert("kind", inputKind);
    report.insert("input", input);

    QJsonObject video;
    video.insert("system", metaData.getVideoSystemDescription());
    video.insert("tapeFormat", vp.tapeFormat);
    video.insert("fieldRate", a.fieldRate);
    video.insert("secondsPerField", spf);
    video.insert("fieldWidth", vp.fieldWidth);
    video.insert("fieldHeight", vp.fieldHeight);
    video.insert("numberOfFields", a.numberOfFields);
    video.insert("numberOfFrames", a.numberOfFrames);
    video.insert("frameOffset", a.frameOffset);
    video.insert("isFirstFieldFirst", a.isFirstFieldFirst);
    report.insert("video", video);

    QJsonObject timing;
    timing.insert("nominalSamplesPerField", a.nominalSamplesPerField);
    timing.insert("nominalSource", a.nominalSource);
    timing.insert("impliedRfSampleRateHz", a.impliedRfSampleRateHz);
    timing.insert("gapDetection", a.gapDetection);
    timing.insert("fileLocRolloverFixups", static_cast<double>(a.fileLocRolloverFixups));
    report.insert("timing", timing);

    QJsonObject rangeObj;
    rangeObj.insert("startFrame", startFrameOneBased > 0 ? startFrameOneBased : 1);
    rangeObj.insert("lengthFrames", lengthFrames > 0 ? lengthFrames : a.numberOfFrames);
    rangeObj.insert("startField", range.startField);
    rangeObj.insert("endFieldExclusive", range.endFieldExclusive);
    rangeObj.insert("secondsOrigin", QStringLiteral("field0"));
    report.insert("range", rangeObj);

    QJsonObject thresholds;
    thresholds.insert("gapTolerance", t.gapTolerance);
    thresholds.insert("syncConfThreshold", t.syncConfThreshold);
    thresholds.insert("syncConfThresholdEffective", a.effectiveSyncConfThreshold);
    thresholds.insert("medianSyncConf", std::isfinite(a.medianSyncConf) ? QJsonValue(a.medianSyncConf) : QJsonValue(QJsonValue::Null));
    thresholds.insert("minRunFields", t.minRunFields);
    thresholds.insert("dropoutStormThreshold", t.dropoutStormThreshold);
    thresholds.insert("activeWidth", a.activeWidth);
    thresholds.insert("activeLines", a.activeLines);
    report.insert("thresholds", thresholds);

    QJsonObject fieldData = fieldDataInfo;
    fieldData.insert("enabled", fieldMetrics != nullptr && fieldMetrics->enabled);
    if (fieldMetrics && fieldMetrics->enabled) {
        QJsonObject ft;
        ft.insert("sceneThresholdIre", t.sceneThresholdIre);
        ft.insert("noiseThresholdIre", t.noiseThresholdIre);
        ft.insert("blankLumaIre", t.blankLumaIre);
        ft.insert("sceneThresholdIreEffective", a.effectiveSceneThresholdIre);
        ft.insert("noiseThresholdIreEffective", a.effectiveNoiseThresholdIre);
        ft.insert("medianFieldDiffIre", std::isfinite(a.medianFieldDiffIre) ? QJsonValue(a.medianFieldDiffIre) : QJsonValue(QJsonValue::Null));
        ft.insert("medianNoiseIre", std::isfinite(a.medianNoiseIre) ? QJsonValue(a.medianNoiseIre) : QJsonValue(QJsonValue::Null));
        fieldData.insert("thresholds", ft);
        fieldData.insert("hasBurst", fieldMetrics->hasBurst);
    }
    report.insert("fieldData", fieldData);

    QJsonArray sections;
    for (const SegmentSection &s : a.sections) {
        QJsonObject o;
        o.insert("startField", s.startField);
        o.insert("endFieldExclusive", s.endFieldExclusive);
        o.insert("startSeconds", s.startField * spf);
        o.insert("endSeconds", s.endFieldExclusive * spf);
        sections.append(o);
    }
    report.insert("sections", sections);

    QJsonArray events;
    for (const SegmentEvent &ev : a.events) {
        QJsonObject o;
        o.insert("kind", ev.kind);
        o.insert("startField", ev.startField);
        o.insert("endFieldExclusive", ev.endFieldExclusive);
        o.insert("startSeconds", ev.startField * spf);
        o.insert("endSeconds", ev.endFieldExclusive * spf);
        o.insert("severity", std::round(ev.severity * 1000.0) / 1000.0);
        o.insert("detail", ev.detail);
        events.append(o);
    }
    report.insert("events", events);

    QJsonObject counts;
    for (const char *kind : {"gap", "sync_loss", "parity_break", "skipped_field", "dropout_storm",
                             "scene_change", "noise", "blank_video", "no_burst"}) {
        counts.insert(kind, a.counts.value(QString::fromLatin1(kind), 0));
    }
    report.insert("counts", counts);

    if (perField) {
        QJsonObject pf;
        pf.insert("syncConf", sliceInts(a.syncConf, range));
        pf.insert("decodeFaults", sliceInts(a.decodeFaults, range));
        pf.insert("dropoutCoverage", sliceDoubles(a.dropoutCoverage, range, 4));
        report.insert("perField", pf);
        if (fieldMetrics && fieldMetrics->enabled) {
            QJsonObject fm;
            fm.insert("lumaMeanIre", sliceDoubles(fieldMetrics->lumaMeanIre, range, 2));
            fm.insert("fieldDiffIre", sliceDoubles(fieldMetrics->fieldDiffIre, range, 2));
            fm.insert("blankingDevIre", sliceDoubles(fieldMetrics->blankingDevIre, range, 2));
            fm.insert("syncTipDevIre", sliceDoubles(fieldMetrics->syncTipDevIre, range, 2));
            fm.insert("noiseIre", sliceDoubles(fieldMetrics->noiseIre, range, 2));
            if (fieldMetrics->hasBurst) fm.insert("burstAmpIre", sliceDoubles(fieldMetrics->burstAmpIre, range, 2));
            report.insert("fieldMetrics", fm);
        }
    }
    return report;
}

QString summariseAnalysis(const SegmentsAnalysis &a, const FieldRange &range)
{
    QString out;
    out += QStringLiteral("Fields %1..%2 of %3 (%4 frames, field rate %5, %6 s)\n")
               .arg(range.startField).arg(range.endFieldExclusive).arg(a.numberOfFields).arg(a.numberOfFrames)
               .arg(a.fieldRate, 0, 'f', 3).arg((range.endFieldExclusive - range.startField) * a.secondsPerField, 0, 'f', 1);
    out += QStringLiteral("Gap detection: %1 (nominal %2 samples/field from %3, implied RF %4 Hz, rollover fixups %5)\n")
               .arg(a.gapDetection).arg(a.nominalSamplesPerField, 0, 'f', 1).arg(a.nominalSource)
               .arg(a.impliedRfSampleRateHz, 0, 'f', 0).arg(a.fileLocRolloverFixups);
    out += QStringLiteral("Sync loss below syncConf %1 (median %2)\n")
               .arg(a.effectiveSyncConfThreshold, 0, 'f', 1).arg(a.medianSyncConf, 0, 'f', 1);
    out += QStringLiteral("Sections: %1\n").arg(a.sections.size());
    for (auto it = a.counts.constBegin(); it != a.counts.constEnd(); ++it) {
        out += QStringLiteral("  %1: %2\n").arg(it.key()).arg(it.value());
    }
    return out;
}
