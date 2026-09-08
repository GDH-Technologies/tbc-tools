/************************************************************************

    processingpool.cpp

    tbc-segments - Threaded walk over the raw TBC fields
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

#include "processingpool.h"

#include <QDebug>
#include <QMutexLocker>
#include <QVector>
#include <cmath>
#include <limits>

#include "tbc/logging.h"

FieldWalkPool::FieldWalkPool(const QString &_lumaFilename, const QString &_chromaFilename,
                             qint32 _maxThreads, const TbcMetaData &_metaData, const FieldGeometry &_geometry)
    : lumaFilename(_lumaFilename), chromaFilename(_chromaFilename), maxThreads(qMax<qint32>(1, _maxThreads)),
      metaData(_metaData), geometry(_geometry)
{
}

bool FieldWalkPool::process(FieldMetrics &out)
{
    const qint32 fieldLength = geometry.fieldWidth * geometry.fieldHeight;
    if (!geometry.valid() || fieldLength <= 0) {
        qCritical() << "tbc-segments: metadata geometry is not usable for a field walk";
        return false;
    }
    if (!lumaVideo.open(lumaFilename, fieldLength, geometry.fieldWidth)) {
        qCritical() << "tbc-segments: could not open TBC" << lumaFilename;
        return false;
    }
    lastFieldNumber = metaData.getNumberOfFields();
    const qint32 available = lumaVideo.getNumberOfAvailableFields();
    if (available < lastFieldNumber) {
        qCritical().nospace() << "tbc-segments: TBC holds only " << available
                              << " fields but the metadata describes " << lastFieldNumber
                              << "; refusing to guess the alignment";
        lumaVideo.close();
        return false;
    }
    if (available > lastFieldNumber) {
        // The decoders flush their metadata in batches, so a TBC that was cut
        // short (or is still being written) carries a tail the metadata never
        // describes. Field i of the TBC is still field i of the metadata:
        // walk the described fields and leave the surplus alone.
        qInfo().nospace() << "tbc-segments: TBC holds " << available << " fields, the metadata describes "
                          << lastFieldNumber << "; walking the first " << lastFieldNumber;
    }
    if (!chromaFilename.isEmpty()) {
        if (chromaVideo.open(chromaFilename, fieldLength, geometry.fieldWidth)
            && chromaVideo.getNumberOfAvailableFields() >= lastFieldNumber) {
            chromaOpen = true;
        } else {
            qWarning() << "tbc-segments: chroma TBC unusable; burst amplitude comes from the luma TBC";
            if (chromaVideo.isSourceValid()) chromaVideo.close();
        }
    }

    out.resize(lastFieldNumber);
    out.enabled = true;
    // Not chromaOpen: a composite decode (CVBS, LaserDisc) has no chroma
    // sibling and measures burst from the luma TBC quite legitimately.
    out.hasBurst = !geometry.skipBurst;
    output = &out;
    inputFieldNumber = 1;
    storedFields = 0;
    nextReportAt = qMax<qint32>(1, lastFieldNumber / 20);
    totalTimer.start();
    qInfo().nospace() << "tbc-segments: walking " << lastFieldNumber << " fields with " << maxThreads
                      << " thread(s)" << (chromaOpen ? " (luma + chroma)" : " (luma only)");

    QVector<QThread *> threads;
    threads.resize(maxThreads);
    for (qint32 i = 0; i < maxThreads; i++) {
        threads[i] = new FieldWalker(*this, geometry);
        threads[i]->start(QThread::LowPriority);
    }
    for (qint32 i = 0; i < maxThreads; i++) {
        threads[i]->wait();
        delete threads[i];
    }
    lumaVideo.close();
    if (chromaOpen) chromaVideo.close();
    output = nullptr;
    if (aborted()) return false;

    const double secs = totalTimer.elapsed() / 1000.0;
    qInfo().nospace() << "tbc-segments: field walk complete, " << lastFieldNumber << " fields in "
                      << QString::number(secs, 'f', 1) << " s (" << QString::number(secs > 0 ? lastFieldNumber / secs : 0.0, 'f', 0) << " fields/s)";
    return true;
}

bool FieldWalkPool::nextField(qint32 &fieldNumber, SourceVideo::Data &luma, SourceVideo::Data &chroma,
                              SourceVideo::Data &previousSameParity, bool &haveChroma, bool &havePrevious)
{
    QMutexLocker locker(&inputMutex);
    if (inputFieldNumber > lastFieldNumber || aborted()) return false;
    fieldNumber = inputFieldNumber++;
    luma = lumaVideo.getVideoField(fieldNumber);
    haveChroma = chromaOpen;
    if (chromaOpen) chroma = chromaVideo.getVideoField(fieldNumber);
    havePrevious = fieldNumber > 2;
    if (havePrevious) previousSameParity = lumaVideo.getVideoField(fieldNumber - 2);
    return true;
}

void FieldWalkPool::store(qint32 fieldNumber, const FieldSample &sample, double fieldDiffIre)
{
    QMutexLocker locker(&outputMutex);
    if (!output) return;
    const qint32 i = fieldNumber - 1;
    if (i < 0 || i >= output->lumaMeanIre.size()) return;
    output->lumaMeanIre[i] = sample.lumaMeanIre;
    output->blankingDevIre[i] = sample.blankingDevIre;
    output->syncTipDevIre[i] = sample.syncTipDevIre;
    output->noiseIre[i] = sample.noiseIre;
    output->burstAmpIre[i] = sample.burstAmpIre;
    output->fieldDiffIre[i] = fieldDiffIre;
    storedFields++;
    if (storedFields >= nextReportAt) {
        qInfo().nospace() << "tbc-segments: " << storedFields << "/" << lastFieldNumber << " fields ("
                          << (100 * storedFields / qMax<qint32>(1, lastFieldNumber)) << "%)";
        nextReportAt += qMax<qint32>(1, lastFieldNumber / 20);
    }
}

FieldWalker::FieldWalker(FieldWalkPool &_pool, const FieldGeometry &_geometry, QObject *parent)
    : QThread(parent), pool(_pool), geometry(_geometry)
{
}

void FieldWalker::run()
{
    qint32 fieldNumber = 0;
    SourceVideo::Data luma, chroma, previous;
    bool haveChroma = false, havePrevious = false;
    while (!pool.aborted()) {
        if (!pool.nextField(fieldNumber, luma, chroma, previous, haveChroma, havePrevious)) break;
        const FieldSample sample = measureField(luma, haveChroma ? &chroma : nullptr, geometry);
        const double diff = havePrevious ? sameParityDifferenceIre(luma, previous, geometry)
                                         : std::numeric_limits<double>::quiet_NaN();
        pool.store(fieldNumber, sample, diff);
    }
}
