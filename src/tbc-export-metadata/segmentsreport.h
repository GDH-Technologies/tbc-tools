/************************************************************************

    segmentsreport.h

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

#ifndef SEGMENTSREPORT_H
#define SEGMENTSREPORT_H

#include <QString>
#include <QtGlobal>

#include "tbcmetadata.h"

/*!
    Write the recording-segment report (tbc-segments' schema 1 JSON) from the
    metadata alone: the stored segments (derived through the library when none
    are stored), the events derived from the metadata and any stored picture
    metrics, and each segment's 1-based frame range under the mixed-frame
    rule. No TBC is read. fileName "-" writes to stdout.

    @param inputPath The metadata file the report describes (recorded in it).
    @param startFrameOneBased Optional 1-based start frame; < 1 for the first.
    @param lengthFrames Optional frame length; < 1 to the end of the input.
*/
bool writeSegmentsJson(TbcMetaData &metaData,
                       const QString &fileName,
                       const QString &inputPath,
                       qint32 startFrameOneBased = -1,
                       qint32 lengthFrames = -1);

#endif
