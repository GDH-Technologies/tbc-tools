/************************************************************************

    fieldmetrics.h

    tbc-segments - Per-field metrics from raw TBC field samples
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

#ifndef FIELDMETRICS_H
#define FIELDMETRICS_H

#include <QtGlobal>

#include "sourcevideo.h"
#include "tbcmetadata.h"

// The geometry a field measurement needs, lifted from VideoParameters once.
struct FieldGeometry {
    qint32 fieldWidth = 0;
    qint32 fieldHeight = 0;
    qint32 activeVideoStart = 0;      // samples, half-open [start, end)
    qint32 activeVideoEnd = 0;
    qint32 firstActiveFieldLine = 1;  // 1-based field lines, half-open [first, last)
    qint32 lastActiveFieldLine = 1;
    qint32 colourBurstStart = 0;      // samples
    qint32 colourBurstEnd = 0;
    double black16bIre = 0.0;
    double white16bIre = 65535.0;
    double blanking16bIre = 0.0;
    double syncTipIre = -40.0;        // nominal sync tip level for the system

    // Sampling stride used by every measurement (speed over precision).
    qint32 sampleStep = 4;
    qint32 lineStep = 2;

    // Do not measure burst amplitude (--no-burst). Set when the caller would
    // rather not read the chroma TBC at all: burst is the only thing it is
    // needed for, and reading it doubles the walk's I/O to sample the few
    // samples of each line the burst occupies.
    bool skipBurst = false;

    double ireScale() const { return (white16bIre - black16bIre) / 100.0; }
    double toIre(double sample16) const { return (sample16 - black16bIre) / ireScale(); }
    bool valid() const;
};

FieldGeometry geometryFromParameters(const TbcMetaData::VideoParameters &vp);

// One field's measurements (NaN when the region cannot be measured).
struct FieldSample {
    double lumaMeanIre = 0.0;
    double blankingDevIre = 0.0;
    double syncTipDevIre = 0.0;
    double noiseIre = 0.0;
    double burstAmpIre = 0.0;
};

// Measure a field. *chroma* (same geometry) supplies the burst when the luma
// TBC is an S-Video luma-only stream; nullptr measures burst from *luma*.
FieldSample measureField(const SourceVideo::Data &luma, const SourceVideo::Data *chroma, const FieldGeometry &g);

// Mean absolute difference (IRE) over the active area between two fields of
// the same parity (n and n-2), subsampled by the geometry's strides.
double sameParityDifferenceIre(const SourceVideo::Data &field, const SourceVideo::Data &previousSameParity, const FieldGeometry &g);

#endif // FIELDMETRICS_H
