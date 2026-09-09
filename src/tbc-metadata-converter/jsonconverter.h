/******************************************************************************
 * jsonconverter.h
 * tbc-metadata-converter - Metadata converter tool for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef JSONCONVERTER_H
#define JSONCONVERTER_H

#include <QString>
#include "tbcmetadata.h"

class JsonConverter
{
public:
    enum class Direction {
        JsonToSqlite,
        SqliteToJson
    };

    // repairFieldNumbering: convert source metadata whose field numbering is
    // broken (a duplicate or missing seqNo) by dropping the repeats and
    // renumbering, instead of refusing it. Lossy -- see
    // TbcMetaData::repairFieldNumbering() -- so it is opt-in only.
    JsonConverter(const QString &inputFilename, const QString &outputFilename, Direction direction,
                  bool repairFieldNumbering = false);
    ~JsonConverter();

    bool process();

private:
    Direction m_direction;
    QString m_inputFilename;
    QString m_outputFilename;
    bool m_repairFieldNumbering;
    bool processJsonToSqlite();
    // Refuses (or, with --repair, mends) metadata whose field numbering would
    // not survive the trip through field_record's (capture_id, field_id) key.
    bool ensureFieldNumbering(TbcMetaData &metaData);
    // Deletes a half-written output file and any SQLite sidecar beside it, so
    // a failed conversion leaves nothing that later shadows the source.
    void removePartialOutput();
    bool processSqliteToJson();
    void reportMetadataContents(TbcMetaData &metaData);
    void reportJsonContents(TbcMetaData &metaData);
    void countDropouts(const TbcMetaData &metaData, qint32 &totalDropouts);
};

#endif // JSONCONVERTER_H