/************************************************************************

    segmentexportplan.cpp

    tbc-segments - Which recording segments a per-segment export runs, and how
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
#include <algorithm>

SegmentExportPlan planSegmentExports(const QVector<TbcMetaData::Segment> &segmentsIn,
                                     const QString &selection,
                                     const QString &outputBase,
                                     bool appendTitle,
                                     const SegmentFrameRangeFn &frameRange)
{
    SegmentExportPlan plan;
    QVector<TbcMetaData::Segment> segments = segmentsIn;
    std::stable_sort(segments.begin(), segments.end(), [](const TbcMetaData::Segment &a, const TbcMetaData::Segment &b) {
        return a.startField < b.startField;
    });
    qint32 maxId = 1;
    for (const TbcMetaData::Segment &segment : segments) {
        maxId = qMax(maxId, segment.id);
    }
    const int digits = qMax(2, QString::number(maxId).size());
    for (const TbcMetaData::Segment &segment : segments) {
        if (selection != QLatin1String("all") && !segment.enabled) {
            continue;
        }
        if (selection == QLatin1String("enabled_clips") && segment.kind != QLatin1String("clip")) {
            continue;
        }
        qint32 startFrame = 0;
        qint32 lengthFrames = 0;
        if (!frameRange || !frameRange(segment, &startFrame, &lengthFrames)) {
            plan.skippedNoWholeFrame.append(segment.id);
            continue;
        }
        QString base = outputBase + QStringLiteral("_") + QString::number(segment.id).rightJustified(digits, QLatin1Char('0'));
        if (appendTitle && !segment.title.trimmed().isEmpty()) {
            QString title = segment.title.trimmed();
            title.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
            title = title.left(40);
            title.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
            if (!title.isEmpty()) {
                base += QStringLiteral("_") + title;
            }
        }
        SegmentExportItem item;
        item.segmentId = segment.id;
        item.startFrameOneBased = startFrame;
        item.lengthFrames = lengthFrames;
        item.outputBase = base;
        item.label = segment.title;
        plan.items.append(item);
    }
    return plan;
}
