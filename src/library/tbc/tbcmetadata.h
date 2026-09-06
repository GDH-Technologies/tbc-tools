/******************************************************************************
 * tbcmetadata.h
 * tbc-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2025 Simon Inns
 * SPDX-FileCopyrightText: 2022 Ryan Holtz
 * SPDX-FileCopyrightText: 2022-2023 Adam Sampson
 * SPDX-FileCopyrightText: 2026 Hugo Caille
 * SPDX-FileCopyrightText: 2026 Harry Munday
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef TBCMETADATA_H
#define TBCMETADATA_H

#include <QString>
#include <QVector>
#include <QTemporaryFile>
#include <QDebug>
#include <array>
#include <cmath>
#include <limits>

#include "dropouts.h"

class SqliteReader;
class SqliteWriter;
class JsonReader;
class JsonWriter;

// The video system (combination of a line standard and a colour standard)
// Note: If you update this, be sure to update VIDEO_SYSTEM_DEFAULTS also
enum VideoSystem {
    PAL = 0,    // 625-line PAL
    NTSC,       // 525-line NTSC
    PAL_M,      // 525-line PAL
    SECAM,      // 625-line SECAM (FM chroma)
    MESECAM,    // 625-line MESECAM (FM chroma, VHS-style) — same decode as SECAM
};

bool parseVideoSystemName(QString name, VideoSystem &system);

class TbcMetaData
{

public:
    // VBI Metadata definition
    struct Vbi {
        bool inUse = false;
        std::array<qint32, 3> vbiData { 0, 0, 0 };

        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // Video metadata definition
    struct VideoParameters {
        // -- Members stored in the metadata --

        qint32 numberOfSequentialFields = -1;

        VideoSystem system = NTSC;
        bool isSubcarrierLocked = false;
        bool isWidescreen = false;

        qint32 colourBurstStart = -1;
        qint32 colourBurstEnd = -1;
        qint32 activeVideoStart = -1;
        qint32 activeVideoEnd = -1;

        qint32 white16bIre = -1;
        qint32 black16bIre = -1;
        qint32 blanking16bIre = -1;

        qint32 fieldWidth = -1;
        qint32 fieldHeight = -1;
        double sampleRate = -1.0;

        bool isMapped = false;
        QString tapeFormat = "";
        QString chromaDecoder = "";
        double chromaGain = -1.0;
        double chromaPhase = -1.0;
        double lumaNR = -1.0;
        qint32 ntscAdaptive = -1;
        double ntscAdaptThreshold = -1.0;
        double ntscChromaWeight = -1.0;
        qint32 ntscPhaseCompensation = -1;
        double palTransformThreshold = -1.0;

        QString gitBranch;
        QString gitCommit;
        qint32 userEditInSelection = -1;
        qint32 userEditOutSelection = -1;
        qint32 userMarkerSelection = -1;
        QString userMarkerComment;
        QString userMarkersJson;

        // The sample rate, in Hz, that the per-field fileLoc/diskLoc values are
        // expressed in: the decoder's RF working rate (vhs-decode resamples its
        // input to 40 MHz unless told not to). This is NOT sampleRate, which is
        // the TBC output rate. -1 when the decoder did not record it; consumers
        // then have to be told the rate or estimate it from the fileLoc deltas.
        double rfSourceSampleRateHz = -1.0;

        // Which decoder produced the metadata ("ld-decode" or "vhs-decode"), as
        // stored in the SQLite capture row. Empty when unknown (a JSON source);
        // the SQLite writer then infers it from tapeFormat.
        QString decoder;

        // Informational keys the decoders write into the JSON (never stored in
        // SQLite): kept so a JSON round trip does not drop them.
        QString osInfo;
        QString version;

        // -- Members set by the library --

        // Colour subcarrier frequency in Hz
        double fSC = -1.0;

        // The range of active lines within a frame.
        // This is the same information represented in two different ways, for
        // field- and frame-based processing respectively; the field range
        // should cover the active lines in both fields of a frame.
        // These are half-open ranges, where lines are numbered sequentially
        // from 1 within each field or interlaced frame.
        qint32 firstActiveFieldLine = -1;
        qint32 lastActiveFieldLine = -1;
        qint32 firstActiveFrameLine = -1;
        qint32 lastActiveFrameLine = -1;

        // Flags if our data has been initialized yet
        bool isValid = false;

        void read(SqliteReader &reader, int captureId);
        void write(SqliteWriter &writer, int captureId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // Specification for customising the range of active lines in VideoParameters.
    // -1 for any of these means to use the default for the standard.
    struct LineParameters {
        qint32 firstActiveFieldLine = -1;
        qint32 lastActiveFieldLine = -1;
        qint32 firstActiveFrameLine = -1;
        qint32 lastActiveFrameLine = -1;

        void applyTo(VideoParameters &videoParameters);
    };

    // VITS metrics metadata definition
    struct VitsMetrics {
        bool inUse = false;
        double wSNR = 0.0;
        double bPSNR = 0.0;

        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // NTSC Specific metadata definition
    struct ClosedCaption;
    struct Ntsc {
        bool inUse = false;
        bool isFmCodeDataValid = false;
        qint32 fmCodeData = 0;
        bool fieldFlag = false;
        bool isVideoIdDataValid = false;
        qint32 videoIdData = 0;
        bool whiteFlag = false;

        void read(SqliteReader &reader, int captureId, int fieldId, ClosedCaption &closedCaption);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
        void read(JsonReader &reader, ClosedCaption &closedCaption);
        void write(JsonWriter &writer) const;
    };

    // VITC timecode definition
    struct Vitc {
        bool inUse = false;

        // Just the VITC data, without the sync bits or CRC.
        // vitcData[0]'s LSB is bit 2; vitcData[7]'s MSB is bit 79.
        std::array<qint32, 8> vitcData;

        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // Closed Caption definition
    struct ClosedCaption {
        bool inUse = false;

        qint32 data0 = -1;
        qint32 data1 = -1;

        void read(SqliteReader &reader, int captureId, int fieldId);
        void write(SqliteWriter &writer, int captureId, int fieldId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // PCM sound metadata definition
    struct PcmAudioParameters {
        double sampleRate = -1.0;
        bool isLittleEndian = false;
        bool isSigned = false;
        qint32 bits = -1;

        // Flags if our data has been initialized yet
        bool isValid = false;

        void read(SqliteReader &reader, int captureId);
        void write(SqliteWriter &writer, int captureId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // Per-field picture metrics, measured by the decoder from the TBC field it
    // wrote (or backfilled by tbc-segments --write from a field walk). All in
    // IRE, 2 dp. A member that could not be measured is NaN: it is omitted
    // from JSON (the JSON reader cannot parse null) and NULL in SQLite.
    // Definitions are shared with tbc-segments' fieldmetrics.cpp.
    struct PictureMetrics {
        bool inUse = false;
        double lumaMeanIre = std::numeric_limits<double>::quiet_NaN();     // mean of the active area
        double fieldDiffIre = std::numeric_limits<double>::quiet_NaN();    // mean |field - field n-2| (same parity by output index)
        double blankingDevIre = std::numeric_limits<double>::quiet_NaN();  // back porch mean - blanking level
        double syncTipDevIre = std::numeric_limits<double>::quiet_NaN();   // sync tip mean - nominal sync tip
        double noiseIre = std::numeric_limits<double>::quiet_NaN();        // back porch standard deviation
        double burstAmpIre = std::numeric_limits<double>::quiet_NaN();     // colour burst peak-to-peak

        bool anyFinite() const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // Field metadata definition
    struct Field {
        qint32 seqNo = 0;   // Note: This is the unique primary-key
        bool isFirstField = false;
        qint32 syncConf = 0;
        double medianBurstIRE = 0.0;
        qint32 fieldPhaseID = -1;
        qint32 audioSamples = -1;

        VitsMetrics vitsMetrics;
        Vbi vbi;
        Ntsc ntsc;
        Vitc vitc;
        ClosedCaption closedCaption;
        DropOuts dropOuts;
        PictureMetrics pictureMetrics;
        bool pad = false;
        // SECAM: true when this field's first active line carries D'R (the R-Y
        // line). Used by the pre-demodulated Dr/Db SECAM decoder (see
        // secampredemoddecoder.h) for line identity, since that path has no FM
        // rest carrier to auto-detect it from. Round-tripped with the rest of
        // the field metadata; harmless (unused) for non-SECAM sources.
        bool secamFirstLineIsRed = false;

        double diskLoc = -1;
        qint64 fileLoc = -1;
        qint32 decodeFaults = -1;
        qint32 efmTValues = -1;

        // vhs-decode's field-order bookkeeping. JSON only (vhs-decode's own
        // SQLite writer has no columns for them); kept so a JSON round trip
        // through this library does not drop them. -1 / hasX == false = absent.
        qint32 burstStartLine = -1;
        bool detectedFirstField = false;
        bool hasDetectedFirstField = false;
        bool isDuplicateField = false;
        bool hasIsDuplicateField = false;

        void read(SqliteReader &reader, int captureId);
        void write(SqliteWriter &writer, int captureId) const;
        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // A fact the decoder knew while decoding that cannot be re-derived from the
    // per-field records: a sync-loss jump, a skipped/duplicated/dropped field at
    // a seam, a --resume seam, an LD redo. Append-only; never edited by tools.
    // Field references are 0-based (the SQLite field_id), half-open where a
    // range applies. `source` is "decoder" for rows the decoder wrote and
    // "tbc-segments" for rows reconstructed from the field records by a
    // backfill.
    struct DecoderEvent {
        QString kind;
        qint32 field = -1;                 // first written field at/after the event, 0-based
        qint64 fileLoc = -1;               // RF sample offset the event happened at (-1 unknown)
        bool hasRfDeltaSamples = false;
        qint64 rfDeltaSamples = 0;         // RF advance across the event (may be negative)
        double rfDeltaFields = std::numeric_limits<double>::quiet_NaN(); // rfDeltaSamples / nominal samples per field
        QString source = QStringLiteral("decoder");
        QString detailJson;                // kind-specific extras, opaque JSON text

        // A kind that marks a discontinuity in the recording.
        bool isSeam() const;

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // A recording segment: the editable layer over the decoder's records.
    // Derived by the tbc-tools segments library (source "derived") or edited by
    // an operator (source "user"). Field numbers are 0-based, half-open.
    struct Segment {
        qint32 id = -1;                    // stable; never renumbered; new = max + 1
        qint32 startField = 0;
        qint32 endFieldExclusive = 0;
        QString kind = QStringLiteral("unknown");   // clip | blank | noise | unknown
        QString source = QStringLiteral("derived"); // derived | user
        bool enabled = true;
        QString title;
        QString comment;
        QString createdBy;
        QString updatedAt;                 // ISO-8601 UTC
        QString derivedFrom;               // JSON text: tool, commit, thresholds

        void read(JsonReader &reader);
        void write(JsonWriter &writer) const;
    };

    // CLV timecode (used by frame number conversion methods)
    struct ClvTimecode {
        qint32 hours;
        qint32 minutes;
        qint32 seconds;
        qint32 pictureNumber;
    };

    TbcMetaData();

    // Prevent copying or assignment
    TbcMetaData(const TbcMetaData &) = delete;
    TbcMetaData& operator=(const TbcMetaData &) = delete;

    void clear();
    bool read(QString fileName);
    bool write(QString fileName) const;
    void readFields(JsonReader &reader);
    void writeFields(JsonWriter &writer) const;
    void readFields(SqliteReader &reader, int captureId);
    void writeFields(SqliteWriter &writer, int captureId) const;

    // The SQLite file is the canonical store and the JSON a projection of it.
    // Given a .tbc.json path whose .tbc.db sibling exists, returns the .db
    // path; otherwise returns fileName unchanged. Tools open whatever this
    // returns.
    static QString resolveMetadataPath(const QString &fileName);
    // The .tbc.db sibling path of a .tbc.json path (or fileName itself when it
    // is already SQLite).
    static QString sqliteSiblingPath(const QString &fileName);
    static bool isJsonMetadataFilename(const QString &fileName);

    // Write SQLite-first: the .tbc.db (created from this object when only a
    // .tbc.json exists, updated in place otherwise) and then, when a .tbc.json
    // sibling exists or fileName named one, that JSON rewritten atomically as
    // a projection (<json>.tmp then rename). Returns the canonical (.db) path
    // written through *canonicalPath when given.
    bool writeWithProjection(const QString &fileName, QString *canonicalPath = nullptr) const;

    // Decoder events and segments (see the struct comments)
    const QVector<DecoderEvent> &getDecoderEvents() const;
    void setDecoderEvents(const QVector<DecoderEvent> &events);
    void appendDecoderEvent(const DecoderEvent &event);

    const QVector<Segment> &getSegments() const;
    void setSegments(const QVector<Segment> &newSegments);
    // Appends with id = max existing id + 1 (or 1) and returns that id
    qint32 appendSegment(const Segment &segment);

    // Picture metrics of a field (1-based sequential field number)
    const PictureMetrics &getFieldPictureMetrics(qint32 sequentialFieldNumber) const;
    void updateFieldPictureMetrics(const PictureMetrics &pictureMetrics, qint32 sequentialFieldNumber);
    // True when at least one field carries a finite picture metric
    bool hasPictureMetrics() const;

    const VideoParameters &getVideoParameters() const;
    void setVideoParameters(const VideoParameters &videoParameters);

    const PcmAudioParameters &getPcmAudioParameters() const;
    void setPcmAudioParameters(const PcmAudioParameters &pcmAudioParam);

    // Handle line parameters
    void processLineParameters(TbcMetaData::LineParameters &_lineParameters);

    // Get field metadata
    const Field &getField(qint32 sequentialFieldNumber) const;
    const VitsMetrics &getFieldVitsMetrics(qint32 sequentialFieldNumber) const;
    const Vbi &getFieldVbi(qint32 sequentialFieldNumber) const;
    const Ntsc &getFieldNtsc(qint32 sequentialFieldNumber) const;
    const Vitc &getFieldVitc(qint32 sequentialFieldNumber) const;
    const ClosedCaption &getFieldClosedCaption(qint32 sequentialFieldNumber) const;
    const DropOuts &getFieldDropOuts(qint32 sequentialFieldNumber) const;

    // Set field metadata
    void updateField(const Field &field, qint32 sequentialFieldNumber);
    void updateFieldVitsMetrics(const TbcMetaData::VitsMetrics &vitsMetrics, qint32 sequentialFieldNumber);
    void updateFieldVbi(const TbcMetaData::Vbi &vbi, qint32 sequentialFieldNumber);
    void updateFieldNtsc(const TbcMetaData::Ntsc &ntsc, qint32 sequentialFieldNumber);
    void updateFieldVitc(const TbcMetaData::Vitc &vitc, qint32 sequentialFieldNumber);
    void updateFieldClosedCaption(const TbcMetaData::ClosedCaption &closedCaption, qint32 sequentialFieldNumber);
    void updateFieldDropOuts(const DropOuts &dropOuts, qint32 sequentialFieldNumber);
    void clearFieldDropOuts(qint32 sequentialFieldNumber);

    void appendField(const Field &field);

    void setNumberOfFields(qint32 numberOfFields);
    qint32 getNumberOfFields() const;
    qint32 getNumberOfFrames() const;
    qint32 getFirstFieldNumber(qint32 frameNumber) const;
    qint32 getSecondFieldNumber(qint32 frameNumber) const;

    void setIsFirstFieldFirst(bool flag);
    bool getIsFirstFieldFirst() const;

    qint32 convertClvTimecodeToFrameNumber(TbcMetaData::ClvTimecode clvTimeCode);
    TbcMetaData::ClvTimecode convertFrameNumberToClvTimecode(qint32 clvFrameNumber);

    // PCM Analogue audio helper methods
    qint32 getFieldPcmAudioStart(qint32 sequentialFieldNumber) const;
    qint32 getFieldPcmAudioLength(qint32 sequentialFieldNumber) const;

    // Video system helper methods
    QString getVideoSystemDescription() const;

private:
    bool isFirstFieldFirst;
    VideoParameters videoParameters;
    PcmAudioParameters pcmAudioParameters;
    QVector<Field> fields;
    QVector<DecoderEvent> decoderEvents;
    QVector<Segment> segments;
    QVector<qint32> pcmAudioFieldStartSampleMap;
    QVector<qint32> pcmAudioFieldLengthMap;

    void initialiseVideoSystemParameters();
    qint32 getFieldNumber(qint32 frameNumber, qint32 field) const;
    void generatePcmAudioMap();

    void readDecoderEvents(JsonReader &reader);
    void writeDecoderEvents(JsonWriter &writer) const;
    void readSegments(JsonReader &reader);
    void writeSegments(JsonWriter &writer) const;
    void readDecoderEvents(SqliteReader &reader, int captureId);
    void writeDecoderEvents(SqliteWriter &writer, int captureId) const;
    void readSegments(SqliteReader &reader, int captureId);
    void writeSegments(SqliteWriter &writer, int captureId) const;
    bool writeJson(const QString &fileName) const;
    bool writeSqlite(const QString &fileName) const;
    QString effectiveDecoderName() const;
};

#endif // TBCMETADATA_H
