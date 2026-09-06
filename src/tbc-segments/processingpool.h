/************************************************************************

    processingpool.h

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

#ifndef PROCESSINGPOOL_H
#define PROCESSINGPOOL_H

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QThread>

#include "fieldmetrics.h"
#include "segments.h"
#include "sourcevideo.h"
#include "tbcmetadata.h"

// Reads every field once (file access serialised, measurement parallel) and
// fills a FieldMetrics. Same shape as ld-process-vits' ProcessingPool.
class FieldWalkPool
{
public:
    FieldWalkPool(const QString &lumaFilename, const QString &chromaFilename,
                  qint32 maxThreads, const TbcMetaData &metaData, const FieldGeometry &geometry);

    // Opens the TBC(s), checks the field count against the metadata, runs the
    // workers and fills *out*. False on any failure (message already logged).
    bool process(FieldMetrics &out);

    // -- worker API --
    // Next 1-based field to measure, with its samples and the same-parity
    // predecessor (n-2) when one exists. False when the input is exhausted.
    bool nextField(qint32 &fieldNumber, SourceVideo::Data &luma, SourceVideo::Data &chroma,
                   SourceVideo::Data &previousSameParity, bool &haveChroma, bool &havePrevious);
    void store(qint32 fieldNumber, const FieldSample &sample, double fieldDiffIre);
    bool aborted() const { return abort.loadRelaxed() != 0; }
    void requestAbort() { abort.storeRelaxed(1); }

private:
    QString lumaFilename;
    QString chromaFilename;
    qint32 maxThreads;
    const TbcMetaData &metaData;
    FieldGeometry geometry;

    QAtomicInt abort;
    QElapsedTimer totalTimer;

    QMutex inputMutex;
    SourceVideo lumaVideo;
    SourceVideo chromaVideo;
    bool chromaOpen = false;
    qint32 inputFieldNumber = 1;
    qint32 lastFieldNumber = 0;

    QMutex outputMutex;
    FieldMetrics *output = nullptr;
    qint32 storedFields = 0;
    qint32 nextReportAt = 0;
};

class FieldWalker : public QThread
{
    Q_OBJECT
public:
    explicit FieldWalker(FieldWalkPool &pool, const FieldGeometry &geometry, QObject *parent = nullptr);

protected:
    void run() override;

private:
    FieldWalkPool &pool;
    FieldGeometry geometry;
};

#endif // PROCESSINGPOOL_H
