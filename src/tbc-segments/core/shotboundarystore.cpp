/************************************************************************

    shotboundarystore.cpp

    tbc-segments - Shot boundaries in the .tbc.db (the module's own tables)
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

#include "shotboundarystore.h"

#include <QAtomicInteger>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "tbcmetadata.h"

namespace {

// No semicolons inside these statements' comments: they run one at a time.
const QStringList kSchema = {
    QStringLiteral("CREATE TABLE IF NOT EXISTS tbc_segments_meta ("
                   " key TEXT PRIMARY KEY,"
                   " value TEXT NOT NULL)"),
    QStringLiteral("CREATE TABLE IF NOT EXISTS shot_boundary ("
                   " shot_boundary_id INTEGER PRIMARY KEY,"
                   " capture_id INTEGER NOT NULL,"
                   " field INTEGER NOT NULL,"
                   " evidence TEXT NOT NULL,"
                   " source TEXT NOT NULL,"
                   " verdict TEXT NOT NULL CHECK (verdict IN ('detected','confirmed','rejected')),"
                   " scene_change INTEGER NOT NULL DEFAULT 0 CHECK (scene_change IN (0,1)),"
                   " scene_title TEXT,"
                   " scene_enabled INTEGER NOT NULL DEFAULT 1 CHECK (scene_enabled IN (0,1)),"
                   " shot_enabled INTEGER NOT NULL DEFAULT 1 CHECK (shot_enabled IN (0,1)),"
                   " confidence REAL,"
                   " reason TEXT,"
                   " detail_json TEXT,"
                   " comment TEXT,"
                   " created_by TEXT,"
                   " updated_at TEXT,"
                   " derived_from TEXT)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS shot_boundary_field ON shot_boundary(capture_id, field)"),
    QStringLiteral("INSERT OR IGNORE INTO tbc_segments_meta (key, value) VALUES ('shot_boundary_schema', '1')"),
    // glossary-legacy: 3.3.1-gdh-1.1 stored its detections as segment_mark rows
    QStringLiteral("DROP TABLE IF EXISTS segment_mark"), // glossary-legacy
    QStringLiteral("DELETE FROM tbc_segments_meta WHERE key = 'marks_schema'"), // glossary-legacy
};

// One connection per call, closed and removed before the call returns
class Connection
{
public:
    explicit Connection(const QString &path)
    {
        static QAtomicInteger<quint64> serial;
        name_ = QStringLiteral("tbc_segments_shotboundarystore_%1").arg(serial.fetchAndAddRelaxed(1));
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        db.setDatabaseName(path);
        ok_ = db.open();
        if (!ok_) error_ = db.lastError().text();
    }
    ~Connection()
    {
        {
            QSqlDatabase db = QSqlDatabase::database(name_, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(name_);
    }
    QSqlDatabase db() const { return QSqlDatabase::database(name_, false); }
    bool ok() const { return ok_; }
    QString error() const { return error_; }

private:
    QString name_;
    bool ok_ = false;
    QString error_;
};

bool hasTable(QSqlDatabase db, const QString &name)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?"));
    q.addBindValue(name);
    return q.exec() && q.next();
}

bool readRows(QSqlDatabase db, QVector<ShotBoundary> *boundaries, QString *error)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT shot_boundary_id, field, evidence, source, verdict, scene_change, scene_title,"
                               " scene_enabled, shot_enabled, confidence, reason, detail_json, comment, created_by,"
                               " updated_at, derived_from FROM shot_boundary ORDER BY field, shot_boundary_id"))) {
        if (error) *error = q.lastError().text();
        return false;
    }
    while (q.next()) {
        ShotBoundary boundary;
        boundary.id = q.value(0).toInt();
        boundary.field = q.value(1).toInt();
        boundary.evidence = q.value(2).toString();
        boundary.source = q.value(3).toString();
        boundary.verdict = q.value(4).toString();
        boundary.sceneChange = q.value(5).toInt() == 1;
        boundary.sceneTitle = q.value(6).toString();
        boundary.sceneEnabled = q.value(7).toInt() == 1;
        boundary.shotEnabled = q.value(8).toInt() == 1;
        boundary.confidence = q.value(9).isNull() ? 0.0 : q.value(9).toDouble();
        boundary.reason = q.value(10).toString();
        boundary.detailJson = q.value(11).toString();
        boundary.comment = q.value(12).toString();
        boundary.createdBy = q.value(13).toString();
        boundary.updatedAt = q.value(14).toString();
        boundary.derivedFrom = q.value(15).toString();
        boundaries->append(boundary);
    }
    return true;
}

} // namespace

namespace ShotBoundaryStore {

QString databasePathFor(const QString &metadataPath)
{
    if (metadataPath.isEmpty()) return QString();
    const QString candidate = TbcMetaData::isJsonMetadataFilename(metadataPath)
                                  ? TbcMetaData::sqliteSiblingPath(metadataPath)
                                  : metadataPath;
    return QFileInfo::exists(candidate) ? candidate : QString();
}

bool read(const QString &databasePath, QVector<ShotBoundary> *boundaries, bool *tableExists, QString *error)
{
    if (tableExists) *tableExists = false;
    if (!boundaries) return false;
    boundaries->clear();
    if (databasePath.isEmpty() || !QFileInfo::exists(databasePath)) return true;
    Connection connection(databasePath);
    if (!connection.ok()) {
        if (error) *error = connection.error();
        return false;
    }
    if (!hasTable(connection.db(), QStringLiteral("shot_boundary"))) return true;
    if (tableExists) *tableExists = true;
    return readRows(connection.db(), boundaries, error);
}

bool write(const QString &databasePath, const QVector<ShotBoundary> &boundaries, bool *changed, QString *error)
{
    if (changed) *changed = false;
    if (databasePath.isEmpty() || !QFileInfo::exists(databasePath)) {
        if (error) *error = QStringLiteral("no .tbc.db to store shot boundaries in");
        return false;
    }
    Connection connection(databasePath);
    if (!connection.ok()) {
        if (error) *error = connection.error();
        return false;
    }
    QSqlDatabase db = connection.db();

    const bool legacy = hasTable(db, QStringLiteral("segment_mark")); // glossary-legacy: dropped below
    if (hasTable(db, QStringLiteral("shot_boundary")) && !legacy) {
        QVector<ShotBoundary> stored;
        if (readRows(db, &stored, error) && stored == boundaries) return true;
    } else if (boundaries.isEmpty() && !legacy) {
        return true;   // nothing stored, nothing to store: leave the file alone
    }

    if (!db.transaction()) {
        if (error) *error = db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    for (const QString &statement : kSchema) {
        if (!q.exec(statement)) {
            if (error) *error = q.lastError().text();
            db.rollback();
            return false;
        }
    }
    qint32 captureId = 1;
    if (q.exec(QStringLiteral("SELECT capture_id FROM capture ORDER BY capture_id LIMIT 1")) && q.next()) {
        captureId = q.value(0).toInt();
    }
    if (!q.exec(QStringLiteral("DELETE FROM shot_boundary"))) {
        if (error) *error = q.lastError().text();
        db.rollback();
        return false;
    }
    q.prepare(QStringLiteral("INSERT INTO shot_boundary (shot_boundary_id, capture_id, field, evidence, source, verdict,"
                             " scene_change, scene_title, scene_enabled, shot_enabled, confidence, reason, detail_json,"
                             " comment, created_by, updated_at, derived_from)"
                             " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const ShotBoundary &boundary : boundaries) {
        q.addBindValue(boundary.id);
        q.addBindValue(captureId);
        q.addBindValue(boundary.field);
        q.addBindValue(boundary.evidence);
        q.addBindValue(boundary.source);
        q.addBindValue(boundary.verdict);
        q.addBindValue(boundary.sceneChange ? 1 : 0);
        q.addBindValue(boundary.sceneTitle);
        q.addBindValue(boundary.sceneEnabled ? 1 : 0);
        q.addBindValue(boundary.shotEnabled ? 1 : 0);
        q.addBindValue(boundary.confidence);
        q.addBindValue(boundary.reason);
        q.addBindValue(boundary.detailJson);
        q.addBindValue(boundary.comment);
        q.addBindValue(boundary.createdBy);
        q.addBindValue(boundary.updatedAt);
        q.addBindValue(boundary.derivedFrom);
        if (!q.exec()) {
            if (error) *error = q.lastError().text();
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        if (error) *error = db.lastError().text();
        return false;
    }
    if (changed) *changed = true;
    return true;
}

QString sidecarPathFor(const QString &jsonPath)
{
    return jsonPath + QStringLiteral(".shot-boundaries.json");
}

bool writeSidecar(const QString &jsonPath, const QVector<ShotBoundary> &boundaries, QString *error)
{
    QJsonArray array;
    for (const ShotBoundary &b : boundaries) {
        QJsonObject o;
        o.insert("id", b.id);
        o.insert("field", b.field);
        o.insert("evidence", b.evidence);
        o.insert("source", b.source);
        o.insert("verdict", b.verdict);
        o.insert("sceneChange", b.sceneChange);
        o.insert("sceneTitle", b.sceneTitle);
        o.insert("sceneEnabled", b.sceneEnabled);
        o.insert("shotEnabled", b.shotEnabled);
        o.insert("confidence", b.confidence);
        o.insert("reason", b.reason);
        o.insert("detailJson", b.detailJson);
        o.insert("comment", b.comment);
        o.insert("createdBy", b.createdBy);
        o.insert("updatedAt", b.updatedAt);
        o.insert("derivedFrom", b.derivedFrom);
        array.append(o);
    }
    QSaveFile file(sidecarPathFor(jsonPath));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{"shotBoundaries", array}}).toJson(QJsonDocument::Compact));
    return file.commit();
}

bool readSidecar(const QString &jsonPath, QVector<ShotBoundary> *boundaries)
{
    if (!boundaries) return false;
    boundaries->clear();
    QFile file(sidecarPathFor(jsonPath));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).object().value("shotBoundaries").toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject o = value.toObject();
        ShotBoundary b;
        b.id = o.value("id").toInt();
        b.field = o.value("field").toInt();
        b.evidence = o.value("evidence").toString();
        b.source = o.value("source").toString();
        b.verdict = o.value("verdict").toString();
        b.sceneChange = o.value("sceneChange").toBool();
        b.sceneTitle = o.value("sceneTitle").toString();
        b.sceneEnabled = o.value("sceneEnabled").toBool(true);
        b.shotEnabled = o.value("shotEnabled").toBool(true);
        b.confidence = o.value("confidence").toDouble();
        b.reason = o.value("reason").toString();
        b.detailJson = o.value("detailJson").toString();
        b.comment = o.value("comment").toString();
        b.createdBy = o.value("createdBy").toString();
        b.updatedAt = o.value("updatedAt").toString();
        b.derivedFrom = o.value("derivedFrom").toString();
        boundaries->append(b);
    }
    return true;
}

} // namespace ShotBoundaryStore
