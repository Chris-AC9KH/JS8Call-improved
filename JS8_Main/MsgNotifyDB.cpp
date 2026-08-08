/**
 * @file MsgNotifyDB.cpp
 * @brief Stored-message notification tracking database implementation.
 */

#include "MsgNotifyDB.h"
#include "vendor/sqlite3/sqlite3.h"
#include "DriftingDateTime.h"

#include <QTimeZone>

namespace {
constexpr char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS msg_notify_v1 ("
    "  msg_id    INTEGER PRIMARY KEY, "
    "  callsign  VARCHAR(255), "
    "  last_sent TEXT"
    ");";
} // namespace

MsgNotifyDB::MsgNotifyDB(const QString &path)
    : path_{path}, db_{nullptr} {}

MsgNotifyDB::~MsgNotifyDB() { close(); }

bool MsgNotifyDB::open() {
    int rc = sqlite3_open(path_.toLocal8Bit().data(), &db_);
    if (rc != SQLITE_OK) {
        close();
        return false;
    }

    rc = sqlite3_exec(db_, SCHEMA, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        close();
        return false;
    }

    return true;
}

void MsgNotifyDB::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MsgNotifyDB::isOpen() const { return db_ != nullptr; }

QString MsgNotifyDB::error() const {
    if (db_) {
        return QString::fromLocal8Bit(sqlite3_errmsg(db_));
    }
    return {};
}

bool MsgNotifyDB::upsertSent(int msgId, const QString &callsign,
                             const QDateTime &ts) {
    if (!isOpen()) return false;

    const char *sql =
        "INSERT INTO msg_notify_v1 (msg_id, callsign, last_sent) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(msg_id) DO UPDATE SET "
        "  callsign = excluded.callsign, "
        "  last_sent = excluded.last_sent;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    auto c8 = callsign.toLocal8Bit();
    auto t8 = ts.toUTC().toString("yyyy-MM-dd HH:mm:ss").toLocal8Bit();
    sqlite3_bind_int(stmt, 1, msgId);
    sqlite3_bind_text(stmt, 2, c8.data(), -1, nullptr);
    sqlite3_bind_text(stmt, 3, t8.data(), -1, nullptr);
    sqlite3_step(stmt);

    return sqlite3_finalize(stmt) == SQLITE_OK;
}

QDateTime MsgNotifyDB::getLastSent(int msgId) {
    if (!isOpen()) return {};

    const char *sql =
        "SELECT last_sent FROM msg_notify_v1 WHERE msg_id = ? LIMIT 1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    sqlite3_bind_int(stmt, 1, msgId);

    QDateTime result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto raw = QByteArray(
            (const char *)sqlite3_column_text(stmt, 0),
            sqlite3_column_bytes(stmt, 0));
        result = QDateTime::fromString(
            QString::fromUtf8(raw), "yyyy-MM-dd HH:mm:ss");
        result.setTimeZone(QTimeZone::utc());
    }

    sqlite3_finalize(stmt);
    return result;
}

bool MsgNotifyDB::deleteByMsgId(int msgId) {
    if (!isOpen()) return false;

    const char *sql = "DELETE FROM msg_notify_v1 WHERE msg_id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, msgId);
    sqlite3_step(stmt);

    return sqlite3_finalize(stmt) == SQLITE_OK;
}
