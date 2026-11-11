/************************************************************************

    sceneshots.cpp

    tbc-segments - Scenes and shots, worked out from the shot boundaries
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

#include "sceneshots.h"

#include <QJsonObject>
#include <algorithm>

namespace {

// Glossary: title (docs/Tools/tbc-segments-glossary.md#title), of a content span
QString spanTitle(const TbcMetaData::Segment &span)
{
    return span.title.trimmed().isEmpty() ? QStringLiteral("Segment %1").arg(span.id) : span.title.trimmed();
}

// The unrejected boundaries strictly inside a span, in field order
QVector<ShotBoundary> boundariesInside(const TbcMetaData::Segment &span, const QVector<ShotBoundary> &boundaries)
{
    QVector<ShotBoundary> inside;
    for (const ShotBoundary &boundary : boundaries) {
        if (!boundary.rejected() && boundary.field > span.startField && boundary.field < span.endFieldExclusive) {
            inside.append(boundary);
        }
    }
    std::stable_sort(inside.begin(), inside.end(), [](const ShotBoundary &a, const ShotBoundary &b) {
        return a.field < b.field;
    });
    return inside;
}

QVector<TbcMetaData::Segment> spansInOrder(const QVector<TbcMetaData::Segment> &segments)
{
    QVector<TbcMetaData::Segment> spans;
    for (const TbcMetaData::Segment &segment : segments) {
        if (isContentSpanKind(segment.kind)) spans.append(segment);
    }
    std::stable_sort(spans.begin(), spans.end(), [](const TbcMetaData::Segment &a, const TbcMetaData::Segment &b) {
        return a.startField < b.startField;
    });
    return spans;
}

} // namespace

// Glossary: scene (#scene), scene change (#scene-change)
QVector<SegmentPart> scenesOf(const QVector<TbcMetaData::Segment> &segments, const QVector<ShotBoundary> &boundaries)
{
    QVector<SegmentPart> scenes;
    for (const TbcMetaData::Segment &span : spansInOrder(segments)) {
        SegmentPart current;
        current.spanId = span.id;
        current.number = 1;
        current.startField = span.startField;
        current.title = spanTitle(span);
        current.enabled = span.enabled;
        for (const ShotBoundary &boundary : boundariesInside(span, boundaries)) {
            if (!boundary.sceneChange) continue;
            current.endFieldExclusive = boundary.field;
            scenes.append(current);
            const qint32 number = current.number + 1;
            current = SegmentPart();
            current.spanId = span.id;
            current.number = number;
            current.startField = boundary.field;
            current.boundaryId = boundary.id;
            current.title = boundary.sceneTitle.trimmed().isEmpty()
                                ? QStringLiteral("%1 · scene %2").arg(spanTitle(span)).arg(number)
                                : boundary.sceneTitle.trimmed();
            current.enabled = boundary.sceneEnabled;
        }
        current.endFieldExclusive = span.endFieldExclusive;
        scenes.append(current);
    }
    for (qint32 i = 0; i < scenes.size(); i++) scenes[i].id = i + 1;
    return scenes;
}

// Glossary: shot (#shot), shot boundary (#shot-boundary)
QVector<SegmentPart> shotsOf(const QVector<TbcMetaData::Segment> &segments, const QVector<ShotBoundary> &boundaries)
{
    const QVector<SegmentPart> scenes = scenesOf(segments, boundaries);
    QVector<TbcMetaData::Segment> spans = spansInOrder(segments);
    QVector<SegmentPart> shots;
    for (const SegmentPart &scene : scenes) {
        TbcMetaData::Segment span;
        for (const TbcMetaData::Segment &candidate : spans) {
            if (candidate.id == scene.spanId) span = candidate;
        }
        SegmentPart current;
        current.spanId = scene.spanId;
        current.sceneId = scene.id;
        current.number = 1;
        current.startField = scene.startField;
        current.boundaryId = scene.boundaryId;
        current.title = QStringLiteral("%1 · 1").arg(scene.title);
        // The first shot of a span takes the span's flag; a shot a scene
        // change starts takes that boundary's shot flag
        current.enabled = span.enabled;
        for (const ShotBoundary &boundary : boundaries) {
            if (boundary.id == scene.boundaryId && scene.boundaryId >= 0) current.enabled = boundary.shotEnabled;
        }
        TbcMetaData::Segment sceneRange = span;
        sceneRange.startField = scene.startField;
        sceneRange.endFieldExclusive = scene.endFieldExclusive;
        for (const ShotBoundary &boundary : boundariesInside(sceneRange, boundaries)) {
            current.endFieldExclusive = boundary.field;
            shots.append(current);
            const qint32 number = current.number + 1;
            current = SegmentPart();
            current.spanId = scene.spanId;
            current.sceneId = scene.id;
            current.number = number;
            current.startField = boundary.field;
            current.boundaryId = boundary.id;
            current.title = QStringLiteral("%1 · %2").arg(scene.title).arg(number);
            current.enabled = boundary.shotEnabled;
        }
        current.endFieldExclusive = scene.endFieldExclusive;
        shots.append(current);
    }
    for (qint32 i = 0; i < shots.size(); i++) shots[i].id = i + 1;
    return shots;
}

TbcMetaData::Segment partAsSegment(const SegmentPart &part)
{
    TbcMetaData::Segment segment;
    segment.id = part.id;
    segment.startField = part.startField;
    segment.endFieldExclusive = part.endFieldExclusive;
    segment.kind = contentSpanKind();
    segment.enabled = part.enabled;
    segment.title = part.title;
    return segment;
}

QJsonArray partsToJson(const TbcMetaData &metaData, const QVector<SegmentPart> &parts, double secondsPerField)
{
    QJsonArray array;
    for (const SegmentPart &part : parts) {
        QJsonObject o;
        o.insert("id", part.id);
        o.insert("spanId", part.spanId);
        if (part.sceneId > 0) o.insert("sceneId", part.sceneId);
        o.insert("number", part.number);
        o.insert("startField", part.startField);
        o.insert("endFieldExclusive", part.endFieldExclusive);
        o.insert("startSeconds", part.startField * secondsPerField);
        o.insert("endSeconds", part.endFieldExclusive * secondsPerField);
        o.insert("shotBoundaryId", part.boundaryId);
        o.insert("title", part.title);
        o.insert("enabled", part.enabled);
        qint32 startFrame = 0, lengthFrames = 0;
        if (!segmentFrameRange(metaData, partAsSegment(part), &startFrame, &lengthFrames)) {
            startFrame = 0;
            lengthFrames = 0;
        }
        o.insert("startFrame", startFrame);
        o.insert("lengthFrames", lengthFrames);
        array.append(o);
    }
    return array;
}
