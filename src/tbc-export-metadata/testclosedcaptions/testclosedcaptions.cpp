/************************************************************************

    testclosedcaptions.cpp

    Unit tests for the Scenarist SCC closed caption exporter
    Copyright (C) 2026 Reece Dodge

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

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

// Tests must assert even in Release builds, where NDEBUG would otherwise
// compile every assert() away
#undef NDEBUG
#include <cassert>
#include <iostream>

#include "closedcaptions.h"

using std::cerr;

namespace {

// Build NTSC metadata with the given number of fields, alternating first and
// second fields as a real capture does.
void buildMetaData(TbcMetaData &metaData, qint32 numberOfFields)
{
    TbcMetaData::VideoParameters videoParameters;
    videoParameters.system = NTSC;
    videoParameters.fieldWidth = 910;
    videoParameters.fieldHeight = 263;
    metaData.setVideoParameters(videoParameters);

    for (qint32 i = 0; i < numberOfFields; i++) {
        TbcMetaData::Field field;
        field.isFirstField = ((i % 2) == 0);
        metaData.appendField(field);
    }
}

// Attach closed caption data to one field (1-based, as the exporter counts)
void setCaption(TbcMetaData &metaData, qint32 fieldIndex, qint32 data0, qint32 data1)
{
    TbcMetaData::ClosedCaption closedCaption;
    closedCaption.inUse = true;
    closedCaption.data0 = data0;
    closedCaption.data1 = data1;
    metaData.updateFieldClosedCaption(closedCaption, fieldIndex);
}

// Export to a temporary file and return the lines that carry caption data
QStringList exportDataLines(TbcMetaData &metaData, const QTemporaryDir &dir)
{
    const QString fileName = dir.filePath(QStringLiteral("captions.scc"));
    const bool written = writeClosedCaptions(metaData, fileName);
    assert(written);

    QFile file(fileName);
    const bool opened = file.open(QFile::ReadOnly | QFile::Text);
    assert(opened);
    QTextStream stream(&file);

    QStringList dataLines;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QStringLiteral("Scenarist_SCC"))) continue;
        dataLines.append(line);
    }
    return dataLines;
}

} // namespace

// SCC V1.0 carries the bytes as transmitted, which is with odd parity in bit
// 7. Control code 0x14 0x2C (EDM) goes out as 942c.
void testWritesBytesWithOddParity()
{
    cerr << "Testing writeClosedCaptions writes bytes with odd parity\n";

    QTemporaryDir dir;
    TbcMetaData metaData;
    buildMetaData(metaData, 8);
    setCaption(metaData, 1, 0x14, 0x2C);

    const QStringList lines = exportDataLines(metaData, dir);

    assert(lines.size() == 1);
    assert(lines[0].contains(QStringLiteral("942c")));
}

// Text bytes take parity too: 'W' (0x57) already has odd parity, 'H' (0x48)
// needs the parity bit set, giving 57c8.
void testWritesTextBytesWithOddParity()
{
    cerr << "Testing writeClosedCaptions applies parity to text bytes\n";

    QTemporaryDir dir;
    TbcMetaData metaData;
    buildMetaData(metaData, 8);
    setCaption(metaData, 1, 0x57, 0x48);

    const QStringList lines = exportDataLines(metaData, dir);

    assert(lines.size() == 1);
    assert(lines[0].contains(QStringLiteral("57c8")));
}

// SCC is a field 1 (CC1) format. Data recovered from second fields is a
// different caption service and must not be spliced into the byte stream.
void testIgnoresSecondFieldData()
{
    cerr << "Testing writeClosedCaptions ignores second field data\n";

    QTemporaryDir dir;
    TbcMetaData metaData;
    buildMetaData(metaData, 8);
    setCaption(metaData, 2, 0x14, 0x2C);
    setCaption(metaData, 4, 0x57, 0x48);

    const QStringList lines = exportDataLines(metaData, dir);

    assert(lines.isEmpty());
}

// Timecodes are non-drop (:ff), so they count at 30 frames per second.
// Frame 1800 is exactly one minute.
void testWritesNonDropTimeStamps()
{
    cerr << "Testing writeClosedCaptions writes 30 fps non-drop timecodes\n";

    QTemporaryDir dir;
    TbcMetaData metaData;
    buildMetaData(metaData, 3602);
    setCaption(metaData, 3601, 0x14, 0x2C);

    const QStringList lines = exportDataLines(metaData, dir);

    assert(lines.size() == 1);
    assert(lines[0].startsWith(QStringLiteral("00:01:00:00\t")));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testWritesBytesWithOddParity();
    testWritesTextBytesWithOddParity();
    testIgnoresSecondFieldData();
    testWritesNonDropTimeStamps();

    cerr << "All closed caption export tests passed\n";
    return 0;
}
