/************************************************************************

    testmetadata.cpp

    Unit tests for metadata classes
    Copyright (C) 2022 Adam Sampson
    Copyright (C) 2025 Simon Inns

    This file is part of tbc-tools.

    tbc-tools is free software: you can redistribute it and/or
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

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "tbcmetadata.h"
#include "tbc/logging.h"

// The checks must fire in Release builds too (CMAKE_BUILD_TYPE=Release
// defines NDEBUG, which silences assert)
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " #cond   \
                      << "\n";                                                             \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (0)

namespace {

bool nearly(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance;
}

// A small NTSC capture as vhs-decode would describe it
void buildMetadata(TbcMetaData &metaData, qint32 fields, bool withExtras)
{
    TbcMetaData::VideoParameters vp;
    vp.system = NTSC;
    vp.fieldWidth = 910;
    vp.fieldHeight = 263;
    vp.sampleRate = 14318181.0;
    vp.black16bIre = 16384;
    vp.white16bIre = 54016;
    vp.blanking16bIre = 16384;
    vp.colourBurstStart = 78;
    vp.colourBurstEnd = 110;
    vp.activeVideoStart = 134;
    vp.activeVideoEnd = 896;
    vp.tapeFormat = QStringLiteral("VHS");
    vp.numberOfSequentialFields = fields;
    vp.isValid = true;
    if (withExtras) {
        vp.rfSourceSampleRateHz = 40000000.0;
        vp.decoder = QStringLiteral("vhs-decode");
        vp.osInfo = QStringLiteral("Linux-6.0");
        vp.version = QStringLiteral("vhs-decode 0.3.5");
        vp.gitBranch = QStringLiteral("main");
        vp.gitCommit = QStringLiteral("abc1234");
    }
    metaData.setVideoParameters(vp);

    for (qint32 i = 0; i < fields; i++) {
        TbcMetaData::Field field;
        field.isFirstField = (i % 2) == 0;
        field.syncConf = 45;
        field.fileLoc = static_cast<qint64>(i) * 675840;
        if (i == 3) field.fileLoc = (static_cast<qint64>(1) << 33) + 5;   // past 32 bits
        field.diskLoc = i;
        field.decodeFaults = (i == 5) ? 4 : -1;
        field.vitsMetrics.inUse = true;
        field.vitsMetrics.bPSNR = 40.5;
        if (withExtras) {
            field.pictureMetrics.lumaMeanIre = 30.0 + i;
            field.pictureMetrics.fieldDiffIre = (i >= 2) ? 1.25 : std::numeric_limits<double>::quiet_NaN();
            field.pictureMetrics.noiseIre = 2.0;
            field.pictureMetrics.blankingDevIre = 0.5;
            field.pictureMetrics.syncTipDevIre = -0.25;
            field.pictureMetrics.burstAmpIre = (i % 2) ? std::numeric_limits<double>::quiet_NaN() : 40.0;
            field.pictureMetrics.inUse = true;
            if (i == 2) {
                field.dropOuts.append(100, 120, 10);
                field.dropOuts.append(300, 340, 11);
                field.dropOuts.append(500, 510, 12);
            }
            if (i == 6) {
                field.burstStartLine = 7;
                field.detectedFirstField = false;
                field.hasDetectedFirstField = true;
                field.isDuplicateField = true;
                field.hasIsDuplicateField = true;
            }
        }
        metaData.appendField(field);
    }

    if (withExtras) {
        TbcMetaData::DecoderEvent skipped;
        skipped.kind = QStringLiteral("skipped_field");
        skipped.field = 5;
        skipped.fileLoc = static_cast<qint64>(5) * 675840;
        skipped.hasRfDeltaSamples = true;
        skipped.rfDeltaSamples = 1351680;
        skipped.rfDeltaFields = 2.0;
        skipped.detailJson = QStringLiteral("{\"action\":\"flip\",\"distanceFields\":2.0}");
        metaData.appendDecoderEvent(skipped);

        TbcMetaData::DecoderEvent jump;
        jump.kind = QStringLiteral("no_sync_pulses");
        jump.field = 8;
        jump.hasRfDeltaSamples = true;
        jump.rfDeltaSamples = 4000000;
        metaData.appendDecoderEvent(jump);

        TbcMetaData::Segment clip;
        clip.startField = 0;
        clip.endFieldExclusive = 5;
        clip.kind = QStringLiteral("clip");
        clip.title = QStringLiteral("First clip");
        clip.createdBy = QStringLiteral("testmetadata");
        clip.updatedAt = QStringLiteral("2026-09-06T12:00:00Z");
        clip.derivedFrom = QStringLiteral("{\"tool\":\"testmetadata\"}");
        CHECK(metaData.appendSegment(clip) == 1);

        TbcMetaData::Segment noise;
        noise.startField = 5;
        noise.endFieldExclusive = fields;
        noise.kind = QStringLiteral("noise");
        noise.source = QStringLiteral("user");
        noise.enabled = false;
        noise.comment = QStringLiteral("snow after the seam");
        CHECK(metaData.appendSegment(noise) == 2);
    }
}

void checkExtras(const TbcMetaData &metaData, qint32 fields, bool fromSqlite = false)
{
    const TbcMetaData::VideoParameters &vp = metaData.getVideoParameters();
    CHECK(nearly(vp.rfSourceSampleRateHz, 40000000.0));
    // Decoder provenance survives both stores (os_info / decoder_version columns)
    CHECK(vp.osInfo == QStringLiteral("Linux-6.0"));
    CHECK(vp.version == QStringLiteral("vhs-decode 0.3.5"));
    if (fromSqlite) {
        CHECK(vp.decoder == QStringLiteral("vhs-decode"));
    }
    CHECK(vp.tapeFormat == QStringLiteral("VHS"));
    CHECK(metaData.getNumberOfFields() == fields);

    CHECK(metaData.getField(4).fileLoc == (static_cast<qint64>(1) << 33) + 5);
    CHECK(metaData.getField(6).decodeFaults == 4);

    const TbcMetaData::PictureMetrics &m0 = metaData.getFieldPictureMetrics(1);
    CHECK(m0.inUse);
    CHECK(nearly(m0.lumaMeanIre, 30.0));
    CHECK(std::isnan(m0.fieldDiffIre));          // not measurable on the first fields
    CHECK(nearly(m0.burstAmpIre, 40.0));
    const TbcMetaData::PictureMetrics &m1 = metaData.getFieldPictureMetrics(2);
    CHECK(std::isnan(m1.burstAmpIre));           // odd field: no burst measured
    CHECK(nearly(m1.noiseIre, 2.0));
    const TbcMetaData::PictureMetrics &m2 = metaData.getFieldPictureMetrics(3);
    CHECK(nearly(m2.fieldDiffIre, 1.25));
    CHECK(metaData.hasPictureMetrics());

    CHECK(metaData.getField(3).dropOuts.size() == 3);
    CHECK(metaData.getField(3).dropOuts.startx(1) == 300);

    const QVector<TbcMetaData::DecoderEvent> &events = metaData.getDecoderEvents();
    CHECK(events.size() == 2);
    CHECK(events[0].kind == QStringLiteral("skipped_field"));
    CHECK(events[0].field == 5);
    CHECK(events[0].fileLoc == static_cast<qint64>(5) * 675840);
    CHECK(events[0].hasRfDeltaSamples && events[0].rfDeltaSamples == 1351680);
    CHECK(nearly(events[0].rfDeltaFields, 2.0));
    CHECK(events[0].source == QStringLiteral("decoder"));
    CHECK(events[0].detailJson.contains(QStringLiteral("\"action\":\"flip\"")));
    CHECK(!events[0].isSeam());
    CHECK(events[1].kind == QStringLiteral("no_sync_pulses"));
    CHECK(events[1].fileLoc == -1);
    CHECK(std::isnan(events[1].rfDeltaFields));
    CHECK(events[1].isSeam());

    const QVector<TbcMetaData::Segment> &segments = metaData.getSegments();
    CHECK(segments.size() == 2);
    CHECK(segments[0].id == 1 && segments[0].startField == 0 && segments[0].endFieldExclusive == 5);
    CHECK(segments[0].kind == QStringLiteral("clip") && segments[0].source == QStringLiteral("derived"));
    CHECK(segments[0].enabled && segments[0].title == QStringLiteral("First clip"));
    CHECK(segments[0].updatedAt == QStringLiteral("2026-09-06T12:00:00Z"));
    CHECK(segments[0].derivedFrom == QStringLiteral("{\"tool\":\"testmetadata\"}"));
    CHECK(segments[1].id == 2 && segments[1].kind == QStringLiteral("noise"));
    CHECK(segments[1].source == QStringLiteral("user") && !segments[1].enabled);
    CHECK(segments[1].comment == QStringLiteral("snow after the seam"));
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

int queryInt(const QString &dbPath, const QString &sql)
{
    const QString connection = QStringLiteral("testmetadata_probe");
    int value = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        CHECK(db.open());
        QSqlQuery query(db);
        CHECK(query.exec(sql));
        CHECK(query.next());
        value = query.value(0).toInt();
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return value;
}

void execSql(const QString &dbPath, const QString &sql)
{
    const QString connection = QStringLiteral("testmetadata_exec");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        CHECK(db.open());
        QSqlQuery query(db);
        CHECK(query.exec(sql));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QString queryString(const QString &dbPath, const QString &sql)
{
    const QString connection = QStringLiteral("testmetadata_probe_s");
    QString value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        CHECK(db.open());
        QSqlQuery query(db);
        CHECK(query.exec(sql));
        CHECK(query.next());
        value = query.value(0).toString();
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return value;
}

// vhs-decode's schema version 1 (lddecode/tbc_db.py), verbatim, so the
// migration test starts from exactly what a decode on disk looks like
const char *kVhsDecodeV1Ddl = R"(
PRAGMA user_version = 1;

CREATE TABLE capture (
    capture_id INTEGER PRIMARY KEY,
    system TEXT NOT NULL CHECK (system IN ('NTSC','PAL','PAL_M')),
    decoder TEXT NOT NULL CHECK (decoder IN ('ld-decode','vhs-decode')),
    git_branch TEXT,
    git_commit TEXT,
    video_sample_rate REAL,
    active_video_start INTEGER,
    active_video_end INTEGER,
    field_width INTEGER,
    field_height INTEGER,
    number_of_sequential_fields INTEGER,
    colour_burst_start INTEGER,
    colour_burst_end INTEGER,
    is_mapped INTEGER CHECK (is_mapped IN (0,1)),
    is_subcarrier_locked INTEGER CHECK (is_subcarrier_locked IN (0,1)),
    is_widescreen INTEGER CHECK (is_widescreen IN (0,1)),
    white_16b_ire INTEGER,
    black_16b_ire INTEGER,
    blanking_16b_ire INTEGER,
    capture_notes TEXT
);

CREATE TABLE pcm_audio_parameters (
    capture_id INTEGER PRIMARY KEY REFERENCES capture(capture_id) ON DELETE CASCADE,
    bits INTEGER,
    is_signed INTEGER CHECK (is_signed IN (0,1)),
    is_little_endian INTEGER CHECK (is_little_endian IN (0,1)),
    sample_rate REAL
);

CREATE TABLE field_record (
    capture_id INTEGER NOT NULL REFERENCES capture(capture_id) ON DELETE CASCADE,
    field_id INTEGER NOT NULL,
    audio_samples INTEGER,
    decode_faults INTEGER,
    disk_loc REAL,
    efm_t_values INTEGER,
    field_phase_id INTEGER,
    file_loc INTEGER,
    is_first_field INTEGER CHECK (is_first_field IN (0,1)),
    median_burst_ire REAL,
    pad INTEGER CHECK (pad IN (0,1)),
    sync_conf INTEGER,
    ntsc_is_fm_code_data_valid INTEGER CHECK (ntsc_is_fm_code_data_valid IN (0,1)),
    ntsc_fm_code_data INTEGER,
    ntsc_field_flag INTEGER CHECK (ntsc_field_flag IN (0,1)),
    ntsc_is_video_id_data_valid INTEGER CHECK (ntsc_is_video_id_data_valid IN (0,1)),
    ntsc_video_id_data INTEGER,
    ntsc_white_flag INTEGER CHECK (ntsc_white_flag IN (0,1)),
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE vits_metrics (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    b_psnr REAL,
    w_snr REAL,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE vbi (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    vbi0 INTEGER NOT NULL,
    vbi1 INTEGER NOT NULL,
    vbi2 INTEGER NOT NULL,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE drop_outs (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    field_line INTEGER NOT NULL,
    startx INTEGER NOT NULL,
    endx INTEGER NOT NULL,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id, field_line, startx, endx)
);

CREATE TABLE vitc (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    vitc0 INTEGER NOT NULL,
    vitc1 INTEGER NOT NULL,
    vitc2 INTEGER NOT NULL,
    vitc3 INTEGER NOT NULL,
    vitc4 INTEGER NOT NULL,
    vitc5 INTEGER NOT NULL,
    vitc6 INTEGER NOT NULL,
    vitc7 INTEGER NOT NULL,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);

CREATE TABLE closed_caption (
    capture_id INTEGER NOT NULL,
    field_id INTEGER NOT NULL,
    data0 INTEGER,
    data1 INTEGER,
    FOREIGN KEY (capture_id, field_id)
        REFERENCES field_record(capture_id, field_id) ON DELETE CASCADE,
    PRIMARY KEY (capture_id, field_id)
);
)";

void createVhsDecodeV1Database(const QString &dbPath, int fields)
{
    const QString connection = QStringLiteral("testmetadata_v1");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        CHECK(db.open());
        const QStringList statements = QString::fromUtf8(kVhsDecodeV1Ddl).split(';', Qt::SkipEmptyParts);
        for (const QString &statement : statements) {
            const QString trimmed = statement.trimmed();
            if (trimmed.isEmpty()) continue;
            QSqlQuery query(db);
            if (!query.exec(trimmed)) {
                std::cerr << "v1 DDL failed: " << query.lastError().text().toStdString() << "\n";
                CHECK(false);
            }
        }
        QSqlQuery capture(db);
        CHECK(capture.exec(QStringLiteral(
            "INSERT INTO capture (capture_id, system, decoder, git_branch, git_commit, video_sample_rate, "
            "active_video_start, active_video_end, field_width, field_height, number_of_sequential_fields, "
            "colour_burst_start, colour_burst_end, is_mapped, is_subcarrier_locked, is_widescreen, "
            "white_16b_ire, black_16b_ire, blanking_16b_ire, capture_notes) VALUES "
            "(1, 'NTSC', 'vhs-decode', 'main', 'abc1234', 14318181.0, 134, 896, 910, 263, %1, 78, 110, "
            "0, 0, 0, 54016, 16384, 16384, 'VHS')").arg(fields)));
        for (int i = 0; i < fields; i++) {
            QSqlQuery field(db);
            CHECK(field.exec(QStringLiteral(
                "INSERT INTO field_record (capture_id, field_id, decode_faults, disk_loc, field_phase_id, "
                "file_loc, is_first_field, pad, sync_conf) VALUES (1, %1, NULL, %2, %3, %4, %5, 0, 45)")
                .arg(i).arg(i).arg((i % 4) + 1).arg(static_cast<qint64>(i) * 675840).arg(i % 2 == 0 ? 1 : 0)));
            QSqlQuery vits(db);
            CHECK(vits.exec(QStringLiteral(
                "INSERT INTO vits_metrics (capture_id, field_id, b_psnr, w_snr) VALUES (1, %1, 40.5, 0)").arg(i)));
        }
        QSqlQuery dropout(db);
        CHECK(dropout.exec(QStringLiteral(
            "INSERT INTO drop_outs (capture_id, field_id, field_line, startx, endx) VALUES (1, 2, 10, 100, 120)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

} // namespace

// Run unit tests for VideoSystem
void testVideoSystem() {
    std::cerr << "Testing VideoSystem\n";

    VideoSystem system;
    bool b;

    b = parseVideoSystemName("PAL", system);
    CHECK(b);
    CHECK(system == PAL);
    b = parseVideoSystemName("NTSC", system);
    CHECK(b);
    CHECK(system == NTSC);
    b = parseVideoSystemName("PAL_M", system);
    CHECK(b);
    CHECK(system == PAL_M);
    b = parseVideoSystemName("", system);
    CHECK(!b);
    b = parseVideoSystemName("SPURIOUS", system);
    CHECK(!b);
}

// JSON projection: everything round-trips, NaN is omitted, no null anywhere
void testJsonRoundTrip()
{
    std::cerr << "Testing JSON round trip\n";
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("capture.tbc.json"));

    TbcMetaData original;
    buildMetadata(original, 10, true);
    CHECK(original.write(jsonPath));

    const QString text = readTextFile(jsonPath);
    CHECK(!text.contains(QStringLiteral("null")));
    CHECK(text.contains(QStringLiteral("\"decoderEvents\":[")));
    CHECK(text.contains(QStringLiteral("\"segments\":[")));
    CHECK(text.contains(QStringLiteral("\"rfSourceSampleRateHz\":")));
    CHECK(text.contains(QStringLiteral("\"pictureMetrics\":{")));
    CHECK(text.contains(QStringLiteral("\"burstStartLine\":7")));
    CHECK(text.contains(QStringLiteral("\"isDuplicateField\":true")));

    TbcMetaData copy;
    CHECK(copy.read(jsonPath));
    checkExtras(copy, 10);
    CHECK(copy.getField(7).burstStartLine == 7);
    CHECK(copy.getField(7).hasIsDuplicateField && copy.getField(7).isDuplicateField);
    CHECK(copy.getField(7).hasDetectedFirstField && !copy.getField(7).detectedFirstField);
    CHECK(!copy.getField(1).hasIsDuplicateField);

    // Once read from a file, write -> read -> write is byte-identical (the
    // first write of an in-memory object differs only in the library-derived
    // members read() fills in, such as the active line range)
    const QString jsonPath2 = dir.filePath(QStringLiteral("capture2.tbc.json"));
    CHECK(copy.write(jsonPath2));
    TbcMetaData copy2;
    CHECK(copy2.read(jsonPath2));
    checkExtras(copy2, 10);
    const QString jsonPath3 = dir.filePath(QStringLiteral("capture3.tbc.json"));
    CHECK(copy2.write(jsonPath3));
    CHECK(readTextFile(jsonPath3) == readTextFile(jsonPath2));

    // Without the new records the JSON still carries an empty decoderEvents
    // array (the capability marker) and no segments member
    TbcMetaData plain;
    buildMetadata(plain, 4, false);
    const QString plainPath = dir.filePath(QStringLiteral("plain.tbc.json"));
    CHECK(plain.write(plainPath));
    const QString plainText = readTextFile(plainPath);
    CHECK(plainText.contains(QStringLiteral("\"decoderEvents\":[]")));
    CHECK(!plainText.contains(QStringLiteral("\"segments\"")));
    CHECK(!plainText.contains(QStringLiteral("\"pictureMetrics\"")));
}

// A decoder-shaped JSON: fields last, unknown keys, null inside an unknown key
void testDecoderShapedJson()
{
    std::cerr << "Testing decoder-shaped JSON\n";
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("decoded.tbc.json"));
    {
        QFile file(jsonPath);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write(
            "{\"pcmAudioParameters\":{\"bits\":16,\"isLittleEndian\":true,\"isSigned\":true,\"sampleRate\":44100},\n"
            "\"videoParameters\":{\"numberOfSequentialFields\":2,\"osInfo\":\"Linux\",\"version\":\"vhs-decode 1.0\","
            "\"gitBranch\":\"main\",\"gitCommit\":\"deadbeef\",\"system\":\"NTSC\",\"fieldWidth\":910,\"sampleRate\":14318181.0,"
            "\"rfSourceSampleRateHz\":40000000.0,\"black16bIre\":16384,\"white16bIre\":54016,\"blanking16bIre\":16384,"
            "\"fieldHeight\":263,\"colourBurstStart\":78,\"colourBurstEnd\":110,\"activeVideoStart\":134,\"activeVideoEnd\":896,"
            "\"tapeFormat\":\"VHS\",\"futureKey\":{\"a\":null,\"b\":[null,1]}},\n"
            "\"decoderEvents\":[],\n"
            "\"fields\":[\n"
            "{\"isFirstField\":true,\"detectedFirstField\":true,\"isDuplicateField\":false,\"burstStartLine\":6,"
            "\"syncConf\":45,\"seqNo\":1,\"diskLoc\":0.0,\"fileLoc\":0,\"vitsMetrics\":{\"bPSNR\":40.1},"
            "\"pictureMetrics\":{\"lumaMeanIre\":31.5,\"noiseIre\":1.75},\"fieldPhaseID\":1,\"unknownPerField\":null},\n"
            "{\"isFirstField\":false,\"syncConf\":45,\"seqNo\":2,\"diskLoc\":1.0,\"fileLoc\":675840,"
            "\"vitsMetrics\":{\"bPSNR\":40.2},\"pictureMetrics\":{\"lumaMeanIre\":31.6,\"fieldDiffIre\":0.5,"
            "\"noiseIre\":1.8,\"blankingDevIre\":0.1,\"syncTipDevIre\":-0.2,\"burstAmpIre\":39.9},\"fieldPhaseID\":2}\n"
            "]}\n");
        file.close();
    }

    TbcMetaData metaData;
    CHECK(metaData.read(jsonPath));
    CHECK(metaData.getNumberOfFields() == 2);
    CHECK(nearly(metaData.getVideoParameters().rfSourceSampleRateHz, 40000000.0));
    CHECK(metaData.getVideoParameters().version == QStringLiteral("vhs-decode 1.0"));
    CHECK(metaData.getDecoderEvents().isEmpty());
    CHECK(metaData.getSegments().isEmpty());
    const TbcMetaData::PictureMetrics &m1 = metaData.getFieldPictureMetrics(1);
    CHECK(m1.inUse && nearly(m1.lumaMeanIre, 31.5) && nearly(m1.noiseIre, 1.75));
    CHECK(std::isnan(m1.fieldDiffIre) && std::isnan(m1.burstAmpIre));
    const TbcMetaData::PictureMetrics &m2 = metaData.getFieldPictureMetrics(2);
    CHECK(nearly(m2.burstAmpIre, 39.9) && nearly(m2.syncTipDevIre, -0.2));
    CHECK(metaData.getField(1).burstStartLine == 6);
    CHECK(metaData.getField(1).hasDetectedFirstField && metaData.getField(1).detectedFirstField);
}

// SQLite round trip: create, read, update, read; idempotent and lossless
void testSqliteRoundTrip()
{
    std::cerr << "Testing SQLite round trip\n";
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("capture.tbc.db"));

    TbcMetaData original;
    buildMetadata(original, 10, true);
    CHECK(original.write(dbPath));
    CHECK(queryInt(dbPath, QStringLiteral("PRAGMA user_version")) == 8);
    CHECK(queryString(dbPath, QStringLiteral("SELECT decoder FROM capture")) == QStringLiteral("vhs-decode"));
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM picture_metrics")) == 10);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM picture_metrics WHERE burst_amp_ire IS NULL")) == 5);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM decoder_event")) == 2);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM segment")) == 2);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM drop_outs")) == 3);
    CHECK(queryString(dbPath, QStringLiteral("SELECT file_loc FROM field_record WHERE field_id = 3"))
          == QString::number((static_cast<qint64>(1) << 33) + 5));

    TbcMetaData copy;
    CHECK(copy.read(dbPath));
    checkExtras(copy, 10, true);
    CHECK(copy.getVideoParameters().decoder == QStringLiteral("vhs-decode"));

    // Update in place: nothing duplicates, nothing relabels
    CHECK(copy.write(dbPath));
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM drop_outs")) == 3);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM decoder_event")) == 2);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM segment")) == 2);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM field_record")) == 10);
    CHECK(queryString(dbPath, QStringLiteral("SELECT decoder FROM capture")) == QStringLiteral("vhs-decode"));
    CHECK(queryInt(dbPath, QStringLiteral("PRAGMA user_version")) == 8);

    TbcMetaData again;
    CHECK(again.read(dbPath));
    checkExtras(again, 10, true);

    // An object that never said which decoder it came from must not relabel
    // an existing vhs-decode file either
    TbcMetaData anonymous;
    buildMetadata(anonymous, 10, true);
    TbcMetaData::VideoParameters vp = anonymous.getVideoParameters();
    vp.decoder.clear();
    anonymous.setVideoParameters(vp);
    CHECK(anonymous.write(dbPath));
    CHECK(queryString(dbPath, QStringLiteral("SELECT decoder FROM capture")) == QStringLiteral("vhs-decode"));
}

// Migration: a vhs-decode schema-version-1 file reads cleanly and upgrades in
// place on the first write without touching its rows
void testMigrationFromVhsDecodeV1()
{
    std::cerr << "Testing migration from a vhs-decode v1 database\n";
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("old.tbc.db"));
    createVhsDecodeV1Database(dbPath, 4);
    CHECK(queryInt(dbPath, QStringLiteral("PRAGMA user_version")) == 1);

    TbcMetaData metaData;
    CHECK(metaData.read(dbPath));
    CHECK(metaData.getNumberOfFields() == 4);
    CHECK(!metaData.hasPictureMetrics());
    CHECK(metaData.getDecoderEvents().isEmpty());
    CHECK(metaData.getSegments().isEmpty());
    CHECK(metaData.getVideoParameters().rfSourceSampleRateHz < 0.0);
    CHECK(metaData.getVideoParameters().decoder == QStringLiteral("vhs-decode"));
    CHECK(metaData.getField(3).dropOuts.size() == 1);

    // Add a segment and write back: the file migrates, the rows survive
    TbcMetaData::Segment clip;
    clip.startField = 0;
    clip.endFieldExclusive = 4;
    clip.kind = QStringLiteral("clip");
    metaData.appendSegment(clip);
    CHECK(metaData.write(dbPath));

    CHECK(queryInt(dbPath, QStringLiteral("PRAGMA user_version")) == 8);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                                          "('picture_metrics','decoder_event','segment')")) == 3);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('capture') "
                                          "WHERE name='rf_source_sample_rate_hz'")) == 1);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM field_record")) == 4);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM vits_metrics")) == 4);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM drop_outs")) == 1);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM segment")) == 1);
    CHECK(queryString(dbPath, QStringLiteral("SELECT decoder FROM capture")) == QStringLiteral("vhs-decode"));

    TbcMetaData migrated;
    CHECK(migrated.read(dbPath));
    CHECK(migrated.getSegments().size() == 1 && migrated.getSegments()[0].id == 1);
    CHECK(migrated.getField(3).dropOuts.size() == 1);
}

// Path helpers and the SQLite-first write
// A backfill only changes the picture metrics, the decoder events and the
// segments. Rewriting the field rows to persist them is what makes
// tbc-segments --write cost a full rewrite of the database (2.6 GB of churn
// for ~40 MB of metrics on a long capture, hours of it over NFS), so a scoped
// write has to leave those rows strictly alone.
void testScopedSqliteWrite()
{
    std::cerr << "Testing scoped SQLite writes\n";
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("scoped.tbc.db"));

    TbcMetaData original;
    buildMetadata(original, 8, true);
    CHECK(original.write(dbPath));
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM field_record")) == 8);

    // Mark a field row on disk with a value the in-memory object does not have.
    // A scoped write that excludes the field rows must not touch it.
    execSql(dbPath, QStringLiteral("UPDATE field_record SET sync_conf = 4242 WHERE field_id = 3"));
    CHECK(queryInt(dbPath, QStringLiteral("SELECT sync_conf FROM field_record WHERE field_id = 3")) == 4242);

    TbcMetaData loaded;
    CHECK(loaded.read(dbPath));
    TbcMetaData::PictureMetrics metrics;
    metrics.lumaMeanIre = 12.5;
    metrics.noiseIre = 3.25;
    loaded.updateFieldPictureMetrics(metrics, 4); // 1-based field number

    SqliteWriteScope scope;
    scope.fields = false;
    CHECK(loaded.writeWithProjection(dbPath, nullptr, scope));

    // The field row still carries the marker: it was never rewritten
    CHECK(queryInt(dbPath, QStringLiteral("SELECT sync_conf FROM field_record WHERE field_id = 3")) == 4242);
    // ...and the metrics did land
    CHECK(queryString(dbPath, QStringLiteral("SELECT luma_mean_ire FROM picture_metrics WHERE field_id = 3")).toDouble() == 12.5);

    // A full-scope write is still a full rewrite. "original" never saw the
    // marker, so writing it back puts the field row's own value (syncConf 45,
    // as buildMetadata sets it) over the top of it.
    CHECK(original.writeWithProjection(dbPath));
    CHECK(queryInt(dbPath, QStringLiteral("SELECT sync_conf FROM field_record WHERE field_id = 3")) == 45);
}

void testSqliteFirstPaths()
{
    std::cerr << "Testing SQLite-first path resolution and projection\n";
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("tape.tbc.json"));
    const QString dbPath = dir.filePath(QStringLiteral("tape.tbc.db"));

    CHECK(TbcMetaData::isJsonMetadataFilename(jsonPath));
    CHECK(TbcMetaData::isJsonMetadataFilename(jsonPath + QStringLiteral(".tmp")));
    CHECK(!TbcMetaData::isJsonMetadataFilename(dbPath));
    CHECK(TbcMetaData::sqliteSiblingPath(jsonPath) == dbPath);
    CHECK(TbcMetaData::sqliteSiblingPath(jsonPath + QStringLiteral(".tmp")) == dbPath);
    CHECK(TbcMetaData::sqliteSiblingPath(dbPath) == dbPath);

    // No database yet: the JSON is what there is
    CHECK(TbcMetaData::resolveMetadataPath(jsonPath) == jsonPath);

    // A JSON-only decode written SQLite-first: the database appears, the JSON
    // is rewritten as a projection, and from now on the database is what opens
    TbcMetaData original;
    buildMetadata(original, 6, true);
    CHECK(original.write(jsonPath));
    TbcMetaData loaded;
    CHECK(loaded.read(jsonPath));
    TbcMetaData::Segment clip;
    clip.startField = 0;
    clip.endFieldExclusive = 6;
    clip.kind = QStringLiteral("clip");
    clip.source = QStringLiteral("user");
    clip.title = QStringLiteral("Edited in a JSON-only world");
    loaded.setSegments(QVector<TbcMetaData::Segment>() << clip);
    QString canonical;
    CHECK(loaded.writeWithProjection(jsonPath, &canonical));
    CHECK(canonical == dbPath);
    CHECK(QFileInfo::exists(dbPath));
    CHECK(!QFileInfo::exists(jsonPath + QStringLiteral(".tmp")));
    CHECK(!QFileInfo::exists(jsonPath + QStringLiteral(".bup")));
    CHECK(TbcMetaData::resolveMetadataPath(jsonPath) == dbPath);
    CHECK(TbcMetaData::resolveMetadataPath(dbPath) == dbPath);
    CHECK(queryInt(dbPath, QStringLiteral("SELECT COUNT(*) FROM segment")) == 1);
    CHECK(readTextFile(jsonPath).contains(QStringLiteral("Edited in a JSON-only world")));

    // A second write is a no-op for the projection
    const QString before = readTextFile(jsonPath);
    TbcMetaData fromDb;
    CHECK(fromDb.read(TbcMetaData::resolveMetadataPath(jsonPath)));
    CHECK(fromDb.getSegments().size() == 1);
    CHECK(fromDb.writeWithProjection(dbPath));
    CHECK(readTextFile(jsonPath) == before);

    // Segment ids are stable and never reused
    TbcMetaData::Segment another;
    another.startField = 6;
    another.endFieldExclusive = 6;
    CHECK(fromDb.appendSegment(another) == 2);
    QVector<TbcMetaData::Segment> pruned = fromDb.getSegments();
    pruned.removeFirst();
    fromDb.setSegments(pruned);
    CHECK(fromDb.appendSegment(another) == 3);
}

int main(int argc, char *argv[])
{
    // Initialise Qt. The library's handler writes to stderr directly; Qt's
    // default handler sends qCritical to journald when stderr is not a
    // terminal, which hides a failing SQL statement from a ctest log.
    qInstallMessageHandler(debugOutputHandler);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("testmetadata");
    QCoreApplication::setOrganizationDomain("domesday86.com");

    // Set up the command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("testmetadata - unit tests for ld-decode's metadata library");
    parser.addHelpOption();
    parser.addVersionOption();

    // Test options
    QCommandLineOption statsOption(QStringList() << "s" << "stats",
                                   "parse all fields and show statistics");
    parser.addOption(statsOption);
    QCommandLineOption exitOption(QStringList() << "x" << "exit",
                                  "call exit(0) after parsing, to analyse memory usage");
    parser.addOption(exitOption);

    // Positional argument to specify input video file
    parser.addPositionalArgument("input", "Input SQLite file (omit to run unit tests)");

    // Positional argument to specify output video file
    parser.addPositionalArgument("output", "Output SQLite file (omit to only read input)");

    // Parse the command line
    parser.process(app);

    // Process the positional args
    QStringList positionalArguments = parser.positionalArguments();
    if (positionalArguments.count() == 0) {
        // Run unit tests (with the library's debug stream on, so a failing
        // SQL statement names itself)
        setDebug(true);
        testVideoSystem();
        testJsonRoundTrip();
        testDecoderShapedJson();
        testSqliteRoundTrip();
        testMigrationFromVhsDecodeV1();
        testSqliteFirstPaths();
        testScopedSqliteWrite();
        std::cout << "testmetadata: all checks passed\n";
        return 0;
    }
    if (positionalArguments.count() > 2) {
        qCritical("You may specify one input file and (optionally) one output file");
        return 1;
    }

    // Read the input file
    TbcMetaData metaData;
    if (!metaData.read(positionalArguments.at(0))) {
        qCritical("Unable to read input file");
        return 1;
    }

    // Show statistics
    if (parser.isSet(statsOption)) {
        // These are not very useful, but they ensure that TbcMetaData has
        // actually fully parsed all of the metadata.

        qint32 numFields = metaData.getNumberOfFields();
        qint32 numMetrics = 0;
        double meanWSNR = 0.0;
        qint32 numDropOuts = 0;

        for (qint32 i = 1; i <= numFields; i++) {
            const TbcMetaData::Field &field = metaData.getField(i);

            if (field.vitsMetrics.inUse) {
                ++numMetrics;
                meanWSNR += field.vitsMetrics.wSNR;
            }

            numDropOuts += field.dropOuts.size();
        }

        if (numMetrics > 0) meanWSNR /= numMetrics;

        std::cout << "fields=" << numFields << " metrics=" << numMetrics << " wSNR=" << meanWSNR << " dropouts=" << numDropOuts << "\n";
    }

    // Force an exit if requested
    if (parser.isSet(exitOption)) {
        // We use the C exit function so that C++ destructors don't get called.
        // We can then use a memory debugger like valgrind to analyse the
        // memory that was "leaked", and see how much the parser allocated.
        exit(0);
    }

    // Write the output file, if given
    if (positionalArguments.count() == 2) {
        if (!metaData.write(positionalArguments.at(1))) {
            qCritical("Unable to write output file");
            return 1;
        }
    }

    return 0;
}