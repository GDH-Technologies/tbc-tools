/************************************************************************

    closedcaptions.cpp

    tbc-export-metadata - Export ld-decode metadata into other formats
    Copyright (C) 2019-2020 Adam Sampson
    Copyright (C) 2021 Simon Inns

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

#include "closedcaptions.h"

#include "vbidecoder.h"

#include <QtGlobal>
#include <QFile>
#include <QTextStream>
#include <set>
#include <vector>
#include "tbc/logging.h"

using std::set;
using std::vector;

// Generate an SCC format timestamp based on the field index
QString generateTimeStamp(qint32 fieldIndex, VideoSystem system)
{
    // Convert to a 0-based count of frames
    const qint32 frameIndex = (fieldIndex - 1) / 2;

    // We are generating non-drop timecode (:ff not ;ff), which counts whole
    // frames -- 30 per second for 525-line systems, 25 for 625-line. (NTSC
    // plays those 30 frames back over 30/1.001 seconds, which is exactly the
    // drift that drop-frame timecode exists to correct; a non-drop timestamp
    // must not try to correct it itself, or it stops matching frame numbers.)
    const qint32 framesPerSecond = (system == PAL || system == SECAM || system == MESECAM) ? 25 : 30;
    const qint32 framesPerMinute = framesPerSecond * 60;
    const qint32 framesPerHour = framesPerMinute * 60;

    // Since the subtitle is relative to the video we can simply calculate the
    // timecode from the sequential field number (which should work even if the
    // input is a snippet from a LaserDisc sample)
    const qint32 hh = frameIndex / framesPerHour;
    const qint32 mm = (frameIndex % framesPerHour) / framesPerMinute;
    const qint32 ss = (frameIndex % framesPerMinute) / framesPerSecond;
    const qint32 ff = frameIndex % framesPerSecond;

    // Create the timestamp
    return QString("%1:%2:%3:%4").arg(hh, 2, 10, QLatin1Char('0'))
                                 .arg(mm, 2, 10, QLatin1Char('0'))
                                 .arg(ss, 2, 10, QLatin1Char('0'))
                                 .arg(ff, 2, 10, QLatin1Char('0'));
}

// Add the odd parity bit that line 21 bytes are transmitted with, which is
// what an SCC file records [CTA-608 p14]
qint32 addOddParity(qint32 dataByte)
{
    const qint32 value = dataByte & 0x7F;

    qint32 ones = 0;
    for (qint32 bit = 0; bit < 7; bit++) {
        if ((value >> bit) & 1) ones++;
    }

    return ((ones % 2) == 0) ? (value | 0x80) : value;
}

// Sanity check the CC data byte and set to -1 if it probably is invalid
qint32 sanityCheckData(qint32 dataByte)
{
    // Already marked as invalid?
    if (dataByte == -1) return -1;

    // Is it in the valid command byte range?
    if (dataByte >= 0x10 && dataByte <= 0x1F) {
        // Valid command byte
        return dataByte;
    }

    // Valid 7-bit ASCII range?
    if (dataByte >= 0x20 && dataByte <= 0x7E) {
        // Valid character byte
        return dataByte;
    }

    // Invalid byte
    return 0;
}

// Extract any available CC data and output it in Scenarist Closed Caption format (SCC) V1.0
// Protocol description:  http://www.theneitherworld.com/mcpoodle/SCC_TOOLS/DOCS/SCC_FORMAT.HTML
bool writeClosedCaptions(TbcMetaData &metaData, const QString &fileName)
{
    const auto videoParameters = metaData.getVideoParameters();

    // Open the output file
    QFile file(fileName);
    if (!file.open(QFile::WriteOnly | QFile::Text)) {
        tbcDebug(QStringLiteral("writeClosedCaptions: Could not open file for output"));
        return false;
    }
    QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#endif

    // Output the SCC V1.0 header
    stream << "Scenarist_SCC V1.0";

    // Extract the closed captions data and stream to the text file
    bool captionInProgress = false;
    QString debugCaption;
    for (qint32 fieldIndex = 1; fieldIndex <= videoParameters.numberOfSequentialFields; fieldIndex++) {
        // SCC V1.0 records the line 21 data of field 1 only. Anything
        // recovered from field 2 is a different caption service (CC3/CC4 or
        // XDS), and splicing it into this byte stream would corrupt it.
        if (!metaData.getField(fieldIndex).isFirstField) continue;

        // Get the CC data bytes from the field. A field with no caption data,
        // or whose bytes failed their parity check, contributes nothing.
        const TbcMetaData::ClosedCaption &closedCaption = metaData.getFieldClosedCaption(fieldIndex);
        qint32 data0 = 0;
        qint32 data1 = 0;
        if (closedCaption.inUse) {
            data0 = sanityCheckData(closedCaption.data0);
            data1 = sanityCheckData(closedCaption.data1);
            if (data0 < 0 || data1 < 0) {
                data0 = 0;
                data1 = 0;
            }
        }

        if (data0 > 0 || data1 > 0) {
            if (!captionInProgress) {
                // Start of new caption

                // Output a timecode followed by a tab character (in SCC format)
                QString timeStamp = generateTimeStamp(fieldIndex, videoParameters.system);
                stream << "\n\n" << timeStamp << "\t";
                debugCaption = "writeClosedCaptions(): Caption data at " + timeStamp + " : [";

                // Set the caption in progress flag
                captionInProgress = true;
            }

            // Output the 2 bytes of data as 2 hexadecimal values, as
            // transmitted -- that is, with their odd parity bits. So 0x14 and
            // 0x2C are written as 942c, followed by a space.
            stream << QString("%1").arg(addOddParity(data0), 2, 16, QLatin1Char('0'));
            stream << QString("%1").arg(addOddParity(data1), 2, 16, QLatin1Char('0'));
            stream << " ";

            // Add the 2 bytes of the data output to the debug caption too
            if (data0 >= 0x10 && data0 <= 0x1F) {
                // This is a command byte, so output a space
                debugCaption = debugCaption + " ";
            } else {
                // Normal text - display

                // Create a string from the two characters
                char string[3];
                string[0] = static_cast<char>(data0);
                string[1] = static_cast<char>(data1);
                string[2] = static_cast<char>(0);

                // Add it to the debug output
                debugCaption = debugCaption + QString::fromLocal8Bit(string);
            }
        } else {
            // No CC data for this frame
            if (captionInProgress) {
                // End of current caption
                debugCaption = debugCaption + "]";
                tbcDebugStream() << debugCaption;
            }
            captionInProgress = false;
        }
    }

    // Add some trailing white space
    stream << "\n\n";

    // Done!
    file.close();
    return true;
}
