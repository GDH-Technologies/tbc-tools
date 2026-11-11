/************************************************************************

    restartmarks.cpp

    tbc-segments - Camcorder restart marks
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

#include "restartmarks.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double medianOfRange(const QVector<double> &values, qint32 first, qint32 lastInclusive)
{
    QVector<double> window;
    for (qint32 i = std::max(0, first); i <= std::min<qint32>(values.size() - 1, lastInclusive); i++) {
        if (std::isfinite(values[i])) window.append(values[i]);
    }
    if (window.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(window.begin(), window.end());
    const qint32 n = window.size();
    return (n % 2 == 1) ? window[n / 2] : 0.5 * (window[n / 2 - 1] + window[n / 2]);
}

double clamp01(double v)
{
    return std::max(0.0, std::min(1.0, v));
}

// A single field lit by a flash: brighter than both neighbours and than the
// picture around it by at least flashIre
bool isFlashField(const QVector<double> &luma, qint32 j, double flashIre)
{
    if (j < 1 || j + 1 >= luma.size()) return false;
    const double l = luma[j];
    if (!std::isfinite(l) || !std::isfinite(luma[j - 1]) || !std::isfinite(luma[j + 1])) return false;
    if (l - std::max(luma[j - 1], luma[j + 1]) <= flashIre) return false;
    const double before = medianOfRange(luma, j - 6, j - 2);
    const double after = medianOfRange(luma, j + 2, j + 6);
    const double around = std::max(std::isfinite(before) ? before : -1e9, std::isfinite(after) ? after : -1e9);
    return l - around > flashIre;
}

QString number(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

} // namespace

bool SegmentMark::operator==(const SegmentMark &o) const
{
    return id == o.id && field == o.field && kind == o.kind && source == o.source && enabled == o.enabled
           && split == o.split && std::abs(confidence - o.confidence) < 1e-9 && reason == o.reason
           && detailJson == o.detailJson && comment == o.comment && createdBy == o.createdBy
           && updatedAt == o.updatedAt && derivedFrom == o.derivedFrom;
}

QVector<SegmentMark> detectRestartMarks(const SegmentsAnalysis &analysis,
                                        const FieldMetrics &metrics,
                                        const SegmentsThresholds &t,
                                        const QVector<TbcMetaData::Segment> &baseSegments,
                                        const QString &createdBy)
{
    QVector<SegmentMark> marks;
    const qint32 n = analysis.numberOfFields;
    if (!t.restartMarks || !metrics.enabled || n < 1 || metrics.fieldDiffIre.size() != n) return marks;
    const QVector<double> &D = metrics.fieldDiffIre;
    const QVector<double> &L = metrics.lumaMeanIre;
    const QVector<double> &N = metrics.noiseIre;
    const double noiseLimit = analysis.effectiveNoiseThresholdIre > 0 ? analysis.effectiveNoiseThresholdIre : t.noiseThresholdIre;
    const qint32 edge = std::max(2, t.restartFlashWindowFields);

    QJsonObject provenance;
    provenance.insert("tool", createdBy);
    QJsonObject thresholds;
    thresholds.insert("restartPairMatch", t.restartPairMatch);
    thresholds.insert("restartMinPeakIre", t.restartMinPeakIre);
    thresholds.insert("restartMinContrast", t.restartMinContrast);
    thresholds.insert("restartMaxNeighbourRatio", t.restartMaxNeighbourRatio);
    thresholds.insert("restartFlashWindowFields", t.restartFlashWindowFields);
    thresholds.insert("restartFlashIre", t.restartFlashIre);
    provenance.insert("thresholds", thresholds);
    const QString derivedFrom = QString::fromUtf8(QJsonDocument(provenance).toJson(QJsonDocument::Compact));
    const QString stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    struct Candidate { qint32 field; double contrast; SegmentMark mark; };
    QVector<Candidate> candidates;

    for (const TbcMetaData::Segment &segment : baseSegments) {
        if (segment.kind != QLatin1String("clip")) continue;
        const qint32 first = std::max(segment.startField + edge, 2);
        const qint32 last = std::min(segment.endFieldExclusive - edge, n - 15);   // exclusive
        for (qint32 i = first; i < last; i++) {
            const double a = D[i], b = D[i + 1];
            if (!std::isfinite(a) || !std::isfinite(b)) continue;
            const double peak = std::max(a, b);
            if (peak < t.restartMinPeakIre) continue;
            if (std::min(a, b) < t.restartPairMatch * peak) continue;

            const double before = medianOfRange(D, i - 12, i - 2);
            const double after = medianOfRange(D, i + 3, i + 14);
            double background = std::max(std::isfinite(before) ? before : 0.0, std::isfinite(after) ? after : 0.0);
            background = std::max(background, 0.05);
            const double contrast = peak / background;
            if (contrast < t.restartMinContrast) continue;

            // Quiet neighbours: no higher than halfway from the background to
            // the peak, and no more than restartMaxNeighbourRatio of the peak
            // (sustained motion keeps the fields around a spike high)
            const double quietLimit = std::min(background + 0.5 * (peak - background), t.restartMaxNeighbourRatio * peak);
            const double neighbourBefore = D[i - 1], neighbourAfter = D[i + 2];
            if (!(std::isfinite(neighbourBefore) && neighbourBefore <= quietLimit)) continue;
            if (!(std::isfinite(neighbourAfter) && neighbourAfter <= quietLimit)) continue;

            if ((std::isfinite(N[i]) && N[i] >= noiseLimit) || (std::isfinite(N[i + 1]) && N[i + 1] >= noiseLimit)) continue;

            bool flash = false;
            for (qint32 j = std::max(1, i - t.restartFlashWindowFields); j < i && !flash; j++) {
                flash = isFlashField(L, j, t.restartFlashIre);
            }
            if (flash) continue;

            // Confidence: 50% for passing, the rest from how cleanly it passes
            const double pairScore = clamp01((std::min(a, b) / peak - t.restartPairMatch) / std::max(1e-6, 1.0 - t.restartPairMatch));
            const double contrastScore = clamp01((contrast - t.restartMinContrast) / std::max(1e-6, 4.0 - t.restartMinContrast));
            const double loudestNeighbour = std::max(neighbourBefore, neighbourAfter);
            const double quietScore = clamp01(1.0 - (loudestNeighbour - background) / std::max(1e-6, quietLimit - background));
            const double confidence = 0.5 + 0.5 * (0.3 * pairScore + 0.4 * contrastScore + 0.3 * quietScore);

            const double lumaShift = medianOfRange(L, i + 2, i + 11) - medianOfRange(L, i - 10, i - 1);

            SegmentMark mark;
            mark.field = i;
            mark.split = t.splitAtRestarts;
            mark.confidence = std::round(confidence * 1000.0) / 1000.0;
            mark.reason = QStringLiteral("matched field pair %1/%2 IRE (%3%), %4x background, neighbours quiet "
                                         "(%5/%6 IRE), no flash in %7 fields; luma %8%9 IRE")
                              .arg(number(a, 1), number(b, 1))
                              .arg(qRound(100.0 * std::min(a, b) / peak))
                              .arg(number(contrast, 1))
                              .arg(number(neighbourBefore, 1), number(neighbourAfter, 1))
                              .arg(t.restartFlashWindowFields)
                              .arg(lumaShift >= 0 ? QStringLiteral("+") : QString())
                              .arg(number(std::isfinite(lumaShift) ? lumaShift : 0.0, 1));
            QJsonObject detail;
            detail.insert("pairIre", QJsonArray::fromVariantList({std::round(a * 100) / 100, std::round(b * 100) / 100}));
            detail.insert("backgroundIre", std::round(background * 100) / 100);
            detail.insert("contrast", std::round(contrast * 100) / 100);
            detail.insert("neighboursIre", QJsonArray::fromVariantList({std::round(neighbourBefore * 100) / 100,
                                                                        std::round(neighbourAfter * 100) / 100}));
            detail.insert("lumaShiftIre", std::isfinite(lumaShift) ? std::round(lumaShift * 100) / 100 : 0.0);
            mark.detailJson = QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Compact));
            mark.createdBy = createdBy;
            mark.updatedAt = stamp;
            mark.derivedFrom = derivedFrom;
            candidates.append({i, contrast, mark});
        }
    }

    // Marks closer than four fields: the stronger stands
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &x, const Candidate &y) {
        return x.contrast > y.contrast;
    });
    QVector<Candidate> kept;
    for (const Candidate &c : candidates) {
        bool clear = true;
        for (const Candidate &k : kept) {
            if (std::abs(k.field - c.field) < 4) { clear = false; break; }
        }
        if (clear) kept.append(c);
    }
    std::sort(kept.begin(), kept.end(), [](const Candidate &x, const Candidate &y) { return x.field < y.field; });
    for (const Candidate &c : kept) {
        SegmentMark mark = c.mark;
        mark.id = marks.size() + 1;
        marks.append(mark);
    }
    return marks;
}

QVector<SegmentMark> mergeMarks(const QVector<SegmentMark> &stored, const QVector<SegmentMark> &detected)
{
    QVector<SegmentMark> result;
    qint32 nextId = 0;   // above every stored id: a vanished mark's id is never reused
    for (const SegmentMark &mark : stored) {
        nextId = std::max(nextId, mark.id);
        if (mark.source == QLatin1String("user") || !mark.enabled) {
            result.append(mark);
        }
    }
    const qint32 keptCount = result.size();
    for (SegmentMark mark : detected) {
        bool covered = false;
        for (qint32 k = 0; k < keptCount; k++) {
            if (std::abs(result[k].field - mark.field) <= 2) { covered = true; break; }
        }
        if (covered) continue;
        // A derived mark the detection finds again keeps its id
        qint32 id = -1;
        for (const SegmentMark &old : stored) {
            if (old.source != QLatin1String("user") && old.enabled && std::abs(old.field - mark.field) <= 2) {
                id = old.id;
                break;
            }
        }
        mark.id = id;
        result.append(mark);
    }
    for (const SegmentMark &mark : result) nextId = std::max(nextId, mark.id);
    for (SegmentMark &mark : result) {
        if (mark.id < 1) mark.id = ++nextId;
    }
    std::stable_sort(result.begin(), result.end(), [](const SegmentMark &x, const SegmentMark &y) { return x.field < y.field; });
    return result;
}

QVector<TbcMetaData::Segment> splitSegmentsAtMarks(const QVector<TbcMetaData::Segment> &segments,
                                                   const QVector<SegmentMark> &marks)
{
    QVector<qint32> cuts;
    for (const SegmentMark &mark : marks) {
        if (mark.enabled && mark.split) cuts.append(mark.field);
    }
    if (cuts.isEmpty()) return segments;
    std::sort(cuts.begin(), cuts.end());
    qint32 nextId = 0;
    for (const TbcMetaData::Segment &segment : segments) nextId = std::max(nextId, segment.id);

    QVector<TbcMetaData::Segment> result;
    for (const TbcMetaData::Segment &segment : segments) {
        if (segment.kind != QLatin1String("clip")) {
            result.append(segment);
            continue;
        }
        TbcMetaData::Segment piece = segment;
        for (qint32 cut : cuts) {
            if (cut <= piece.startField || cut >= piece.endFieldExclusive) continue;
            TbcMetaData::Segment head = piece;
            head.endFieldExclusive = cut;
            result.append(head);
            piece.id = ++nextId;
            piece.startField = cut;
            piece.title.clear();
            piece.comment.clear();
        }
        result.append(piece);
    }
    std::stable_sort(result.begin(), result.end(), [](const TbcMetaData::Segment &x, const TbcMetaData::Segment &y) {
        return x.startField < y.startField;
    });
    return result;
}

QVector<SegmentEvent> restartEvents(const QVector<SegmentMark> &marks)
{
    QVector<SegmentEvent> events;
    for (const SegmentMark &mark : marks) {
        if (!mark.enabled) continue;
        SegmentEvent event;
        event.kind = mark.kind;
        event.startField = mark.field;
        event.endFieldExclusive = mark.field + 1;
        event.severity = mark.confidence;
        QJsonObject detail;
        detail.insert("markId", mark.id);
        detail.insert("source", mark.source);
        detail.insert("split", mark.split);
        detail.insert("reason", mark.reason);
        const QJsonDocument measurements = QJsonDocument::fromJson(mark.detailJson.toUtf8());
        if (measurements.isObject()) detail.insert("measurements", measurements.object());
        event.detail = detail;
        events.append(event);
    }
    return events;
}

void addMarksToReport(QJsonObject *report, const TbcMetaData &metaData, const SegmentsAnalysis &analysis,
                      const QVector<SegmentMark> &marks, const QString &marksSource)
{
    if (!report) return;
    const double spf = analysis.secondsPerField;
    QJsonArray marksArray;
    for (const SegmentMark &mark : marks) {
        QJsonObject o;
        o.insert("id", mark.id);
        o.insert("field", mark.field);
        o.insert("seconds", mark.field * spf);
        o.insert("frame", frameContainingField(metaData, mark.field));
        o.insert("kind", mark.kind);
        o.insert("source", mark.source);
        o.insert("enabled", mark.enabled);
        o.insert("split", mark.split);
        o.insert("confidence", mark.confidence);
        o.insert("reason", mark.reason);
        o.insert("comment", mark.comment);
        o.insert("createdBy", mark.createdBy);
        o.insert("updatedAt", mark.updatedAt);
        const QJsonDocument measurements = QJsonDocument::fromJson(mark.detailJson.toUtf8());
        if (measurements.isObject()) o.insert("detail", measurements.object());
        marksArray.append(o);
    }
    report->insert("marks", marksArray);
    report->insert("marksSource", marksSource);

    // Restart events join the event list in field order
    QVector<QJsonObject> events;
    for (const QJsonValue &value : report->value("events").toArray()) events.append(value.toObject());
    qint32 restarts = 0;
    for (const SegmentEvent &event : restartEvents(marks)) {
        QJsonObject o;
        o.insert("kind", event.kind);
        o.insert("startField", event.startField);
        o.insert("endFieldExclusive", event.endFieldExclusive);
        o.insert("startSeconds", event.startField * spf);
        o.insert("endSeconds", event.endFieldExclusive * spf);
        o.insert("severity", std::round(event.severity * 1000.0) / 1000.0);
        o.insert("detail", event.detail);
        events.append(o);
        restarts++;
    }
    std::stable_sort(events.begin(), events.end(), [](const QJsonObject &x, const QJsonObject &y) {
        const qint32 a = x.value("startField").toInt(), b = y.value("startField").toInt();
        if (a != b) return a < b;
        return x.value("kind").toString() < y.value("kind").toString();
    });
    QJsonArray eventsArray;
    for (const QJsonObject &o : events) eventsArray.append(o);
    report->insert("events", eventsArray);
    QJsonObject counts = report->value("counts").toObject();
    counts.insert("restart", restarts);
    report->insert("counts", counts);
}
