/************************************************************************

    decoderpool.h

    tbc-process-vbi - VBI and IEC NTSC specific processor for ld-decode
    Copyright (C) 2018-2019 Simon Inns

    This file is part of tbc-tools.

    tbc-process-vbi is free software: you can redistribute it and/or
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

#ifndef DECODERPOOL_H
#define DECODERPOOL_H

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QMutex>
#include <QThread>

#include "sourcevideo.h"
#include "tbc/vbiprocessingoptions.h"
#include "tbcmetadata.h"
#include "vbilinedecoder.h"

class DecoderPool
{
public:
    // Public methods
    explicit DecoderPool(QString _inputFilename, QString _outputMetadataFilename,
                        qint32 _maxThreads, TbcMetaData &_metaData,
                        VbiProcessingOptions _options = VbiProcessingOptions{});
    bool process();

    // Accessor used by worker threads to gate per-type decoding
    const VbiProcessingOptions &options() const { return processingOptions; }

    // Member functions used by worker threads
    bool getInputField(qint32 &fieldNumber, SourceVideo::Data &fieldVideoData, TbcMetaData::Field &fieldMetadata, TbcMetaData::VideoParameters &videoParameters);
    bool setOutputField(qint32 fieldNumber, const TbcMetaData::Field& fieldMetadata);

private:
    QString inputFilename;
    QString outputMetadataFilename;
    qint32 maxThreads;
    QElapsedTimer totalTimer;

    // Atomic abort flag shared by worker threads; workers watch this, and shut
    // down as soon as possible if it becomes true
    QAtomicInt abort;

    // Input stream information (all guarded by inputMutex while threads are running)
    QMutex inputMutex;
    qint32 inputFieldNumber;
    qint32 lastFieldNumber;
    qint32 processedFieldNumber;
    qint32 progressReportInterval;
    TbcMetaData &metaData;
    SourceVideo sourceVideo;
    VbiProcessingOptions processingOptions;

    // Output stream information (all guarded by outputMutex while threads are running)
    QMutex outputMutex;
    QFile targetMetadata;
};

#endif // DECODERPOOL_H
