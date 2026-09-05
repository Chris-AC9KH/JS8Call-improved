/**
 * @file ActivityDB.cpp
 * @brief Persistent per-band activity storage implementation.
 */

#include "ActivityDB.h"
#include "vendor/sqlite3/sqlite3.h"

#include <QTimeZone>

namespace {

constexpr char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS call_activity_v1 ("
    "  config   TEXT NOT NULL, "
    "  band     TEXT NOT NULL, "
    "  callsign TEXT NOT NULL, "
    "  through  TEXT, "
    "  snr      INTEGER, "
    "  grid     TEXT, "
    "  dial     INTEGER, "
    "  offset   INTEGER, "
    "  bits     INTEGER, "
    "  tdrift   REAL, "
    "  cq_ts    TEXT NOT NULL DEFAULT '', "
    "  ack_ts   TEXT NOT NULL DEFAULT '', "
    "  utc_ts   TEXT NOT NULL DEFAULT '', "
    "  submode  INTEGER, "
    "  PRIMARY KEY(config, band, callsign)"
    ");"
    "CREATE TABLE IF NOT EXISTS rx_text_v1 ("
    "  config TEXT NOT NULL, "
    "  band   TEXT NOT NULL, "
    "  html   TEXT, "
    "  PRIMARY KEY(config, band)"
    ");"
    "CREATE TABLE IF NOT EXISTS legacy_import_v1 ("
    "  config TEXT NOT NULL PRIMARY KEY"
    ");";

// grid, cq_ts, ack_ts: keep the stored value when the incoming one is empty
// through: deliberately not guarded, so a direct hearing clears the relay
// WHERE: the update applies only to a row as fresh as the one stored
constexpr char UPSERT_CALL_SQL[] =
    "INSERT INTO call_activity_v1 "
    "  (config, band, callsign, through, snr, grid, dial, offset, "
    "   bits, tdrift, cq_ts, ack_ts, utc_ts, submode) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(config, band, callsign) DO UPDATE SET "
    "  through = excluded.through, "
    "  snr = excluded.snr, "
    "  grid = CASE WHEN excluded.grid <> '' "
    "         THEN excluded.grid ELSE call_activity_v1.grid END, "
    "  dial = excluded.dial, "
    "  offset = excluded.offset, "
    "  bits = excluded.bits, "
    "  tdrift = excluded.tdrift, "
    "  cq_ts = CASE WHEN excluded.cq_ts <> '' "
    "          THEN excluded.cq_ts ELSE call_activity_v1.cq_ts END, "
    "  ack_ts = CASE WHEN excluded.ack_ts <> '' "
    "           THEN excluded.ack_ts ELSE call_activity_v1.ack_ts END, "
    "  utc_ts = CASE WHEN excluded.utc_ts <> '' "
    "           THEN excluded.utc_ts ELSE call_activity_v1.utc_ts END, "
    "  submode = excluded.submode "
    "WHERE excluded.utc_ts >= call_activity_v1.utc_ts;";

constexpr char SAVE_RX_TEXT_SQL[] =
    "INSERT INTO rx_text_v1 (config, band, html) VALUES (?, ?, ?) "
    "ON CONFLICT(config, band) DO UPDATE SET html = excluded.html;";

constexpr char TS_FORMAT[] = "yyyy-MM-dd HH:mm:ss";

QByteArray toTs(const QDateTime &dt) {
    if (!dt.isValid()) return {};
    return dt.toUTC().toString(TS_FORMAT).toUtf8();
}

/**
 * @brief Parse a stored timestamp back into a UTC QDateTime.
 * @param stmt The statement being read.
 * @param col The column holding the timestamp text.
 * @return The value, or an invalid QDateTime when the text is empty or
 *         does not parse.
 *
 * The value is constructed directly in UTC, because parsing a full
 * QDateTime first would normalize through local time - shifting a value
 * that falls in a DST gap - before the zone could be corrected. Text that
 * does not parse yields an invalid QDateTime rather than the midnight
 * QDateTime substitutes, which would turn corrupt text into a
 * plausible-looking timestamp.
 */
QDateTime fromTs(sqlite3_stmt *stmt, int col) {
    auto raw = QByteArray((const char *)sqlite3_column_text(stmt, col),
                          sqlite3_column_bytes(stmt, col));
    if (raw.isEmpty()) return {};
    auto const s = QString::fromUtf8(raw);
    auto const date = QDate::fromString(s.left(10), "yyyy-MM-dd");
    auto const time = QTime::fromString(s.mid(11), "HH:mm:ss");
    if (!date.isValid() || !time.isValid()) {
        return {};
    }
    return QDateTime(date, time, QTimeZone::utc());
}

} // namespace

ActivityDB::ActivityDB(const QString &path)
    : path_{path}, db_{nullptr}, inTransaction_{false},
      pendingClose_{false}, consecutiveFailures_{0},
      upsertCallStmt_{nullptr}, saveRxTextStmt_{nullptr} {}

ActivityDB::~ActivityDB() { close(); }

void ActivityDB::captureError() {
    if (db_) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
    }
}

/**
 * @brief Record the outcome of one statement and count failures.
 * @param ok Whether the statement completed.
 * @return ok unchanged, so a caller can return it directly.
 *
 * A read or write that failed is counted; enough consecutive failures (a
 * volume that vanished mid-session, creeping corruption) close the handle
 * so isOpen() finally reports the truth - the UI's degraded-mode paths
 * and its throttled reopen retry key off isOpen(), and would otherwise be
 * unreachable for a store that breaks after a good open.
 *
 * Only a successful data statement proves the handle usable again, so
 * only that path drops the strike count and any close deferred earlier in
 * the batch. commit() does not reach here at all when its COMMIT
 * succeeds, because a COMMIT succeeding says nothing about a batch whose
 * every data statement failed.
 *
 * On failure the error is captured here, immediately after the failing
 * statement, while sqlite3_errmsg() still describes it.
 */
bool ActivityDB::noteResult(bool ok) {
    if (ok) {
        consecutiveFailures_ = 0;
        pendingClose_ = false;
    } else {
        captureError();
        noteFailureCaptured();
    }
    return ok;
}

/**
 * @brief Count a failure whose error message was already captured.
 *
 * Like noteResult(false), for failures whose error was captured before
 * follow-up statements - a rollback - reset sqlite3_errmsg() to "not an
 * error". Inside a transaction the self-close is deferred to commit() or
 * rollback(), because sqlite3_close() would roll the open transaction
 * back and silently discard every row already written in the batch.
 */
void ActivityDB::noteFailureCaptured() {
    if (++consecutiveFailures_ >= 3) {
        if (inTransaction_) {
            pendingClose_ = true;
        } else {
            close();
        }
    }
}

/**
 * @brief Open the store, creating or validating its schema.
 * @return True when the handle is usable.
 *
 * WAL keeps an interrupted write from corrupting the store, and with
 * synchronous=NORMAL commits are not individually fsynced, so frequent
 * small writes stay cheap on the GUI thread while remaining safe against
 * application crashes - an OS crash can lose, but not corrupt, the most
 * recent commits. NORMAL carries that guarantee only under WAL, and WAL
 * can be refused (filesystems without shared-memory support, such as
 * network homes), so the sync level is relaxed only after checking what
 * actually took effect. The busy timeout is set before that probe: the
 * first WAL conversion takes a lock another connection may briefly hold,
 * and probing without the timeout would silently leave the session on
 * the rollback journal.
 *
 * The schema is created in one transaction, so a power cut cannot leave
 * it half-built. Preparing the hot-path statements here doubles as a
 * schema validation probe: CREATE TABLE IF NOT EXISTS silently accepts a
 * pre-existing table with different columns, say after a version
 * downgrade, and without this check every later write would fail with its
 * return value unexamined - the application would look normal all session
 * while persisting nothing.
 */
bool ActivityDB::open() {
    // sqlite3_open needs UTF-8: toLocal8Bit breaks non-ASCII win32 paths
    int rc = sqlite3_open(path_.toUtf8().data(), &db_);
    if (rc != SQLITE_OK) {
        captureError();
        close();
        return false;
    }

    sqlite3_exec(db_, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
    bool wal = false;
    for (int attempt = 0; attempt < 2 && !wal; ++attempt) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA journal_mode=WAL;", -1, &stmt,
                               nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                wal = qstricmp((const char *)sqlite3_column_text(stmt, 0),
                               "wal") == 0;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (wal) {
        sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr,
                     nullptr);
    }

    rc = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    if (rc == SQLITE_OK) {
        rc = sqlite3_exec(db_, SCHEMA, nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK) {
            rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
            if (rc != SQLITE_OK) captureError();
        } else {
            // capture before the rollback: a good ROLLBACK clears errmsg
            captureError();
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    } else {
        captureError();
    }
    if (rc != SQLITE_OK) {
        close();
        return false;
    }

    if (sqlite3_prepare_v2(db_, UPSERT_CALL_SQL, -1, &upsertCallStmt_,
                           nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, SAVE_RX_TEXT_SQL, -1, &saveRxTextStmt_,
                           nullptr) != SQLITE_OK) {
        captureError();
        close();
        return false;
    }

    consecutiveFailures_ = 0;
    return true;
}

void ActivityDB::close() {
    if (upsertCallStmt_) {
        sqlite3_finalize(upsertCallStmt_);
        upsertCallStmt_ = nullptr;
    }
    if (saveRxTextStmt_) {
        sqlite3_finalize(saveRxTextStmt_);
        saveRxTextStmt_ = nullptr;
    }
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
    inTransaction_ = false;
    pendingClose_ = false;
}

bool ActivityDB::isOpen() const { return db_ != nullptr; }

QString ActivityDB::error() const { return lastError_; }

bool ActivityDB::begin() {
    if (!isOpen() || inTransaction_) return false;
    // IMMEDIATE: take the write lock where the busy timeout still applies
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
        return noteResult(false);
    }
    inTransaction_ = true;
    return true;
}

/**
 * @brief Commit the open transaction.
 * @return True when the COMMIT succeeded.
 *
 * A successful COMMIT is not evidence that the store recovered: only a
 * successful data statement is, and any of those has already cleared both
 * the strike count and any deferred close. This path therefore does not
 * go through noteResult() at all - resetting the strikes here would let
 * an all-failing batch that commits cleanly hold the counter at zero
 * forever, and the handle would never give up.
 *
 * A failed COMMIT (SQLITE_BUSY on the non-WAL fallback's lock upgrade,
 * say) leaves the transaction open, so it is explicitly rolled back:
 * without that, every later autocommit write would silently join the open
 * transaction and be lost wholesale when the handle closes. It also
 * counts as a failure strike, because the successful steps inside a
 * doomed transaction reset the counter and a store whose commits
 * persistently fail would otherwise never report unusable.
 */
bool ActivityDB::commit() {
    if (!isOpen() || !inTransaction_) return false;
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) ==
        SQLITE_OK) {
        inTransaction_ = false;
        if (pendingClose_) {
            // strikes accumulated mid-batch and nothing since succeeded;
            // the commit above kept whatever the batch did write
            pendingClose_ = false;
            close();
        }
        return true;
    }
    captureError();
    rollback();
    noteFailureCaptured();
    return false;
}

void ActivityDB::rollback() {
    if (!isOpen() || !inTransaction_) return;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    inTransaction_ = false;
    if (pendingClose_) {
        pendingClose_ = false;
        close();
    }
}

bool ActivityDB::upsertCall(const QString &config, const QString &band,
                            const CallRecord &record) {
    if (!isOpen() || !upsertCallStmt_) return false;

    auto *stmt = upsertCallStmt_;
    sqlite3_reset(stmt);

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    auto call8 = record.callsign.toUtf8();
    auto through8 = record.through.toUtf8();
    auto grid8 = record.grid.toUtf8();
    auto cq8 = toTs(record.cqTimestamp);
    auto ack8 = toTs(record.ackTimestamp);
    auto utc8 = toTs(record.utcTimestamp);

    // a failed bind leaves utc_ts NULL, which no freshness guard can meet
    int bindRc = SQLITE_OK;
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 3, call8.data(), call8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 4, through8.data(), through8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_int(stmt, 5, record.snr);
    bindRc |= sqlite3_bind_text(stmt, 6, grid8.data(), grid8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_int64(stmt, 7, (sqlite3_int64)record.dial);
    bindRc |= sqlite3_bind_int(stmt, 8, record.offset);
    bindRc |= sqlite3_bind_int(stmt, 9, record.bits);
    bindRc |= sqlite3_bind_double(stmt, 10, record.tdrift);
    bindRc |= sqlite3_bind_text(stmt, 11, cq8.data(), cq8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 12, ack8.data(), ack8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 13, utc8.data(), utc8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_int(stmt, 14, record.submode);
    if (bindRc != SQLITE_OK) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return noteResult(false);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return noteResult(ok);
}

bool ActivityDB::deleteCall(const QString &config, const QString &band,
                            const QString &callsign) {
    if (!isOpen()) return false;

    const char *sql =
        "DELETE FROM call_activity_v1 "
        "WHERE config = ? AND band = ? AND callsign = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return noteResult(false);

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    auto call8 = callsign.toUtf8();
    int bindRc = SQLITE_OK;
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 3, call8.data(), call8.size(),
                                SQLITE_TRANSIENT);
    if (bindRc != SQLITE_OK) {
        // unbound: the zero-row DELETE would still report success
        sqlite3_finalize(stmt);
        return noteResult(false);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    int const changed = ok ? sqlite3_changes(db_) : 0;
    sqlite3_finalize(stmt);
    if (ok && changed == 0) {
        lastError_.clear();
    }
    return noteResult(ok) && changed > 0;
}

bool ActivityDB::deleteCalls(const QString &config, const QString &band) {
    if (!isOpen()) return false;

    const char *sql =
        "DELETE FROM call_activity_v1 WHERE config = ? AND band = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return noteResult(false);

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    int bindRc = SQLITE_OK;
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    if (bindRc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return noteResult(false);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return noteResult(ok);
}

QList<ActivityDB::CallRecord> ActivityDB::loadCalls(const QString &config,
                                                    const QString &band,
                                                    bool *ok) {
    QList<CallRecord> result;
    if (ok) *ok = false;
    if (!isOpen()) return result;

    const char *sql =
        "SELECT callsign, through, snr, grid, dial, offset, bits, tdrift, "
        "       cq_ts, ack_ts, utc_ts, submode "
        "FROM call_activity_v1 WHERE config = ? AND band = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        noteResult(false);
        return result;
    }

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    int bindRc = SQLITE_OK;
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    if (bindRc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        noteResult(false);
        return result;
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        CallRecord r;
        r.callsign = QString::fromUtf8(
            (const char *)sqlite3_column_text(stmt, 0),
            sqlite3_column_bytes(stmt, 0));
        r.through = QString::fromUtf8(
            (const char *)sqlite3_column_text(stmt, 1),
            sqlite3_column_bytes(stmt, 1));
        r.snr = sqlite3_column_int(stmt, 2);
        r.grid = QString::fromUtf8(
            (const char *)sqlite3_column_text(stmt, 3),
            sqlite3_column_bytes(stmt, 3));
        r.dial = (quint64)sqlite3_column_int64(stmt, 4);
        r.offset = sqlite3_column_int(stmt, 5);
        r.bits = sqlite3_column_int(stmt, 6);
        r.tdrift = (float)sqlite3_column_double(stmt, 7);
        r.cqTimestamp = fromTs(stmt, 8);
        r.ackTimestamp = fromTs(stmt, 9);
        r.utcTimestamp = fromTs(stmt, 10);
        r.submode = sqlite3_column_int(stmt, 11);
        result.append(r);
    }

    if (ok) *ok = (rc == SQLITE_DONE);

    sqlite3_finalize(stmt);
    noteResult(rc == SQLITE_DONE);
    return result;
}

bool ActivityDB::saveRxText(const QString &config, const QString &band,
                            const QString &html) {
    if (!isOpen() || !saveRxTextStmt_) return false;

    auto *stmt = saveRxTextStmt_;
    sqlite3_reset(stmt);

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    auto h8 = html.toUtf8();
    int bindRc = SQLITE_OK;
    // explicit lengths, not -1: strlen would truncate at an embedded NUL
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 3, h8.data(), h8.size(), SQLITE_TRANSIENT);
    if (bindRc != SQLITE_OK) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return noteResult(false);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return noteResult(ok);
}

QString ActivityDB::loadRxText(const QString &config, const QString &band,
                               bool *ok) {
    if (ok) *ok = false;
    if (!isOpen()) return {};

    const char *sql =
        "SELECT html FROM rx_text_v1 WHERE config = ? AND band = ? LIMIT 1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        noteResult(false);
        return {};
    }

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    int bindRc = SQLITE_OK;
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    if (bindRc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        noteResult(false);
        return {};
    }

    QString result;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        result = QString::fromUtf8(
            (const char *)sqlite3_column_text(stmt, 0),
            sqlite3_column_bytes(stmt, 0));
        rc = sqlite3_step(stmt);
    }
    if (ok) *ok = (rc == SQLITE_DONE);

    sqlite3_finalize(stmt);
    noteResult(rc == SQLITE_DONE);
    return result;
}

bool ActivityDB::clearRxText(const QString &config, const QString &band) {
    if (!isOpen()) return false;

    const char *sql =
        "DELETE FROM rx_text_v1 WHERE config = ? AND band = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return noteResult(false);

    auto c8 = config.toUtf8();
    auto b8 = band.toUtf8();
    int bindRc = SQLITE_OK;
    bindRc |= sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT);
    bindRc |= sqlite3_bind_text(stmt, 2, b8.data(), b8.size(), SQLITE_TRANSIENT);
    if (bindRc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return noteResult(false);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return noteResult(ok);
}

bool ActivityDB::hasImported(const QString &config, bool *ok) {
    if (ok) *ok = false;
    if (!isOpen()) return false;

    const char *sql =
        "SELECT 1 FROM legacy_import_v1 WHERE config = ? LIMIT 1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        noteResult(false);
        return false;
    }

    auto c8 = config.toUtf8();
    if (sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        sqlite3_finalize(stmt);
        noteResult(false);
        return false;
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    bool const completed = (rc == SQLITE_ROW || rc == SQLITE_DONE);
    noteResult(completed);
    if (ok) *ok = completed;
    return rc == SQLITE_ROW;
}

bool ActivityDB::markImported(const QString &config) {
    if (!isOpen()) return false;

    const char *sql =
        "INSERT OR IGNORE INTO legacy_import_v1 (config) VALUES (?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return noteResult(false);

    auto c8 = config.toUtf8();
    if (sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        sqlite3_finalize(stmt);
        return noteResult(false);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return noteResult(ok);
}

bool ActivityDB::clearConfig(const QString &config) {
    if (!isOpen()) return false;

    const char *sqlCalls =
        "DELETE FROM call_activity_v1 WHERE config = ?;";
    const char *sqlText =
        "DELETE FROM rx_text_v1 WHERE config = ?;";

    auto c8 = config.toUtf8();

    bool ownTransaction = false;
    if (!inTransaction_) {
        if (!begin()) {
            return false;
        }
        ownTransaction = true;
    }
    bool ok = true;

    for (const char *sql : {sqlCalls, sqlText}) {
        if (!isOpen()) { // noteResult() may have closed a failing handle
            ok = false;
            break;
        }
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            noteResult(false);
            ok = false;
            continue;
        }
        if (sqlite3_bind_text(stmt, 1, c8.data(), c8.size(), SQLITE_TRANSIENT) !=
            SQLITE_OK) {
            sqlite3_finalize(stmt);
            noteResult(false);
            ok = false;
            continue;
        }
        bool stepOk = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        noteResult(stepOk);
        ok = stepOk && ok;
    }

    if (ownTransaction) {
        if (ok) {
            ok = commit();
        } else {
            rollback();
        }
    }

    return ok;
}

bool ActivityDB::copyConfig(const QString &from, const QString &to) {
    if (!isOpen()) return false;

    const char *sqlCalls =
        "INSERT OR IGNORE INTO call_activity_v1 "
        "  (config, band, callsign, through, snr, grid, dial, offset, "
        "   bits, tdrift, cq_ts, ack_ts, utc_ts, submode) "
        "SELECT ?, band, callsign, through, snr, grid, dial, offset, "
        "       bits, tdrift, cq_ts, ack_ts, utc_ts, submode "
        "FROM call_activity_v1 WHERE config = ?;";
    const char *sqlText =
        "INSERT OR IGNORE INTO rx_text_v1 (config, band, html) "
        "SELECT ?, band, html FROM rx_text_v1 WHERE config = ?;";
    const char *sqlMarker =
        "INSERT OR IGNORE INTO legacy_import_v1 (config) "
        "SELECT ? FROM legacy_import_v1 WHERE config = ?;";

    auto f8 = from.toUtf8();
    auto t8 = to.toUtf8();

    bool ownTransaction = false;
    if (!inTransaction_) {
        if (!begin()) {
            return false;
        }
        ownTransaction = true;
    }
    bool ok = true;

    for (const char *sql : {sqlCalls, sqlText, sqlMarker}) {
        if (!isOpen()) {
            ok = false;
            break;
        }
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            noteResult(false);
            ok = false;
            continue;
        }
        // 1 is the destination the SELECT writes, 2 the source it reads
        int bindRc = SQLITE_OK;
        bindRc |= sqlite3_bind_text(stmt, 1, t8.data(), t8.size(),
                                    SQLITE_TRANSIENT);
        bindRc |= sqlite3_bind_text(stmt, 2, f8.data(), f8.size(),
                                    SQLITE_TRANSIENT);
        if (bindRc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            noteResult(false);
            ok = false;
            continue;
        }
        bool stepOk = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        noteResult(stepOk);
        ok = stepOk && ok;
    }

    if (ownTransaction) {
        if (ok) {
            ok = commit();
        } else {
            rollback();
        }
    }

    return ok;
}
