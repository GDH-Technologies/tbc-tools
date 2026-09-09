/******************************************************************************
 * sqliteio.h
 * tbc-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef SQLITEIO_H
#define SQLITEIO_H

#include <QString>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <stdexcept>

namespace SqliteValue
{
    int toIntOrDefault(const QSqlQuery &query, const char *column, int defaultValue = -1);
    qint64 toLongLongOrDefault(const QSqlQuery &query, const char *column, qint64 defaultValue = -1);
    double toDoubleOrDefault(const QSqlQuery &query, const char *column, double defaultValue = -1.0);
    bool toBoolOrDefault(const QSqlQuery &query, const char *column, bool defaultValue = false);
}

class SqliteReader
{
public:
    SqliteReader(const QString &fileName);
    ~SqliteReader();
    
    // Explicitly close the database connection
    void close();

    // Exception class to be thrown when parsing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message);
    }

    // Read capture-level metadata
    bool readCaptureMetadata(int &captureId, QString &system, QString &decoder,
                           QString &gitBranch, QString &gitCommit,
                           double &videoSampleRate, int &activeVideoStart, int &activeVideoEnd,
                           int &firstActiveFieldLine, int &lastActiveFieldLine,
                           int &firstActiveFrameLine, int &lastActiveFrameLine,
                           int &fieldWidth, int &fieldHeight, int &numberOfSequentialFields,
                           int &colourBurstStart, int &colourBurstEnd,
                           bool &isMapped, bool &isSubcarrierLocked, bool &isWidescreen,
                           int &white16bIre, int &black16bIre, int &blanking16bIre,
                           QString &chromaDecoder, double &chromaGain, double &chromaPhase, double &lumaNR,
                           int &ntscAdaptive, double &ntscAdaptThreshold, double &ntscChromaWeight,
                           int &ntscPhaseCompensation, double &palTransformThreshold,
                           int &userEditInSelection, int &userEditOutSelection,
                           int &userMarkerSelection, QString &userMarkerComment,
                           QString &userMarkersJson,
                           QString &captureNotes,
                           double &rfSourceSampleRateHz,
                           QString &osInfo, QString &decoderVersion);

    // Read PCM audio parameters
    bool readPcmAudioParameters(int captureId, int &bits, bool &isSigned,
                              bool &isLittleEndian, double &sampleRate);

    // Bulk reads of the segmentation tables (schema version 8). Each returns
    // false when the table is absent (an older database), which callers treat
    // as "no rows".
    bool readAllFieldPictureMetrics(int captureId, QSqlQuery &metricsQuery);
    bool readAllDecoderEvents(int captureId, QSqlQuery &eventsQuery);
    bool readAllSegments(int captureId, QSqlQuery &segmentsQuery);

    // Read field metadata
    bool readFields(int captureId, QSqlQuery &fieldsQuery);

    // Read field-specific data (individual queries - slower)
    bool readFieldVitsMetrics(int captureId, int fieldId, double &wSnr, double &bPsnr);
    bool readFieldVbi(int captureId, int fieldId, int &vbi0, int &vbi1, int &vbi2);
    bool readFieldVitc(int captureId, int fieldId, int vitcData[8]);
    bool readFieldClosedCaption(int captureId, int fieldId, int &data0, int &data1);
    bool readFieldDropouts(int captureId, int fieldId, QSqlQuery &dropoutsQuery);

    // Optimized bulk read methods for all fields (much faster)
    bool readAllFieldVitsMetrics(int captureId, QSqlQuery &vitsQuery);
    bool readAllFieldVbi(int captureId, QSqlQuery &vbiQuery);
    bool readAllFieldVitc(int captureId, QSqlQuery &vitcQuery);
    bool readAllFieldClosedCaptions(int captureId, QSqlQuery &ccQuery);
    bool readAllFieldDropouts(int captureId, QSqlQuery &dropoutsQuery);

private:
    QSqlDatabase db;
    QString connectionName;
};

class SqliteWriter
{
public:
    SqliteWriter(const QString &fileName);
    ~SqliteWriter();
    
    // Explicitly close the database connection
    void close();

    // Exception class to be thrown when writing fails
    class Error : public std::runtime_error
    {
    public:
        Error(std::string message) : std::runtime_error(message) {}
    };

    // Throw an Error exception with the given message
    [[noreturn]] void throwError(std::string message) {
        throw Error(message);
    }

    // Initialize database with schema
    bool createSchema();

    // Write capture-level metadata
    int writeCaptureMetadata(const QString &system, const QString &decoder,
                           const QString &gitBranch, const QString &gitCommit,
                           double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                           int firstActiveFieldLine, int lastActiveFieldLine,
                           int firstActiveFrameLine, int lastActiveFrameLine,
                           int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                           int colourBurstStart, int colourBurstEnd,
                           bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                           int white16bIre, int black16bIre, int blanking16bIre,
                           const QString &chromaDecoder, double chromaGain, double chromaPhase, double lumaNR,
                           int ntscAdaptive, double ntscAdaptThreshold, double ntscChromaWeight,
                           int ntscPhaseCompensation, double palTransformThreshold,
                           int userEditInSelection, int userEditOutSelection,
                           int userMarkerSelection, const QString &userMarkerComment,
                           const QString &userMarkersJson,
                           const QString &captureNotes,
                           double rfSourceSampleRateHz,
                           const QString &osInfo, const QString &decoderVersion);

    // Update existing capture metadata
    bool updateCaptureMetadata(int captureId, const QString &system, const QString &decoder,
                             const QString &gitBranch, const QString &gitCommit,
                             double videoSampleRate, int activeVideoStart, int activeVideoEnd,
                             int firstActiveFieldLine, int lastActiveFieldLine,
                             int firstActiveFrameLine, int lastActiveFrameLine,
                             int fieldWidth, int fieldHeight, int numberOfSequentialFields,
                             int colourBurstStart, int colourBurstEnd,
                             bool isMapped, bool isSubcarrierLocked, bool isWidescreen,
                             int white16bIre, int black16bIre, int blanking16bIre,
                             const QString &chromaDecoder, double chromaGain, double chromaPhase, double lumaNR,
                             int ntscAdaptive, double ntscAdaptThreshold, double ntscChromaWeight,
                             int ntscPhaseCompensation, double palTransformThreshold,
                             int userEditInSelection, int userEditOutSelection,
                             int userMarkerSelection, const QString &userMarkerComment,
                             const QString &userMarkersJson,
                             const QString &captureNotes,
                             double rfSourceSampleRateHz,
                             const QString &osInfo, const QString &decoderVersion);

    // Write PCM audio parameters
    bool writePcmAudioParameters(int captureId, int bits, bool isSigned,
                               bool isLittleEndian, double sampleRate);

    // Write field metadata. fileLoc is an RF sample offset and exceeds 32 bits
    // a few minutes into any capture, so it is 64-bit end to end.
    bool writeField(int captureId, int fieldId, int audioSamples, int decodeFaults,
                   double diskLoc, int efmTValues, int fieldPhaseId, qint64 fileLoc,
                   bool isFirstField, double medianBurstIre, bool pad, int syncConf,
                   bool ntscIsFmCodeDataValid, int ntscFmCodeData, bool ntscFieldFlag,
                   bool ntscIsVideoIdDataValid, int ntscVideoIdData, bool ntscWhiteFlag,
                   bool secamFirstLineIsRed);

    // Write field-specific data
    bool writeFieldVitsMetrics(int captureId, int fieldId, double wSnr, double bPsnr);
    bool writeFieldVbi(int captureId, int fieldId, int vbi0, int vbi1, int vbi2);
    bool writeFieldVitc(int captureId, int fieldId, const int vitcData[8]);
    bool writeFieldClosedCaption(int captureId, int fieldId, int data0, int data1);
    bool deleteCaptureDropouts(int captureId);
    bool deleteFieldDropouts(int captureId, int fieldId);
    bool writeFieldDropouts(int captureId, int fieldId, int startx, int endx, int fieldLine);

    // Segmentation tables (schema version 8). A NaN metric is stored as NULL.
    bool writeFieldPictureMetrics(int captureId, int fieldId, double lumaMeanIre, double fieldDiffIre,
                                  double blankingDevIre, double syncTipDevIre, double noiseIre,
                                  double burstAmpIre);
    bool deleteDecoderEvents(int captureId);
    bool writeDecoderEvent(int captureId, int fieldId, const QString &kind, const QVariant &fileLoc,
                           const QVariant &rfDeltaSamples, const QVariant &rfDeltaFields,
                           const QString &source, const QString &detailJson);
    bool deleteSegments(int captureId);
    bool writeSegment(int captureId, int segmentId, int startField, int endFieldExclusive,
                      const QString &kind, const QString &source, bool enabled,
                      const QString &title, const QString &comment, const QString &createdBy,
                      const QString &updatedAt, const QString &derivedFrom);

    // Transaction support
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

private:
    QSqlDatabase db;
    QString connectionName;
};

#endif // SQLITEIO_H