/**
 * @file HBBlockingDB.cpp
 * @brief HB ACK rate-limiting database implementation.
 */

#include "HBBlockingDB.h"
#include "vendor/sqlite3/sqlite3.h"
#include "DriftingDateTime.h"

#include <QTimeZone>

namespace {
constexpr char ALLCALL_REPLY_SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS allcall_reply_v1 ("
    "  callsign VARCHAR(255) PRIMARY KEY, "
    "  last_seen TEXT"
    ");";
} // namespace

HBBlockingDB::HBBlockingDB(const QString &path)
    : path_{path}, db_{nullptr} {}

HBBlockingDB::~HBBlockingDB() { close(); }

bool HBBlockingDB::open() {
    int rc = sqlite3_open(path_.toLocal8Bit().data(), &db_);
    if (rc != SQLITE_OK) {
        close();
        return false;
    }

    rc = sqlite3_exec(db_, ALLCALL_REPLY_SCHEMA, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        close();
        return false;
    }

    return true;
}

void HBBlockingDB::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool HBBlockingDB::isOpen() const { return db_ != nullptr; }

QString HBBlockingDB::error() const {
    if (db_) {
        return QString::fromLocal8Bit(sqlite3_errmsg(db_));
    }
    return {};
}

bool HBBlockingDB::upsertAllcallReplyTimestamp(const QString &callsign,
                                               const QDateTime &ts) {
    if (!isOpen()) return false;

    const char *sql =
        "INSERT INTO allcall_reply_v1 (callsign, last_seen) VALUES (?, ?) "
        "ON CONFLICT(callsign) DO UPDATE SET last_seen = excluded.last_seen;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    auto c8 = callsign.toLocal8Bit();
    auto t8 = ts.toUTC().toString("yyyy-MM-dd HH:mm:ss").toLocal8Bit();
    sqlite3_bind_text(stmt, 1, c8.data(), -1, nullptr);
    sqlite3_bind_text(stmt, 2, t8.data(), -1, nullptr);
    sqlite3_step(stmt);

    return sqlite3_finalize(stmt) == SQLITE_OK;
}

QDateTime HBBlockingDB::getAllcallReplyTimestamp(const QString &callsign) {
    if (!isOpen()) return {};

    const char *sql =
        "SELECT last_seen FROM allcall_reply_v1 WHERE callsign = ? LIMIT 1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    auto c8 = callsign.toLocal8Bit();
    sqlite3_bind_text(stmt, 1, c8.data(), -1, nullptr);

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

// defined here but not presently used. The database storage is persistant
bool HBBlockingDB::deleteAllcallReplyTimestamp(const QString &callsign) {
    if (!isOpen()) return false;

    const char *sql = "DELETE FROM allcall_reply_v1 WHERE callsign = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    auto c8 = callsign.toLocal8Bit();
    sqlite3_bind_text(stmt, 1, c8.data(), -1, nullptr);
    sqlite3_step(stmt);

    return sqlite3_finalize(stmt) == SQLITE_OK;
}
