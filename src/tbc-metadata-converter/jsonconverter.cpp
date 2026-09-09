/******************************************************************************
 * jsonconverter.cpp
 * tbc-metadata-converter - Metadata converter tool for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "jsonconverter.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>

JsonConverter::JsonConverter(const QString &inputFilename, const QString &outputFilename, Direction direction,
                             bool repairFieldNumbering)
    : m_direction(direction), m_inputFilename(inputFilename), m_outputFilename(outputFilename),
      m_repairFieldNumbering(repairFieldNumbering)
{

}

JsonConverter::~JsonConverter() = default;

bool JsonConverter::process()
{
    switch (m_direction) {
        case Direction::JsonToSqlite:
            return processJsonToSqlite();
        case Direction::SqliteToJson:
            return processSqliteToJson();
    }

    qCritical() << "Unsupported conversion direction";
    return false;
}

bool JsonConverter::processJsonToSqlite()
{
    qInfo() << "Processing JSON file:" << m_inputFilename;
    
    // Check if input file exists
    QFileInfo inputFile(m_inputFilename);
    if (!inputFile.exists()) {
        qCritical() << "Input JSON file does not exist:" << m_inputFilename;
        return false;
    }
    
    // Load the JSON metadata using local TBC library
    TbcMetaData metaData;
    if (!metaData.read(m_inputFilename)) {
        qCritical() << "Failed to read JSON file:" << m_inputFilename;
        return false;
    }
    
    qInfo() << "Successfully loaded JSON metadata";

    // Before anything is written: the SQLite side keys field_record on
    // (capture_id, field_id) and writes it with INSERT OR REPLACE, so source
    // metadata carrying a duplicate seqNo converts into a database that
    // declares more fields than it holds. That database then crashes or
    // mis-reads in every consumer, so the conversion stops here instead.
    if (!ensureFieldNumbering(metaData)) {
        return false;
    }

    // Report on the contents
    reportMetadataContents(metaData);

    qInfo() << "Metadata analysis complete. Output SQLite file will be:" << m_outputFilename;

    // Write through the library, the one owner of the SQLite schema: this
    // tool used to carry its own copy of the DDL, which had already fallen
    // behind (no SECAM column, none of the segmentation tables). A stale
    // output file is replaced, not updated.
    if (QFileInfo::exists(m_outputFilename) && !QFile::remove(m_outputFilename)) {
        qCritical() << "Failed to remove the existing SQLite file:" << m_outputFilename;
        return false;
    }
    if (!metaData.write(m_outputFilename)) {
        qCritical() << "Failed to write SQLite file:" << m_outputFilename;
        // A failed write leaves a database with the schema and no rows, plus
        // its rollback journal. Left on disk it shadows the .tbc.json it was
        // converted from -- consumers open the .tbc.db in preference -- so
        // the asset would look broken rather than unconverted.
        removePartialOutput();
        return false;
    }

    qInfo() << "SQLite database created successfully:" << m_outputFilename;

    return true;
}

// Refuse (or, with the repair opt-in, mend) metadata whose field numbering
// will not survive conversion. See jsonconverter.h.
bool JsonConverter::ensureFieldNumbering(TbcMetaData &metaData)
{
    const TbcMetaData::FieldNumbering numbering = metaData.checkFieldNumbering();
    if (numbering.isValid) {
        return true;
    }

    if (!m_repairFieldNumbering) {
        qCritical().noquote() << "Field numbering in" << m_inputFilename << "is broken:"
                              << numbering.summary();
        qCritical() << "Refusing to convert: the SQLite output would declare more fields than it "
                       "holds, which every consumer reads as a damaged file. Re-run with --repair "
                       "to drop the repeated field numbers and renumber what is left (lossy: the "
                       "fields after a gap shift down by one).";
        return false;
    }

    const qint32 dropped = metaData.repairFieldNumbering();
    qWarning().noquote() << "Repaired field numbering in" << m_inputFilename << ":"
                         << numbering.summary();
    qWarning() << "Dropped" << dropped << "repeated field(s); the metadata now describes"
               << metaData.getNumberOfFields()
               << "fields. Field numbers after the first break no longer line up with the source.";

    const TbcMetaData::FieldNumbering afterRepair = metaData.checkFieldNumbering();
    if (!afterRepair.isValid) {
        qCritical().noquote() << "Repair did not produce sound field numbering:"
                              << afterRepair.summary();
        return false;
    }

    return true;
}

// Delete a half-written output file (and any rollback journal beside it).
void JsonConverter::removePartialOutput()
{
    for (const QString &path : {m_outputFilename, m_outputFilename + QStringLiteral("-journal"),
                                m_outputFilename + QStringLiteral("-wal"),
                                m_outputFilename + QStringLiteral("-shm")}) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            qWarning() << "Could not remove the partially written file:" << path;
        }
    }
}

bool JsonConverter::processSqliteToJson()
{
    qInfo() << "Processing SQLite file:" << m_inputFilename;

    QFileInfo inputFile(m_inputFilename);
    if (!inputFile.exists()) {
        qCritical() << "Input SQLite file does not exist:" << m_inputFilename;
        return false;
    }

    TbcMetaData metaData;
    if (!metaData.read(m_inputFilename)) {
        qCritical() << "Failed to read SQLite file:" << m_inputFilename;
        return false;
    }

    qInfo() << "Successfully loaded SQLite metadata";

    reportMetadataContents(metaData);

    qInfo() << "Metadata analysis complete. Output JSON file will be:" << m_outputFilename;

    if (!metaData.write(m_outputFilename)) {
        qCritical() << "Failed to write JSON file:" << m_outputFilename;
        return false;
    }

    qInfo() << "JSON file created successfully:" << m_outputFilename;
    return true;
}

void JsonConverter::reportMetadataContents(TbcMetaData &metaData)
{
    qInfo() << "=== Metadata Content Analysis ===";
    
    // Basic information
    qInfo() << "Video System:" << metaData.getVideoSystemDescription();
    qInfo() << "Number of Fields:" << metaData.getNumberOfFields();
    qInfo() << "Number of Frames:" << metaData.getNumberOfFrames();
    qInfo() << "First Field First:" << (metaData.getIsFirstFieldFirst() ? "Yes" : "No");
    
    // Comprehensive Video parameters matching README schema
    const TbcMetaData::VideoParameters &videoParams = metaData.getVideoParameters();
    qInfo() << "Video Parameters:";
    qInfo() << "  System:" << (videoParams.system == PAL ? "PAL" :
                                (videoParams.system == NTSC ? "NTSC" :
                                (videoParams.system == PAL_M ? "PAL_M" :
                                (videoParams.system == SECAM ? "SECAM" : "MESECAM"))));
    qInfo() << "  Field Width:" << videoParams.fieldWidth << "pixels";
    qInfo() << "  Field Height:" << videoParams.fieldHeight << "lines";
    qInfo() << "  Video Sample Rate:" << QString::number(videoParams.sampleRate, 'f', 0).toLongLong() << "Hz";
    qInfo() << "  Active Video Start:" << videoParams.activeVideoStart;
    qInfo() << "  Active Video End:" << videoParams.activeVideoEnd;
    qInfo() << "  Colour Burst Start:" << videoParams.colourBurstStart;
    qInfo() << "  Colour Burst End:" << videoParams.colourBurstEnd;
    qInfo() << "  White 16b IRE:" << videoParams.white16bIre;
    qInfo() << "  Black 16b IRE:" << videoParams.black16bIre;
    if (videoParams.blanking16bIre != -1) {
        qInfo() << "  Blanking 16b IRE:" << videoParams.blanking16bIre;
    }
    if (!videoParams.chromaDecoder.isEmpty()) {
        qInfo() << "  Chroma Decoder:" << videoParams.chromaDecoder;
    }
    if (videoParams.chromaGain != -1.0) {
        qInfo() << "  Chroma Gain:" << videoParams.chromaGain;
    }
    if (videoParams.chromaPhase != -1.0) {
        qInfo() << "  Chroma Phase:" << videoParams.chromaPhase;
    }
    if (videoParams.lumaNR != -1.0) {
        qInfo() << "  Luma NR:" << videoParams.lumaNR;
    }
    if (videoParams.ntscAdaptive != -1) {
        qInfo() << "  NTSC Adaptive:" << (videoParams.ntscAdaptive == 1 ? "Yes" : "No");
    }
    if (videoParams.ntscAdaptThreshold != -1.0) {
        qInfo() << "  NTSC Adapt Threshold:" << videoParams.ntscAdaptThreshold;
    }
    if (videoParams.ntscChromaWeight != -1.0) {
        qInfo() << "  NTSC Chroma Weight:" << videoParams.ntscChromaWeight;
    }
    if (videoParams.ntscPhaseCompensation != -1) {
        qInfo() << "  NTSC Phase Compensation:" << (videoParams.ntscPhaseCompensation == 1 ? "Yes" : "No");
    }
    if (videoParams.palTransformThreshold != -1.0) {
        qInfo() << "  PAL Transform Threshold:" << videoParams.palTransformThreshold;
    }
    qInfo() << "  Is Mapped:" << (videoParams.isMapped ? "Yes" : "No");
    qInfo() << "  Is Subcarrier Locked:" << (videoParams.isSubcarrierLocked ? "Yes" : "No");
    qInfo() << "  Is Widescreen:" << (videoParams.isWidescreen ? "Yes" : "No");
    if (!videoParams.gitBranch.isEmpty()) {
        qInfo() << "  Git Branch:" << videoParams.gitBranch;
    }
    if (!videoParams.gitCommit.isEmpty()) {
        qInfo() << "  Git Commit:" << videoParams.gitCommit;
    }
    if (!videoParams.tapeFormat.isEmpty()) {
        qInfo() << "  Tape Format:" << videoParams.tapeFormat;
    }
    
    // PCM Audio parameters (if present)
    const TbcMetaData::PcmAudioParameters &audioParams = metaData.getPcmAudioParameters();
    if (audioParams.isValid) {
        qInfo() << "PCM Audio Parameters:";
        qInfo() << "  Sample Rate:" << audioParams.sampleRate << "Hz";
        qInfo() << "  Bits per Sample:" << audioParams.bits;
        qInfo() << "  Is Signed:" << (audioParams.isSigned ? "Yes" : "No");
        qInfo() << "  Is Little Endian:" << (audioParams.isLittleEndian ? "Yes" : "No");
    } else {
        qInfo() << "PCM Audio Parameters: Not present";
    }
    
    // Count different types of data objects as per schema
    qint32 fieldsWithVbi = 0;
    qint32 fieldsWithVitc = 0;
    qint32 fieldsWithClosedCaptions = 0;
    qint32 fieldsWithVitsMetrics = 0;
    qint32 fieldsWithNtsc = 0;
    qint32 totalDropouts = 0;
    qint32 fieldsWithAudio = 0;
    qint32 paddedFields = 0;
    
    // Analyze each field for detailed statistics
    for (qint32 fieldNum = 1; fieldNum <= metaData.getNumberOfFields(); fieldNum++) {
        const TbcMetaData::Field &field = metaData.getField(fieldNum);
        
        // Count padded fields
        if (field.pad) paddedFields++;
        
        // Count fields with audio samples
        if (field.audioSamples > 0) fieldsWithAudio++;
        
        // Count VBI data
        const TbcMetaData::Vbi &vbi = metaData.getFieldVbi(fieldNum);
        if (vbi.inUse) fieldsWithVbi++;
        
        // Count VITC data
        const TbcMetaData::Vitc &vitc = metaData.getFieldVitc(fieldNum);
        if (vitc.inUse) fieldsWithVitc++;
        
        // Count Closed Caption data
        const TbcMetaData::ClosedCaption &cc = metaData.getFieldClosedCaption(fieldNum);
        if (cc.inUse) fieldsWithClosedCaptions++;
        
        // Count VITS Metrics
        const TbcMetaData::VitsMetrics &vits = metaData.getFieldVitsMetrics(fieldNum);
        if (vits.inUse) fieldsWithVitsMetrics++;
        
        // Count NTSC data
        const TbcMetaData::Ntsc &ntsc = metaData.getFieldNtsc(fieldNum);
        if (ntsc.inUse) fieldsWithNtsc++;
        
        // Count dropouts
        const DropOuts &dropouts = metaData.getFieldDropOuts(fieldNum);
        totalDropouts += dropouts.size();
    }
    
    qInfo() << "Field Data Objects Summary:";
    qInfo() << "  Fields with VBI data:" << fieldsWithVbi << "(" << 
               QString::number(100.0 * fieldsWithVbi / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Fields with VITC data:" << fieldsWithVitc << "(" << 
               QString::number(100.0 * fieldsWithVitc / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Fields with Closed Caption data:" << fieldsWithClosedCaptions << "(" << 
               QString::number(100.0 * fieldsWithClosedCaptions / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Fields with VITS Metrics:" << fieldsWithVitsMetrics << "(" << 
               QString::number(100.0 * fieldsWithVitsMetrics / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    if (videoParams.system == NTSC) {
        qInfo() << "  Fields with NTSC data:" << fieldsWithNtsc << "(" << 
                   QString::number(100.0 * fieldsWithNtsc / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    }
    qInfo() << "  Fields with Audio samples:" << fieldsWithAudio << "(" << 
               QString::number(100.0 * fieldsWithAudio / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Padded fields (no valid video):" << paddedFields << "(" << 
               QString::number(100.0 * paddedFields / metaData.getNumberOfFields(), 'f', 1).toDouble() << "%)";
    qInfo() << "  Total Dropout objects:" << totalDropouts;
    
    // Summary for conversion planning
    qInfo() << "SQLite Conversion Planning:";
    qInfo() << "  Main capture record: 1 row";
    qInfo() << "  PCM audio parameters:" << (audioParams.isValid ? "1 row" : "0 rows (no audio)");
    qInfo() << "  Field records:" << metaData.getNumberOfFields() << "rows";
    qInfo() << "  VBI rows:" << fieldsWithVbi;
    qInfo() << "  VITC rows:" << fieldsWithVitc;
    qInfo() << "  Closed Caption rows:" << fieldsWithClosedCaptions;
    qInfo() << "  VITS Metrics rows:" << fieldsWithVitsMetrics;
    qInfo() << "  Dropout rows:" << totalDropouts;
    
    qInfo() << "=== End Analysis ===";
}
