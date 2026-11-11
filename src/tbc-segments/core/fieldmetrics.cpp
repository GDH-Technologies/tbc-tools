/************************************************************************

    fieldmetrics.cpp

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

#include "fieldmetrics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Region {
    qint32 x0, x1;  // samples, half-open
};

// Mean and standard deviation of a region over the active lines (subsampled).
void regionStats(const SourceVideo::Data &field, const FieldGeometry &g, Region r, double *mean, double *stddev, qint64 *count)
{
    double sum = 0.0, sumSq = 0.0;
    qint64 n = 0;
    const qint32 x0 = std::max(0, r.x0);
    const qint32 x1 = std::min(g.fieldWidth, r.x1);
    for (qint32 line = g.firstActiveFieldLine; line < g.lastActiveFieldLine; line += g.lineStep) {
        const qint64 base = static_cast<qint64>(line - 1) * g.fieldWidth;
        if (base + x1 > field.size()) break;
        for (qint32 x = x0; x < x1; x += g.sampleStep) {
            const double v = field[static_cast<int>(base + x)];
            sum += v;
            sumSq += v * v;
            n++;
        }
    }
    if (count) *count = n;
    if (n == 0) {
        if (mean) *mean = kNaN;
        if (stddev) *stddev = kNaN;
        return;
    }
    const double m = sum / n;
    if (mean) *mean = m;
    if (stddev) *stddev = std::sqrt(std::max(0.0, sumSq / n - m * m));
}

// Mean per-line peak-to-peak of a region (the burst amplitude), in raw units.
double regionPeakToPeak(const SourceVideo::Data &field, const FieldGeometry &g, Region r)
{
    double total = 0.0;
    qint32 lines = 0;
    const qint32 x0 = std::max(0, r.x0);
    const qint32 x1 = std::min(g.fieldWidth, r.x1);
    if (x1 - x0 < 4) return kNaN;
    for (qint32 line = g.firstActiveFieldLine; line < g.lastActiveFieldLine; line += g.lineStep) {
        const qint64 base = static_cast<qint64>(line - 1) * g.fieldWidth;
        if (base + x1 > field.size()) break;
        quint16 lo = 65535, hi = 0;
        for (qint32 x = x0; x < x1; x++) {  // every sample: the burst is only a few cycles wide
            const quint16 v = field[static_cast<int>(base + x)];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        total += static_cast<double>(hi) - static_cast<double>(lo);
        lines++;
    }
    return lines ? total / lines : kNaN;
}

} // namespace

bool FieldGeometry::valid() const
{
    return fieldWidth > 0 && fieldHeight > 0 && activeVideoEnd > activeVideoStart
           && lastActiveFieldLine > firstActiveFieldLine && white16bIre > black16bIre;
}

FieldGeometry geometryFromParameters(const TbcMetaData::VideoParameters &vp)
{
    FieldGeometry g;
    g.fieldWidth = vp.fieldWidth;
    g.fieldHeight = vp.fieldHeight;
    g.activeVideoStart = vp.activeVideoStart;
    g.activeVideoEnd = vp.activeVideoEnd;
    g.firstActiveFieldLine = std::max(1, vp.firstActiveFieldLine);
    g.lastActiveFieldLine = std::min(vp.fieldHeight + 1, std::max(g.firstActiveFieldLine, vp.lastActiveFieldLine));
    g.colourBurstStart = vp.colourBurstStart;
    g.colourBurstEnd = vp.colourBurstEnd;
    g.black16bIre = vp.black16bIre;
    g.white16bIre = vp.white16bIre;
    g.blanking16bIre = vp.blanking16bIre >= 0 ? vp.blanking16bIre : vp.black16bIre;
    g.syncTipIre = (vp.system == PAL || vp.system == SECAM || vp.system == MESECAM) ? -300.0 / 7.0 : -40.0;
    return g;
}

FieldSample measureField(const SourceVideo::Data &luma, const SourceVideo::Data *chroma, const FieldGeometry &g)
{
    FieldSample s;
    s.lumaMeanIre = s.blankingDevIre = s.syncTipDevIre = s.noiseIre = s.burstAmpIre = kNaN;
    if (!g.valid() || luma.size() < g.fieldWidth * g.fieldHeight) return s;

    const double scale = g.ireScale();
    double mean = kNaN, dev = kNaN;

    // Active picture.
    regionStats(luma, g, {g.activeVideoStart, g.activeVideoEnd}, &mean, nullptr, nullptr);
    if (std::isfinite(mean)) s.lumaMeanIre = g.toIre(mean);

    // Back porch: between the end of the burst and the start of active video.
    // Flat on real video, so its deviation is the noise floor and its level the
    // blanking error.
    const qint32 porchStart = std::max(0, g.colourBurstEnd + 2);
    const qint32 porchEnd = g.activeVideoStart - 2;
    if (porchEnd - porchStart >= 4) {
        regionStats(luma, g, {porchStart, porchEnd}, &mean, &dev, nullptr);
        if (std::isfinite(mean)) s.blankingDevIre = (mean - g.blanking16bIre) / scale;
        if (std::isfinite(dev)) s.noiseIre = dev / scale;
    }

    // Sync tip: the middle of the stretch before the burst. Also the noise
    // fallback when the back porch is too narrow to measure.
    const qint32 preBurst = std::max(0, g.colourBurstStart);
    if (preBurst >= 6) {
        regionStats(luma, g, {preBurst / 6, preBurst / 2}, &mean, &dev, nullptr);
        if (std::isfinite(mean)) s.syncTipDevIre = (mean - g.blanking16bIre) / scale - g.syncTipIre;
        if (!std::isfinite(s.noiseIre) && std::isfinite(dev)) s.noiseIre = dev / scale;
    }

    // Burst amplitude, from the chroma field when there is one. Skipped
    // entirely under --no-burst: with no chroma open this would otherwise fall
    // back to the luma TBC, which for a colour-under decode (S-Video, Video8,
    // VHS) carries no burst and would store a meaningless number.
    if (!g.skipBurst && g.colourBurstEnd - g.colourBurstStart >= 4) {
        const SourceVideo::Data &burstSource = (chroma && chroma->size() >= g.fieldWidth * g.fieldHeight) ? *chroma : luma;
        const double pp = regionPeakToPeak(burstSource, g, {g.colourBurstStart, g.colourBurstEnd});
        if (std::isfinite(pp)) s.burstAmpIre = pp / scale;
    }
    return s;
}

double sameParityDifferenceIre(const SourceVideo::Data &field, const SourceVideo::Data &previousSameParity, const FieldGeometry &g)
{
    if (!g.valid()) return kNaN;
    const qint64 needed = static_cast<qint64>(g.fieldWidth) * g.fieldHeight;
    if (field.size() < needed || previousSameParity.size() < needed) return kNaN;
    double sum = 0.0;
    qint64 n = 0;
    const qint32 x0 = std::max(0, g.activeVideoStart);
    const qint32 x1 = std::min(g.fieldWidth, g.activeVideoEnd);
    for (qint32 line = g.firstActiveFieldLine; line < g.lastActiveFieldLine; line += g.lineStep) {
        const qint64 base = static_cast<qint64>(line - 1) * g.fieldWidth;
        for (qint32 x = x0; x < x1; x += g.sampleStep) {
            const int idx = static_cast<int>(base + x);
            sum += std::abs(static_cast<double>(field[idx]) - static_cast<double>(previousSameParity[idx]));
            n++;
        }
    }
    return n ? (sum / n) / g.ireScale() : kNaN;
}
