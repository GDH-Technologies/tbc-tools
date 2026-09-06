/************************************************************************

    testclosedcaption.cpp

    Unit tests for ClosedCaption (EIA/CEA-608 line 21 decoder)
    Copyright (C) 2026 Reece Dodge

    This file is part of tbc-tools.

    ld-process-vbi is free software: you can redistribute it and/or
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

// Tests must assert even in Release builds, where NDEBUG would otherwise
// compile every assert() away
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <iostream>

#include "closedcaption.h"

using std::cerr;

namespace {

// Sample levels taken from a real NTSC VHS decode (14.318 MHz, 910 samples
// per line), so the line normalisation the decoder performs matches what it
// sees on real captures.
constexpr qint32 fieldWidth = 910;
constexpr qint32 fieldHeight = 263;
constexpr qint32 colourBurstEnd = 110;

constexpr double syncTipLevel = 2400.0;     // ~-32 IRE
constexpr double blankingLevel = 15360.0;   //    0 IRE
constexpr double logicOneLevel = 35398.0;   //  +50 IRE, CEA-608 mark level

constexpr double samplesPerBit = static_cast<double>(fieldWidth) / 32.0;
constexpr qint32 syncEnd = 67;              // ~4.7 us sync pulse
constexpr qint32 runInStart = 132;          // 7 cycles of run-in ...
constexpr qint32 startBitSample = 331;      // ... ending at the start bits

TbcMetaData::VideoParameters ntscParameters()
{
    TbcMetaData::VideoParameters videoParameters;
    videoParameters.system = NTSC;
    videoParameters.fieldWidth = fieldWidth;
    videoParameters.fieldHeight = fieldHeight;
    videoParameters.colourBurstStart = 74;
    videoParameters.colourBurstEnd = colourBurstEnd;
    videoParameters.activeVideoStart = 134;
    videoParameters.activeVideoEnd = 894;
    videoParameters.blanking16bIre = static_cast<qint32>(blankingLevel);
    videoParameters.black16bIre = 16243;
    videoParameters.white16bIre = 56320;
    videoParameters.sampleRate = 14318181.818181818;
    return videoParameters;
}

// Deterministic low-level noise, so the tests are reproducible but the line
// still has the dynamic range a real capture would have
double noiseAt(qint32 sample, quint32 seed)
{
    quint32 state = (static_cast<quint32>(sample) * 1664525u) + 1013904223u + seed;
    state ^= state >> 15;
    state *= 2246822519u;
    state ^= state >> 13;
    return static_cast<double>(state % 2000u) - 1000.0;
}

// Sync and blanking, plus a little noise
SourceVideo::Data blankLine(quint32 seed)
{
    SourceVideo::Data line(fieldWidth);
    for (qint32 i = 0; i < fieldWidth; i++) {
        const double base = (i < syncEnd) ? syncTipLevel : blankingLevel;
        line[i] = static_cast<quint16>(qBound(0.0, base + noiseAt(i, seed), 65535.0));
    }
    return line;
}

// Odd parity, as transmitted on line 21 [CTA-608 p14]
bool oddParityBit(qint32 value)
{
    qint32 ones = 0;
    for (qint32 bit = 0; bit < 7; bit++) {
        if ((value >> bit) & 1) ones++;
    }
    return (ones % 2) == 0;
}

// Build a complete line 21 waveform carrying the given two bytes: 7 cycles of
// run-in at 32 x fH, start bits 001, then 2 x (7 data bits LSB first + odd
// parity bit).
SourceVideo::Data captionLine(qint32 data0, qint32 data1, quint32 seed = 1)
{
    SourceVideo::Data line = blankLine(seed);

    // Run-in: sine at the bit clock rate swinging 0 -> 50 IRE, so its mean
    // sits at 25 IRE, the slicing threshold for the data that follows
    const double amplitude = (logicOneLevel - blankingLevel) / 2.0;
    for (qint32 i = runInStart; i < startBitSample; i++) {
        const double phase = 2.0 * M_PI * static_cast<double>(i - runInStart) / samplesPerBit;
        const double value = blankingLevel + (amplitude * (1.0 - std::cos(phase)));
        line[i] = static_cast<quint16>(qBound(0.0, value + noiseAt(i, seed), 65535.0));
    }

    QVector<bool> bits;
    bits << false << false << true;
    for (qint32 value : { data0, data1 }) {
        for (qint32 bit = 0; bit < 7; bit++) {
            bits << (((value >> bit) & 1) != 0);
        }
        bits << oddParityBit(value);
    }

    for (qint32 bitIndex = 0; bitIndex < bits.size(); bitIndex++) {
        const qint32 start = startBitSample + static_cast<qint32>(std::lround(bitIndex * samplesPerBit));
        const qint32 end = startBitSample + static_cast<qint32>(std::lround((bitIndex + 1) * samplesPerBit));
        const double level = bits[bitIndex] ? logicOneLevel : blankingLevel;
        for (qint32 i = start; i < end && i < fieldWidth; i++) {
            line[i] = static_cast<quint16>(qBound(0.0, level + noiseAt(i, seed), 65535.0));
        }
    }

    return line;
}

// Read one field line out of a TBC capture file
SourceVideo::Data readFieldLine(const QString &fileName, qint32 fieldNumber, qint32 fieldLine)
{
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly)) {
        cerr << "Could not open test data file " << qPrintable(fileName) << "\n";
        return SourceVideo::Data();
    }

    const qint64 fieldBytes = static_cast<qint64>(fieldWidth) * fieldHeight * 2;
    const qint64 offset = (fieldBytes * fieldNumber)
        + (static_cast<qint64>(fieldLine - 1) * fieldWidth * 2);
    if (!file.seek(offset)) return SourceVideo::Data();

    const QByteArray raw = file.read(static_cast<qint64>(fieldWidth) * 2);
    if (raw.size() != fieldWidth * 2) return SourceVideo::Data();

    SourceVideo::Data line(fieldWidth);
    const uchar *bytes = reinterpret_cast<const uchar *>(raw.constData());
    for (qint32 i = 0; i < fieldWidth; i++) {
        line[i] = static_cast<quint16>(bytes[i * 2] | (bytes[(i * 2) + 1] << 8));
    }
    return line;
}

} // namespace

// A synthesised line 21 carrying real CEA-608 data must decode to those bytes.
// This guards the run-in gate: it must not reject genuine captions.
void testDecodesValidCaptionLine()
{
    cerr << "Testing ClosedCaption::decodeLine accepts a valid line 21\n";

    ClosedCaption closedCaption;
    TbcMetaData::Field fieldMetadata;

    // 0x14 0x2C is EDM (Erase Displayed Memory) on CC1
    const bool decoded = closedCaption.decodeLine(captionLine(0x14, 0x2C),
                                                 ntscParameters(), fieldMetadata);

    assert(decoded);
    assert(fieldMetadata.closedCaption.inUse);
    assert(fieldMetadata.closedCaption.data0 == 0x14);
    assert(fieldMetadata.closedCaption.data1 == 0x2C);
}

// Text bytes as well as control codes
void testDecodesTextBytes()
{
    cerr << "Testing ClosedCaption::decodeLine recovers text bytes\n";

    ClosedCaption closedCaption;
    TbcMetaData::Field fieldMetadata;

    const bool decoded = closedCaption.decodeLine(captionLine('W', 'H'),
                                                 ntscParameters(), fieldMetadata);

    assert(decoded);
    assert(fieldMetadata.closedCaption.data0 == 'W');
    assert(fieldMetadata.closedCaption.data1 == 'H');
}

// Neither NTSC capture in test-data carries line 21 closed captions, so every
// field of them must decode to "no caption". The decoder must not fit its best
// guess to picture content and report that as caption data.
void testRejectsCaptureWithoutCaptions(const QString &name, qint32 numberOfFields)
{
    cerr << "Testing ClosedCaption::decodeLine rejects " << qPrintable(name)
         << " (capture has no line 21 data)\n";

    const QString fileName = QStringLiteral(TESTDATA_NTSC_DIR "/") + name;

    ClosedCaption closedCaption;
    qint32 falsePositives = 0;

    for (qint32 fieldNumber = 0; fieldNumber < numberOfFields; fieldNumber++) {
        const SourceVideo::Data line = readFieldLine(fileName, fieldNumber, 21);
        assert(!line.isEmpty());

        TbcMetaData::Field fieldMetadata;
        if (closedCaption.decodeLine(line, ntscParameters(), fieldMetadata)) {
            falsePositives++;
            cerr << "  field " << fieldNumber << " wrongly decoded as CC: "
                 << fieldMetadata.closedCaption.data0 << ", "
                 << fieldMetadata.closedCaption.data1 << "\n";
        }
    }

    cerr << "  " << falsePositives << " false positives out of "
         << numberOfFields << " fields\n";
    assert(falsePositives == 0);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testDecodesValidCaptionLine();
    testDecodesTextBytes();
    testRejectsCaptureWithoutCaptions(QStringLiteral("ve-snw-cut.tbc"), 58);
    testRejectsCaptureWithoutCaptions(QStringLiteral("issue176.tbc"), 8);

    cerr << "All ClosedCaption tests passed\n";
    return 0;
}
