/************************************************************************

    markstore.cpp

    tbc-segments - Restart marks in the .tbc.db (the module's own tables)
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

#include "markstore.h"

#include <QAtomicInteger>
#include <QFileInfo>
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
    QStringLiteral("CREATE TABLE IF NOT EXISTS segment_mark ("
                   " mark_id INTEGER PRIMARY KEY,"
                   " capture_id INTEGER NOT NULL,"
                   " field INTEGER NOT NULL,"
                   " kind TEXT NOT NULL,"
                   " source TEXT NOT NULL,"
                   " enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0,1)),"
                   " split INTEGER NOT NULL DEFAULT 0 CHECK (split IN (0,1)),"
                   " confidence REAL,"
                   " reason TEXT,"
                   " detail_json TEXT,"
                   " comment TEXT,"
                   " created_by TEXT,"
                   " updated_at TEXT,"
                   " derived_from TEXT)"),
    QStringLiteral("CREATE INDEX IF NOT EXISTS segment_mark_field ON segment_mark(capture_id, field)"),
    QStringLiteral("INSERT OR IGNORE INTO tbc_segments_meta (key, value) VALUES ('marks_schema', '1')"),
};

// One connection per call, closed and removed before the call returns
class Connection
{
public:
    explicit Connection(const QString &path)
    {
        static QAtomicInteger<quint64> serial;
        name_ = QStringLiteral("tbc_segments_markstore_%1").arg(serial.fetchAndAddRelaxed(1));
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

bool hasMarkTable(QSqlDatabase db)
{
    QSqlQuery q(db);
    return q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'segment_mark'")) && q.next();
}

bool readRows(QSqlDatabase db, QVector<SegmentMark> *marks, QString *error)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT mark_id, field, kind, source, enabled, split, confidence, reason, detail_json,"
                               " comment, created_by, updated_at, derived_from FROM segment_mark ORDER BY field, mark_id"))) {
        if (error) *error = q.lastError().text();
        return false;
    }
    while (q.next()) {
        SegmentMark mark;
        mark.id = q.value(0).toInt();
        mark.field = q.value(1).toInt();
        mark.kind = q.value(2).toString();
        mark.source = q.value(3).toString();
        mark.enabled = q.value(4).toInt() == 1;
        mark.split = q.value(5).toInt() == 1;
        mark.confidence = q.value(6).isNull() ? 0.0 : q.value(6).toDouble();
        mark.reason = q.value(7).toString();
        mark.detailJson = q.value(8).toString();
        mark.comment = q.value(9).toString();
        mark.createdBy = q.value(10).toString();
        mark.updatedAt = q.value(11).toString();
        mark.derivedFrom = q.value(12).toString();
        marks->append(mark);
    }
    return true;
}

} // namespace

namespace MarkStore {

QString databasePathFor(const QString &metadataPath)
{
    if (metadataPath.isEmpty()) return QString();
    const QString candidate = TbcMetaData::isJsonMetadataFilename(metadataPath)
                                  ? TbcMetaData::sqliteSiblingPath(metadataPath)
                                  : metadataPath;
    return QFileInfo::exists(candidate) ? candidate : QString();
}

bool read(const QString &databasePath, QVector<SegmentMark> *marks, bool *hasTable, QString *error)
{
    if (hasTable) *hasTable = false;
    if (!marks) return false;
    marks->clear();
    if (databasePath.isEmpty() || !QFileInfo::exists(databasePath)) return true;
    Connection connection(databasePath);
    if (!connection.ok()) {
        if (error) *error = connection.error();
        return false;
    }
    if (!hasMarkTable(connection.db())) return true;
    if (hasTable) *hasTable = true;
    return readRows(connection.db(), marks, error);
}

bool write(const QString &databasePath, const QVector<SegmentMark> &marks, bool *changed, QString *error)
{
    if (changed) *changed = false;
    if (databasePath.isEmpty() || !QFileInfo::exists(databasePath)) {
        if (error) *error = QStringLiteral("no .tbc.db to store marks in");
        return false;
    }
    Connection connection(databasePath);
    if (!connection.ok()) {
        if (error) *error = connection.error();
        return false;
    }
    QSqlDatabase db = connection.db();

    if (hasMarkTable(db)) {
        QVector<SegmentMark> stored;
        if (readRows(db, &stored, error) && stored == marks) return true;
    } else if (marks.isEmpty()) {
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
    if (!q.exec(QStringLiteral("DELETE FROM segment_mark"))) {
        if (error) *error = q.lastError().text();
        db.rollback();
        return false;
    }
    q.prepare(QStringLiteral("INSERT INTO segment_mark (mark_id, capture_id, field, kind, source, enabled, split, confidence,"
                             " reason, detail_json, comment, created_by, updated_at, derived_from)"
                             " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const SegmentMark &mark : marks) {
        q.addBindValue(mark.id);
        q.addBindValue(captureId);
        q.addBindValue(mark.field);
        q.addBindValue(mark.kind);
        q.addBindValue(mark.source);
        q.addBindValue(mark.enabled ? 1 : 0);
        q.addBindValue(mark.split ? 1 : 0);
        q.addBindValue(mark.confidence);
        q.addBindValue(mark.reason);
        q.addBindValue(mark.detailJson);
        q.addBindValue(mark.comment);
        q.addBindValue(mark.createdBy);
        q.addBindValue(mark.updatedAt);
        q.addBindValue(mark.derivedFrom);
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

} // namespace MarkStore
