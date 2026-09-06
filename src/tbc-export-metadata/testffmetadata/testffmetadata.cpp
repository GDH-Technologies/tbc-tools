/************************************************************************

    testffmetadata.cpp

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

// Recording segments as FFMETADATA1 chapters, and the segments JSON report,
// over synthetic NTSC metadata: chapter placement, START/END rebased to the
// export start field, titles, disabled segments, the navigation fallback,
// the user-marker splice, and the frame ranges of the JSON report.

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>
#include <cstdlib>
#include <iostream>

#include "ffmetadata.h"
#include "segmentsreport.h"
#include "tbcmetadata.h"

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

namespace {

const qint64 kNominal = 667333;   // 40 MHz / 59.94

TbcMetaData::VideoParameters ntscParameters(qint32 fields)
{
    TbcMetaData::VideoParameters vp;
    vp.system = NTSC;
    vp.numberOfSequentialFields = fields;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.sampleRate = 4.0 * 315.0 / 88.0 * 1e6;
    vp.colourBurstStart = 76;
    vp.colourBurstEnd = 112;
    vp.activeVideoStart = 135;
    vp.activeVideoEnd = 895;
    vp.firstActiveFieldLine = 20;
    vp.lastActiveFieldLine = 259;
    vp.firstActiveFrameLine = 40;
    vp.lastActiveFrameLine = 525;
    vp.black16bIre = 16384;
    vp.white16bIre = 54016;
    vp.blanking16bIre = 16384;
    vp.tapeFormat = QStringLiteral("VHS");
    vp.isValid = true;
    return vp;
}

// First field first: frame F (1-based) = fields (2F-2, 2F-1) 0-based
void buildMetadata(TbcMetaData &m, qint32 fields)
{
    m.clear();
    m.setVideoParameters(ntscParameters(fields));
    m.setIsFirstFieldFirst(true);
    for (qint32 i = 0; i < fields; i++) {
        TbcMetaData::Field f;
        f.seqNo = i + 1;
        f.isFirstField = (i % 2 == 0);
        f.syncConf = 100;
        f.fieldPhaseID = (i % 4) + 1;
        f.fileLoc = static_cast<qint64>(i) * kNominal;
        f.diskLoc = static_cast<double>(i);
        f.decodeFaults = 0;
        m.appendField(f);
    }
}

TbcMetaData::Segment segment(qint32 id, qint32 start, qint32 end, const QString &kind, bool enabled,
                             const QString &title = QString(), const QString &comment = QString())
{
    TbcMetaData::Segment s;
    s.id = id;
    s.startField = start;
    s.endFieldExclusive = end;
    s.kind = kind;
    s.source = QStringLiteral("derived");
    s.enabled = enabled;
    s.title = title;
    s.comment = comment;
    s.createdBy = QStringLiteral("testffmetadata");
    return s;
}

void addSegments(TbcMetaData &m)
{
    QVector<TbcMetaData::Segment> segments;
    segments.append(segment(1, 0, 101, QStringLiteral("clip"), true, QStringLiteral("Intro")));
    segments.append(segment(2, 101, 250, QStringLiteral("blank"), false));
    segments.append(segment(3, 250, 400, QStringLiteral("clip"), true, QString(), QStringLiteral("second take")));
    m.setSegments(segments);
}

struct Chapter {
    QString timeBase;
    qint64 start = -1;
    qint64 end = -1;
    QString title;
    QString comment;
};

struct Parsed {
    QString header;
    QStringList topLevel;
    QVector<Chapter> chapters;
};

Parsed parse(const QString &path)
{
    Parsed parsed;
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream stream(&file);
    bool inChapter = false;
    bool first = true;
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (first) {
            parsed.header = line;
            first = false;
            continue;
        }
        if (line.isEmpty()) continue;
        if (line == QLatin1String("[CHAPTER]")) {
            parsed.chapters.append(Chapter());
            inChapter = true;
            continue;
        }
        if (line.startsWith(QLatin1Char(';'))) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        CHECK(eq > 0);
        const QString key = line.left(eq);
        const QString value = line.mid(eq + 1);
        if (!inChapter) {
            parsed.topLevel.append(line);
            continue;
        }
        Chapter &c = parsed.chapters.last();
        if (key == QLatin1String("TIMEBASE")) c.timeBase = value;
        else if (key == QLatin1String("START")) c.start = value.toLongLong();
        else if (key == QLatin1String("END")) c.end = value.toLongLong();
        else if (key == QLatin1String("title")) c.title = value;
        else if (key == QLatin1String("comment")) c.comment = value;
    }
    return parsed;
}

QString tmpPath(const QTemporaryDir &dir, const QString &name)
{
    return dir.filePath(name);
}

void testEnabledSegmentsFullRange(const QTemporaryDir &dir)
{
    TbcMetaData m;
    buildMetadata(m, 400);
    addSegments(m);
    const QString path = tmpPath(dir, QStringLiteral("enabled.txt"));
    CHECK(writeFfmetadata(m, path, -1, -1, true));
    const Parsed p = parse(path);
    CHECK(p.header == QLatin1String(";FFMETADATA1"));
    CHECK(p.chapters.size() == 2);
    CHECK(p.chapters[0].timeBase == QLatin1String("1001/60000"));
    CHECK(p.chapters[0].start == 0 && p.chapters[0].end == 100);
    CHECK(p.chapters[0].title == QLatin1String("Intro"));
    CHECK(p.chapters[0].comment.isEmpty());
    CHECK(p.chapters[1].start == 250 && p.chapters[1].end == 399);
    CHECK(p.chapters[1].title == QLatin1String("Segment 3"));   // untitled: numbered by id
    CHECK(p.chapters[1].comment == QLatin1String("second take"));
}

void testAllSegments(const QTemporaryDir &dir)
{
    TbcMetaData m;
    buildMetadata(m, 400);
    addSegments(m);
    const QString path = tmpPath(dir, QStringLiteral("all.txt"));
    CHECK(writeFfmetadata(m, path, -1, -1, true, FfmetadataSegmentMode::AllSegments));
    const Parsed p = parse(path);
    CHECK(p.chapters.size() == 3);
    CHECK(p.chapters[1].start == 101 && p.chapters[1].end == 249);
    CHECK(p.chapters[1].title == QLatin1String("Segment 2"));
}

void testNoSegmentsAndNoStored(const QTemporaryDir &dir)
{
    TbcMetaData m;
    buildMetadata(m, 400);
    addSegments(m);
    const QString path = tmpPath(dir, QStringLiteral("none.txt"));
    CHECK(writeFfmetadata(m, path, -1, -1, true, FfmetadataSegmentMode::NoSegments));
    CHECK(parse(path).chapters.isEmpty());   // a tape has no navigation chapters

    TbcMetaData bare;
    buildMetadata(bare, 400);
    const QString barePath = tmpPath(dir, QStringLiteral("bare.txt"));
    CHECK(writeFfmetadata(bare, barePath, -1, -1, true));
    CHECK(parse(barePath).chapters.isEmpty());
}

void testRangeRebasing(const QTemporaryDir &dir)
{
    TbcMetaData m;
    buildMetadata(m, 400);
    addSegments(m);
    // Frames 101..150 = fields 200..299; segment 3 starts inside the window
    const QString path = tmpPath(dir, QStringLiteral("range.txt"));
    CHECK(writeFfmetadata(m, path, 101, 50, true));
    const Parsed p = parse(path);
    CHECK(p.chapters.size() == 1);
    CHECK(p.chapters[0].start == 50 && p.chapters[0].end == 99);
    CHECK(p.chapters[0].title == QLatin1String("Segment 3"));

    const QString allPath = tmpPath(dir, QStringLiteral("range-all.txt"));
    CHECK(writeFfmetadata(m, allPath, 101, 50, true, FfmetadataSegmentMode::AllSegments));
    const Parsed a = parse(allPath);
    CHECK(a.chapters.size() == 2);
    CHECK(a.chapters[0].start == 0 && a.chapters[0].end == 49);    // segment 2 clipped to the window
    CHECK(a.chapters[1].start == 50 && a.chapters[1].end == 99);

    // A window entirely inside one segment: one chapter spanning the window
    const QString insidePath = tmpPath(dir, QStringLiteral("inside.txt"));
    CHECK(writeFfmetadata(m, insidePath, 10, 20, true));
    const Parsed i = parse(insidePath);
    CHECK(i.chapters.size() == 1);
    CHECK(i.chapters[0].start == 0 && i.chapters[0].end == 39);

    // Out of range start is refused
    CHECK(!writeFfmetadata(m, tmpPath(dir, QStringLiteral("bad.txt")), 201, 1, true));
}

void testUserMarkerSplice(const QTemporaryDir &dir)
{
    TbcMetaData m;
    buildMetadata(m, 400);
    addSegments(m);
    TbcMetaData::VideoParameters vp = m.getVideoParameters();
    vp.userMarkerSelection = 10;   // frame 10 = fields 18, 19
    vp.userMarkerComment = QStringLiteral("marker; here");
    m.setVideoParameters(vp);
    const QString path = tmpPath(dir, QStringLiteral("marker.txt"));
    CHECK(writeFfmetadata(m, path, -1, -1, true));
    const Parsed p = parse(path);
    CHECK(p.chapters.size() == 4);
    CHECK(p.chapters[0].start == 0 && p.chapters[0].end == 17 && p.chapters[0].title == QLatin1String("Intro"));
    CHECK(p.chapters[1].start == 18 && p.chapters[1].end == 19);
    CHECK(p.chapters[1].title == QLatin1String("marker\\; here"));
    CHECK(p.chapters[2].start == 20 && p.chapters[2].end == 100 && p.chapters[2].title == QLatin1String("Intro"));
    CHECK(p.chapters[3].start == 250 && p.chapters[3].end == 399);
}

void testSegmentsJson(const QTemporaryDir &dir)
{
    TbcMetaData m;
    buildMetadata(m, 400);
    addSegments(m);
    const QString path = tmpPath(dir, QStringLiteral("segments.json"));
    CHECK(writeSegmentsJson(m, path, QStringLiteral("/x/y.tbc.db")));
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const QJsonObject report = QJsonDocument::fromJson(file.readAll()).object();
    CHECK(report.value(QStringLiteral("schemaVersion")).toInt() == 1);
    CHECK(report.value(QStringLiteral("tool")).toObject().value(QStringLiteral("name")).toString()
          == QLatin1String("tbc-export-metadata"));
    CHECK(report.value(QStringLiteral("input")).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("sqlite"));
    CHECK(report.value(QStringLiteral("segmentsSource")).toString() == QLatin1String("stored"));
    CHECK(report.value(QStringLiteral("fieldData")).toObject().value(QStringLiteral("source")).toString() == QLatin1String("none"));
    const QJsonArray segments = report.value(QStringLiteral("segments")).toArray();
    CHECK(segments.size() == 3);
    // Mixed-frame rule: the seam at field 101 (a second field) leaves frame 51 with segment 1
    const QJsonObject s1 = segments.at(0).toObject();
    CHECK(s1.value(QStringLiteral("id")).toInt() == 1);
    CHECK(s1.value(QStringLiteral("startFrame")).toInt() == 1 && s1.value(QStringLiteral("lengthFrames")).toInt() == 51);
    const QJsonObject s2 = segments.at(1).toObject();
    CHECK(s2.value(QStringLiteral("startFrame")).toInt() == 52 && s2.value(QStringLiteral("lengthFrames")).toInt() == 74);
    CHECK(!s2.value(QStringLiteral("enabled")).toBool());
    const QJsonObject s3 = segments.at(2).toObject();
    CHECK(s3.value(QStringLiteral("startFrame")).toInt() == 126 && s3.value(QStringLiteral("lengthFrames")).toInt() == 75);
    CHECK(report.value(QStringLiteral("range")).toObject().value(QStringLiteral("endFieldExclusive")).toInt() == 400);
    CHECK(!report.contains(QStringLiteral("perField")));

    // No stored segments: derived through the library, and the whole file is one clip
    TbcMetaData bare;
    buildMetadata(bare, 400);
    const QString barePath = tmpPath(dir, QStringLiteral("bare.json"));
    CHECK(writeSegmentsJson(bare, barePath, QStringLiteral("/x/y.tbc.json"), 1, 100));
    QFile bareFile(barePath);
    CHECK(bareFile.open(QIODevice::ReadOnly));
    const QJsonObject bareReport = QJsonDocument::fromJson(bareFile.readAll()).object();
    CHECK(bareReport.value(QStringLiteral("segmentsSource")).toString() == QLatin1String("derived"));
    CHECK(bareReport.value(QStringLiteral("input")).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("json"));
    CHECK(bareReport.value(QStringLiteral("segments")).toArray().size() == 1);
    CHECK(bareReport.value(QStringLiteral("range")).toObject().value(QStringLiteral("lengthFrames")).toInt() == 100);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    CHECK(dir.isValid());
    testEnabledSegmentsFullRange(dir);
    testAllSegments(dir);
    testNoSegmentsAndNoStored(dir);
    testRangeRebasing(dir);
    testUserMarkerSplice(dir);
    testSegmentsJson(dir);
    std::cout << "testffmetadata: all checks passed\n";
    return 0;
}
