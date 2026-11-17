/************************************************************************

    segmentexportplan.cpp

    tbc-segments - Which segments a per-segment export runs, and how
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

#include "segmentexportplan.h"

#include <QRegularExpression>

#include "sceneshots.h"
#include <algorithm>

SegmentExportPlan planSegmentExports(const QVector<TbcMetaData::Segment> &segmentsIn,
                                     const QVector<ShotBoundary> &boundaries,
                                     const QString &selectionIn,
                                     const QString &outputBase,
                                     bool appendTitle,
                                     const SegmentFrameRangeFn &frameRange)
{
    SegmentExportPlan plan;
    // glossary-legacy: tbc-analyse and digitization-toolkit stored this value before 3.3.1-gdh-1.2
    const QString selection = selectionIn == QLatin1String("enabled_clips") ? QStringLiteral("enabled_content_spans") // glossary-legacy
                                                                            : selectionIn;
    QVector<TbcMetaData::Segment> units;
    QString prefix;
    if (selection == QLatin1String("enabled_scenes") || selection == QLatin1String("enabled_shots")) {
        const bool scenes = selection == QLatin1String("enabled_scenes");
        prefix = scenes ? QStringLiteral("scene") : QStringLiteral("shot");
        for (const SegmentPart &part : scenes ? scenesOf(segmentsIn, boundaries) : shotsOf(segmentsIn, boundaries)) {
            if (part.enabled) units.append(partAsSegment(part));
        }
    } else {
        units = segmentsIn;
        std::stable_sort(units.begin(), units.end(), [](const TbcMetaData::Segment &a, const TbcMetaData::Segment &b) {
            return a.startField < b.startField;
        });
        QVector<TbcMetaData::Segment> selected;
        for (const TbcMetaData::Segment &segment : units) {
            if (selection != QLatin1String("all") && !segment.enabled) continue;
            if (selection == QLatin1String("enabled_content_spans") && !isContentSpanKind(segment.kind)) continue;
            selected.append(segment);
        }
        units = selected;
    }
    qint32 maxId = 1;
    for (const TbcMetaData::Segment &unit : units) {
        maxId = qMax(maxId, unit.id);
    }
    const int digits = qMax(2, QString::number(maxId).size());
    for (const TbcMetaData::Segment &unit : units) {
        qint32 startFrame = 0;
        qint32 lengthFrames = 0;
        if (!frameRange || !frameRange(unit, &startFrame, &lengthFrames)) {
            plan.skippedNoWholeFrame.append(unit.id);
            continue;
        }
        QString base = outputBase + QStringLiteral("_") + prefix + QString::number(unit.id).rightJustified(digits, QLatin1Char('0'));
        if (appendTitle && !unit.title.trimmed().isEmpty()) {
            QString title = unit.title.trimmed();
            title.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
            title = title.left(40);
            title.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
            if (!title.isEmpty()) {
                base += QStringLiteral("_") + title;
            }
        }
        SegmentExportItem item;
        item.segmentId = unit.id;
        item.startFrameOneBased = startFrame;
        item.lengthFrames = lengthFrames;
        item.outputBase = base;
        item.label = unit.title;
        plan.items.append(item);
    }
    return plan;
}
