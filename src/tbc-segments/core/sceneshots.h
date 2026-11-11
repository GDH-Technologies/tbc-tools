/************************************************************************

    sceneshots.h

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

#ifndef SCENESHOTS_H
#define SCENESHOTS_H

#include <QJsonArray>
#include <QString>
#include <QVector>

#include "shotboundaries.h"
#include "tbcmetadata.h"

// A scene or a shot inside a content span: never stored, always worked out
// from the stored segments and shot boundaries.
// Glossary: scene (docs/Tools/tbc-segments-glossary.md#scene), shot (#shot).
struct SegmentPart {
    qint32 id = 0;                  // 1..N in field order, over the whole file
    qint32 spanId = 0;              // the content span segment it lies in
    qint32 sceneId = 0;             // for a shot: the scene it lies in
    qint32 number = 0;              // 1-based within its span (scene) or scene (shot)
    qint32 startField = 0;
    qint32 endFieldExclusive = 0;
    qint32 boundaryId = -1;         // the shot boundary that starts it; -1 for a span's first
    QString title;                  // Glossary: title (#title)
    bool enabled = true;            // Glossary: enabled (#enabled), at this level only
};

/*!
    The scenes of every content span: a span starts a scene, and so does each
    shot boundary inside it that is a scene change and not rejected.
    Title: the scene change's sceneTitle, else "<span title> · scene n"; the
    first scene of a span takes the span's title. Enabled: the scene change's
    sceneEnabled; the first scene takes the span's flag. A span's title is its
    segment title, else "Segment <id>".
*/
QVector<SegmentPart> scenesOf(const QVector<TbcMetaData::Segment> &segments, const QVector<ShotBoundary> &boundaries);

/*!
    The shots of every content span: a scene starts a shot, and so does each
    shot boundary inside it that is not rejected. Title: "<scene title> · n".
    Enabled: the starting boundary's shotEnabled; the first shot of a span
    takes the span's flag.
*/
QVector<SegmentPart> shotsOf(const QVector<TbcMetaData::Segment> &segments, const QVector<ShotBoundary> &boundaries);

// The parts as a report array, each with its frames under the mixed-frame rule.
QJsonArray partsToJson(const TbcMetaData &metaData, const QVector<SegmentPart> &parts, double secondsPerField);

// A part as a segment (for segmentFrameRange and export planning).
TbcMetaData::Segment partAsSegment(const SegmentPart &part);

#endif // SCENESHOTS_H
