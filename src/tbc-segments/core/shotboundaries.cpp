/************************************************************************

    shotboundaries.cpp

    tbc-segments - Shot boundaries
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

#include "shotboundaries.h"

#include "sceneshots.h"

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

bool ShotBoundary::operator==(const ShotBoundary &o) const
{
    return id == o.id && field == o.field && evidence == o.evidence && source == o.source && verdict == o.verdict
           && sceneChange == o.sceneChange && sceneTitle == o.sceneTitle && sceneEnabled == o.sceneEnabled
           && shotEnabled == o.shotEnabled && std::abs(confidence - o.confidence) < 1e-9 && reason == o.reason
           && detailJson == o.detailJson && comment == o.comment && createdBy == o.createdBy
           && updatedAt == o.updatedAt && derivedFrom == o.derivedFrom;
}

QVector<ShotBoundary> detectShotBoundaries(const SegmentsAnalysis &analysis,
                                           const FieldMetrics *metricsIn,
                                           const SegmentsThresholds &t,
                                           const QVector<TbcMetaData::Segment> &baseSegments,
                                           const QString &createdBy)
{
    QVector<ShotBoundary> boundaries;
    const qint32 n = analysis.numberOfFields;
    if (!t.detectShotBoundaries || n < 1) return boundaries;
    static const FieldMetrics noMetrics;
    const bool havePicture = metricsIn && metricsIn->enabled && metricsIn->fieldDiffIre.size() == n;
    const FieldMetrics &metrics = havePicture ? *metricsIn : noMetrics;
    const QVector<double> &D = metrics.fieldDiffIre;
    const QVector<double> &L = metrics.lumaMeanIre;
    const QVector<double> &N = metrics.noiseIre;
    const double noiseLimit = analysis.effectiveNoiseThresholdIre > 0 ? analysis.effectiveNoiseThresholdIre : t.noiseThresholdIre;
    const qint32 edge = std::max(2, t.shotBoundaryFlashWindowFields);

    QJsonObject provenance;
    provenance.insert("tool", createdBy);
    QJsonObject thresholds;
    thresholds.insert("shotBoundaryPairMatch", t.shotBoundaryPairMatch);
    thresholds.insert("shotBoundaryMinPeakIre", t.shotBoundaryMinPeakIre);
    thresholds.insert("shotBoundaryMinContrast", t.shotBoundaryMinContrast);
    thresholds.insert("shotBoundaryMaxNeighbourRatio", t.shotBoundaryMaxNeighbourRatio);
    thresholds.insert("shotBoundaryFlashWindowFields", t.shotBoundaryFlashWindowFields);
    thresholds.insert("shotBoundaryFlashIre", t.shotBoundaryFlashIre);
    provenance.insert("thresholds", thresholds);
    const QString derivedFrom = QString::fromUtf8(QJsonDocument(provenance).toJson(QJsonDocument::Compact));
    const QString stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    struct Candidate { qint32 field; double strength; ShotBoundary boundary; };
    QVector<Candidate> candidates;

    // Picture evidence (Glossary: evidence, #evidence)
    for (const TbcMetaData::Segment &segment : baseSegments) {
        if (!havePicture || !isContentSpanKind(segment.kind)) continue;
        const qint32 first = std::max(segment.startField + edge, 2);
        const qint32 last = std::min(segment.endFieldExclusive - edge, n - 15);   // exclusive
        for (qint32 i = first; i < last; i++) {
            const double a = D[i], b = D[i + 1];
            if (!std::isfinite(a) || !std::isfinite(b)) continue;
            const double peak = std::max(a, b);
            if (peak < t.shotBoundaryMinPeakIre) continue;
            if (std::min(a, b) < t.shotBoundaryPairMatch * peak) continue;

            const double before = medianOfRange(D, i - 12, i - 2);
            const double after = medianOfRange(D, i + 3, i + 14);
            double background = std::max(std::isfinite(before) ? before : 0.0, std::isfinite(after) ? after : 0.0);
            background = std::max(background, 0.05);
            const double contrast = peak / background;
            if (contrast < t.shotBoundaryMinContrast) continue;

            // Quiet neighbours: no higher than halfway from the background to
            // the peak, and no more than shotBoundaryMaxNeighbourRatio of the peak
            // (sustained motion keeps the fields around a spike high)
            const double quietLimit = std::min(background + 0.5 * (peak - background), t.shotBoundaryMaxNeighbourRatio * peak);
            const double neighbourBefore = D[i - 1], neighbourAfter = D[i + 2];
            if (!(std::isfinite(neighbourBefore) && neighbourBefore <= quietLimit)) continue;
            if (!(std::isfinite(neighbourAfter) && neighbourAfter <= quietLimit)) continue;

            if ((std::isfinite(N[i]) && N[i] >= noiseLimit) || (std::isfinite(N[i + 1]) && N[i + 1] >= noiseLimit)) continue;

            bool flash = false;
            for (qint32 j = std::max(1, i - t.shotBoundaryFlashWindowFields); j < i && !flash; j++) {
                flash = isFlashField(L, j, t.shotBoundaryFlashIre);
            }
            if (flash) continue;

            // Confidence: 50% for passing, the rest from how cleanly it passes
            const double pairScore = clamp01((std::min(a, b) / peak - t.shotBoundaryPairMatch) / std::max(1e-6, 1.0 - t.shotBoundaryPairMatch));
            const double contrastScore = clamp01((contrast - t.shotBoundaryMinContrast) / std::max(1e-6, 4.0 - t.shotBoundaryMinContrast));
            const double loudestNeighbour = std::max(neighbourBefore, neighbourAfter);
            const double quietScore = clamp01(1.0 - (loudestNeighbour - background) / std::max(1e-6, quietLimit - background));
            const double confidence = 0.5 + 0.5 * (0.3 * pairScore + 0.4 * contrastScore + 0.3 * quietScore);

            const double lumaShift = medianOfRange(L, i + 2, i + 11) - medianOfRange(L, i - 10, i - 1);

            ShotBoundary boundary;
            boundary.field = i;
            boundary.evidence = QStringLiteral("picture");
            boundary.confidence = std::round(confidence * 1000.0) / 1000.0;
            boundary.reason = QStringLiteral("matched field pair %1/%2 IRE (%3%), %4x background, neighbours quiet "
                                         "(%5/%6 IRE), no flash in %7 fields; luma %8%9 IRE")
                              .arg(number(a, 1), number(b, 1))
                              .arg(qRound(100.0 * std::min(a, b) / peak))
                              .arg(number(contrast, 1))
                              .arg(number(neighbourBefore, 1), number(neighbourAfter, 1))
                              .arg(t.shotBoundaryFlashWindowFields)
                              .arg(lumaShift >= 0 ? QStringLiteral("+") : QString())
                              .arg(number(std::isfinite(lumaShift) ? lumaShift : 0.0, 1));
            QJsonObject detail;
            detail.insert("pairIre", QJsonArray::fromVariantList({std::round(a * 100) / 100, std::round(b * 100) / 100}));
            detail.insert("backgroundIre", std::round(background * 100) / 100);
            detail.insert("contrast", std::round(contrast * 100) / 100);
            detail.insert("neighboursIre", QJsonArray::fromVariantList({std::round(neighbourBefore * 100) / 100,
                                                                        std::round(neighbourAfter * 100) / 100}));
            detail.insert("lumaShiftIre", std::isfinite(lumaShift) ? std::round(lumaShift * 100) / 100 : 0.0);
            boundary.detailJson = QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Compact));
            boundary.createdBy = createdBy;
            boundary.updatedAt = stamp;
            boundary.derivedFrom = derivedFrom;
            candidates.append({i, contrast, boundary});
        }
    }

    // RF-gap evidence (Glossary: RF gap, #rf-gap): a gap strictly inside a
    // content span is where the decoder lost sync at a seamless shot boundary
    for (const SegmentEvent &event : analysis.events) {
        if (event.kind != QLatin1String("gap")) continue;
        const qint32 field = event.startField;
        bool insideSpan = false;
        for (const TbcMetaData::Segment &segment : baseSegments) {
            if (isContentSpanKind(segment.kind) && field > segment.startField && field < segment.endFieldExclusive) {
                insideSpan = true;
                break;
            }
        }
        if (!insideSpan) continue;
        const bool confirmed = event.detail.value(QStringLiteral("decoderConfirmed")).toBool();
        ShotBoundary boundary;
        boundary.field = field;
        boundary.evidence = QStringLiteral("rf_gap");
        boundary.confidence = confirmed ? 0.9 : 0.75;
        boundary.reason = QStringLiteral("RF gap inside a content span: the RF position jumped %1 fields%2")
                              .arg(number(event.detail.value(QStringLiteral("deltaFields")).toDouble(), 2))
                              .arg(confirmed ? QStringLiteral(" (the decoder recorded the seam)") : QString());
        boundary.detailJson = QString::fromUtf8(QJsonDocument(event.detail).toJson(QJsonDocument::Compact));
        boundary.createdBy = createdBy;
        boundary.updatedAt = stamp;
        boundary.derivedFrom = derivedFrom;
        candidates.append({field, 0.0, boundary});   // picture evidence at the same place wins
    }

    // Boundaries closer than four fields: the stronger stands
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &x, const Candidate &y) {
        return x.strength > y.strength;
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
        ShotBoundary boundary = c.boundary;
        boundary.id = boundaries.size() + 1;
        boundaries.append(boundary);
    }
    return boundaries;
}

QVector<ShotBoundary> mergeShotBoundaries(const QVector<ShotBoundary> &stored, const QVector<ShotBoundary> &detected)
{
    QVector<ShotBoundary> result;
    qint32 nextId = 0;   // above every stored id: a vanished boundary's id is never reused
    for (const ShotBoundary &boundary : stored) {
        nextId = std::max(nextId, boundary.id);
        if (boundary.source == QLatin1String("user") || boundary.rejected()) {
            result.append(boundary);
        }
    }
    const qint32 keptCount = result.size();
    for (ShotBoundary boundary : detected) {
        bool covered = false;
        for (qint32 k = 0; k < keptCount; k++) {
            if (std::abs(result[k].field - boundary.field) <= 2) { covered = true; break; }
        }
        if (covered) continue;
        // A derived boundary the detection finds again keeps its id
        qint32 id = -1;
        for (const ShotBoundary &old : stored) {
            if (old.source != QLatin1String("user") && !old.rejected() && std::abs(old.field - boundary.field) <= 2) {
                id = old.id;
                break;
            }
        }
        boundary.id = id;
        result.append(boundary);
    }
    for (const ShotBoundary &boundary : result) nextId = std::max(nextId, boundary.id);
    for (ShotBoundary &boundary : result) {
        if (boundary.id < 1) boundary.id = ++nextId;
    }
    std::stable_sort(result.begin(), result.end(), [](const ShotBoundary &x, const ShotBoundary &y) { return x.field < y.field; });
    return result;
}

QVector<SegmentEvent> shotBoundaryEvents(const QVector<ShotBoundary> &boundaries)
{
    QVector<SegmentEvent> events;
    for (const ShotBoundary &boundary : boundaries) {
        if (boundary.rejected()) continue;
        SegmentEvent event;
        event.kind = QStringLiteral("shot_boundary");
        event.startField = boundary.field;
        event.endFieldExclusive = boundary.field + 1;
        event.severity = boundary.confidence;
        QJsonObject detail;
        detail.insert("shotBoundaryId", boundary.id);
        detail.insert("evidence", boundary.evidence);
        detail.insert("source", boundary.source);
        detail.insert("verdict", boundary.verdict);
        detail.insert("sceneChange", boundary.sceneChange);
        detail.insert("reason", boundary.reason);
        const QJsonDocument measurements = QJsonDocument::fromJson(boundary.detailJson.toUtf8());
        if (measurements.isObject()) detail.insert("measurements", measurements.object());
        event.detail = detail;
        events.append(event);
    }
    return events;
}

void addShotBoundariesToReport(QJsonObject *report, const TbcMetaData &metaData, const SegmentsAnalysis &analysis,
                               const QVector<TbcMetaData::Segment> &segments,
                               const QVector<ShotBoundary> &boundaries, const QString &boundariesSource)
{
    if (!report) return;
    const double spf = analysis.secondsPerField;
    QJsonArray array;
    for (const ShotBoundary &boundary : boundaries) {
        QJsonObject o;
        o.insert("id", boundary.id);
        o.insert("field", boundary.field);
        o.insert("seconds", boundary.field * spf);
        o.insert("frame", frameContainingField(metaData, boundary.field));
        o.insert("evidence", boundary.evidence);
        o.insert("source", boundary.source);
        o.insert("verdict", boundary.verdict);
        o.insert("sceneChange", boundary.sceneChange);
        o.insert("sceneTitle", boundary.sceneTitle);
        o.insert("sceneEnabled", boundary.sceneEnabled);
        o.insert("shotEnabled", boundary.shotEnabled);
        o.insert("confidence", boundary.confidence);
        o.insert("reason", boundary.reason);
        o.insert("comment", boundary.comment);
        o.insert("createdBy", boundary.createdBy);
        o.insert("updatedAt", boundary.updatedAt);
        const QJsonDocument measurements = QJsonDocument::fromJson(boundary.detailJson.toUtf8());
        if (measurements.isObject()) o.insert("detail", measurements.object());
        array.append(o);
    }
    report->insert("shotBoundaries", array);
    report->insert("shotBoundariesSource", boundariesSource);

    // shot_boundary events join the event list in field order
    QVector<QJsonObject> events;
    for (const QJsonValue &value : report->value("events").toArray()) events.append(value.toObject());
    qint32 count = 0;
    for (const SegmentEvent &event : shotBoundaryEvents(boundaries)) {
        QJsonObject o;
        o.insert("kind", event.kind);
        o.insert("startField", event.startField);
        o.insert("endFieldExclusive", event.endFieldExclusive);
        o.insert("startSeconds", event.startField * spf);
        o.insert("endSeconds", event.endFieldExclusive * spf);
        o.insert("severity", std::round(event.severity * 1000.0) / 1000.0);
        o.insert("detail", event.detail);
        events.append(o);
        count++;
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
    counts.insert("shot_boundary", count);
    report->insert("counts", counts);

    // Glossary: scene (#scene), shot (#shot): worked out, so any caller can export by them
    report->insert("scenes", partsToJson(metaData, scenesOf(segments, boundaries), spf));
    report->insert("shots", partsToJson(metaData, shotsOf(segments, boundaries), spf));
}
