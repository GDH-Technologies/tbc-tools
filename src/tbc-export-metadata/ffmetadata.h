/************************************************************************

    ffmetadata.h

    tbc-export-metadata - Export ld-decode metadata into other formats
    Copyright (C) 2020 Adam Sampson

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

#ifndef FFMETADATA_H
#define FFMETADATA_H

#include <QString>
#include <QtGlobal>

#include "tbcmetadata.h"

/*!
    Which stored recording segments become chapters.
*/
enum class FfmetadataSegmentMode {
    EnabledSegments,   // one chapter per enabled stored segment (the default)
    AllSegments,       // every stored segment, disabled ones included
    NoSegments         // ignore stored segments: LaserDisc navigation chapters only
};

/*!
    Write an FFMETADATA1 file containing navigation information.

    When the metadata holds recording segments (written by vhs-decode's
    consumers: tbc-segments --write-segments, ld-analyse's Segments viewer)
    each selected segment becomes one [CHAPTER] and the LaserDisc navigation
    chapters are not written; START/END are the segment's 0-based fields
    rebased to the export start field, so a per-segment export carries
    correctly placed chapters. The legacy single user marker is still
    spliced in. Without stored segments (or with NoSegments) the output is
    unchanged: LaserDisc navigation chapters plus the user marker.

    This is FFmpeg's generic metadata format, and can be used to provide
    metadata for chapter-supporting formats like Matroska.
    Format description: <https://ffmpeg.org/ffmpeg-formats.html#Metadata-1>

    @param startFrameOneBased Optional 1-based start frame for range export.
           Pass a value < 1 to export from the first frame.
    @param lengthFrames Optional frame length for range export.
           Pass a value < 1 to export to the end of the input.
    @param includeVitcTimecode When true, include FFmpeg-style VITC timecode
           in the output header when available.
    @param segmentMode Which stored segments become chapters.

    Returns true on success, false on failure.
*/
bool writeFfmetadata(TbcMetaData &metaData,
                     const QString &fileName,
                     qint32 startFrameOneBased = -1,
                     qint32 lengthFrames = -1,
                     bool includeVitcTimecode = true,
                     FfmetadataSegmentMode segmentMode = FfmetadataSegmentMode::EnabledSegments);

#endif
