/************************************************************************

    segmentsreport.cpp

    tbc-export-metadata - Export ld-decode metadata into other formats
    Copyright (C) 2026 GDH-Technologies LLC

    This file is part of tbc-tools.

    tbc-export-metadata is free software: you can redistribute it and/or
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

#include "segmentsreport.h"

#include "segments.h"
#include "tbc/logging.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cstdio>

bool writeSegmentsJson(TbcMetaData &metaData,
                       const QString &fileName,
                       const QString &inputPath,
                       qint32 startFrameOneBased,
                       qint32 lengthFrames)
{
    FieldRange range;
    if (!resolveFieldRange(metaData, startFrameOneBased, lengthFrames, &range)) {
        tbcDebug(QStringLiteral("writeSegmentsJson: Could not resolve export start/length range"));
        return false;
    }

    const SegmentsThresholds thresholds;
    const FieldMetrics metrics = FieldMetrics::fromMetadata(metaData);
    const FieldMetrics *metricsPtr = metrics.enabled ? &metrics : nullptr;
    const SegmentsAnalysis analysis = analyseSegments(metaData, range, thresholds, 0.0, metricsPtr);

    QVector<TbcMetaData::Segment> segments = metaData.getSegments();
    QString segmentsSource = QStringLiteral("stored");
    if (segments.isEmpty()) {
        segments = deriveSegments(metaData, analysis, thresholds, QStringLiteral("tbc-export-metadata"));
        segmentsSource = segments.isEmpty() ? QStringLiteral("none") : QStringLiteral("derived");
    }

    QJsonObject fieldDataInfo;
    fieldDataInfo.insert(QStringLiteral("source"), metrics.enabled ? QStringLiteral("stored") : QStringLiteral("none"));
    if (metrics.enabled) {
        fieldDataInfo.insert(QStringLiteral("storedComplete"), FieldMetrics::metadataIsComplete(metaData));
    }

    const QString inputKind = TbcMetaData::isJsonMetadataFilename(inputPath)
                                  ? QStringLiteral("json")
                                  : QStringLiteral("sqlite");
    QJsonObject report = buildReport(metaData, inputPath, inputKind, startFrameOneBased, lengthFrames, range,
                                     thresholds, analysis, metricsPtr, fieldDataInfo, false, segments, segmentsSource);
    QJsonObject tool = report.value(QStringLiteral("tool")).toObject();
    tool.insert(QStringLiteral("name"), QStringLiteral("tbc-export-metadata"));
    report.insert(QStringLiteral("tool"), tool);

    const QByteArray text = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (fileName == QLatin1String("-")) {
        std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stdout);
        std::fflush(stdout);
        return true;
    }
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        tbcDebug(QStringLiteral("writeSegmentsJson: Could not open file for output"));
        return false;
    }
    file.write(text);
    return file.commit();
}
