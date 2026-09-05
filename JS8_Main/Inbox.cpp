/**
 * @file Inbox.cpp
 * @brief Implementation of the message inbox, backed by the normalized
 *        inbox_v2 schema. On first open, if a legacy inbox_v1 (JSON-blob)
 *        table is present, its rows are migrated into inbox_v2 once;
 *        inbox_v1 itself is left untouched and is never written to again.
 * @see migrateV1ToV2() with associated notes on deprecation of v1
 * @author Chris-AC9KH (C) 2026 - All Rights Reserved
 */

#include "Inbox.h"
#include "DriftingDateTime.h"
#include "vendor/sqlite3/sqlite3.h"

#include <QLoggingCategory>
#include <QVariant>

Q_DECLARE_LOGGING_CATEGORY(inbox_js8)

namespace {

/**
 * @brief Current (and only) database schema this software creates or writes.
 * @details Every message field lives in its own normalized column - there is
 *          no JSON blob column here, so no data is left un-migrated behind a
 *          column this software can't see or index. inbox_group_recip_v2
 *          tracks per-recipient delivery for group ("@GROUP") messages, one
 *          row per (msg_id, callsign) that's been delivered to.
 */
constexpr char SCHEMA_V2[] =
    "CREATE TABLE IF NOT EXISTS inbox_v2 ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT, "
    "  type      TEXT NOT NULL, "
    "  utc       TEXT NOT NULL DEFAULT '', "
    "  from_call TEXT NOT NULL DEFAULT '', "
    "  to_call   TEXT NOT NULL DEFAULT '', "
    "  path      TEXT NOT NULL DEFAULT '', "
    "  cmd       TEXT NOT NULL DEFAULT '', "
    "  text      TEXT NOT NULL DEFAULT '', "
    "  grid      TEXT NOT NULL DEFAULT '', "
    "  extra     TEXT NOT NULL DEFAULT '', "
    "  tdrift    REAL NOT NULL DEFAULT 0, "
    "  dial      INTEGER NOT NULL DEFAULT 0, "
    "  offset    INTEGER NOT NULL DEFAULT 0, "
    "  snr       INTEGER NOT NULL DEFAULT 0, "
    "  submode   INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_inbox_v2__type ON inbox_v2(type);"
    "CREATE INDEX IF NOT EXISTS idx_inbox_v2__from ON inbox_v2(from_call);"
    "CREATE INDEX IF NOT EXISTS idx_inbox_v2__to ON inbox_v2(to_call);"
    "CREATE INDEX IF NOT EXISTS idx_inbox_v2__utc ON inbox_v2(utc);"
    "CREATE TABLE IF NOT EXISTS inbox_group_recip_v2 ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "  msg_id INTEGER, "
    "  callsign VARCHAR(255), "
    "  FOREIGN KEY(msg_id) REFERENCES inbox_v2(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_inbox_group_recip_v2__callsign ON"
    "  inbox_group_recip_v2(callsign);";

// Column order/list shared by every SELECT that needs to reconstruct a full
// Message (see row_to_message() below). Queries that only need `text` (to
// check for a non-empty message body) select just that column instead.
constexpr char SELECT_COLUMNS[] =
    "id, type, utc, from_call, to_call, path, cmd, text, grid, extra, "
    "tdrift, dial, offset, snr, submode";

QString column_text(sqlite3_stmt* const stmt, int const iCol) {
    auto const text = sqlite3_column_text(stmt, iCol);
    return text ? QString::fromUtf8(reinterpret_cast<const char*>(text),
                                    sqlite3_column_bytes(stmt, iCol))
                : QString();
}

// Reconstructs a Message from a row selected via SELECT_COLUMNS (columns
// 0..14, in that exact order). This replaces the old JSON round-trip
// entirely - every field the rest of the app reads out of a stored
// Message's params() now has a real column behind it.
Message row_to_message(sqlite3_stmt* const stmt) {
    auto const type = column_text(stmt, 1);
    auto const dial = (quint64)sqlite3_column_int64(stmt, 11);
    auto const offset = sqlite3_column_int(stmt, 12);

    QVariantMap params{
        {"UTC", column_text(stmt, 2)},
        {"FROM", column_text(stmt, 3)},
        {"TO", column_text(stmt, 4)},
        {"PATH", column_text(stmt, 5)},
        {"CMD", column_text(stmt, 6)},
        {"TEXT", column_text(stmt, 7)},
        {"GRID", column_text(stmt, 8)},
        {"EXTRA", column_text(stmt, 9)},
        {"TDRIFT", sqlite3_column_double(stmt, 10)},
        {"DIAL", dial},
        {"OFFSET", offset},
        {"FREQ", dial + offset},
        {"SNR", sqlite3_column_int(stmt, 13)},
        {"SUBMODE", sqlite3_column_int(stmt, 14)},
    };

    return Message(type, "", params);
}

} // namespace

Inbox::Inbox(QString path) : path_{path}, db_{nullptr} {}
Inbox::~Inbox() { close(); }

/**
 * Low-Level Interface
 **/

bool Inbox::isOpen() { return db_ != nullptr; }

bool Inbox::open() {
    int rc = sqlite3_open(path_.toLocal8Bit().data(), &db_);
    if (rc != SQLITE_OK) {
        close();
        return false;
    }

    // Ensure schemas and migrate v1 -> v2 if needed
    if (!ensureSchemaAndMigrate()) {
        return false;
    }

    return true;
}

void Inbox::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

QString Inbox::error() {
    if (db_) {
        return QString::fromLocal8Bit(sqlite3_errmsg(db_));
    }
    return "";
}

/**
 * Schema + Migration
 *
 * @brief Investigates database system master tables to discover if a specific table exists.
 * @param[in] tableName The exact string identifier name of the table target.
 * @return True if found in sqlite_master registry entries, false if completely absent.
 */
bool Inbox::hasTable(const char* tableName) {
    if (!db_) return false;

    const char* sql =
        "SELECT name FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_bind_text(stmt, 1, tableName, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return false;
    }

    bool exists = false;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        exists = true;
    }
    sqlite3_finalize(stmt);
    return exists;
}

bool Inbox::createV2Schema() {
    int rc = sqlite3_exec(db_, SCHEMA_V2, nullptr, nullptr, nullptr);
    return rc == SQLITE_OK;
}

bool Inbox::ensureSchemaAndMigrate() {
    // inbox_v2 not existing yet is the "fire once" migration signal: either
    // this is a brand new database, or it's the first time this version of
    // the software has opened a database left behind by an older version.
    // Either way, create inbox_v2 and, if a legacy inbox_v1 is sitting
    // there, migrate its rows in once. inbox_v1 itself is left completely
    // untouched; removing it is a job for a future release.
    // At present this provides a minimal layer of compatibility for older
    // versions of JS8Call that might try to open a MSG inbox that was migrated
    // by newer software that uses inbox_v2.
    if (!hasTable("inbox_v2")) {
        if (!createV2Schema()) return false;

        if (hasTable("inbox_v1")) {
            if (!migrateV1ToV2()) return false;
        }
    }

    return true;
}

/**
 * @deprecated The inbox_v1 (JSON-blob) schema was deprecated as of JS8Call
 *             3.0.3 in favor of inbox_v2's normalized columns. This method
 *             exists solely as a one-time upgrade path for databases created
 *             by pre-3.0.3 versions of the software: it is invoked once from
 *             ensureSchemaAndMigrate() when inbox_v2 does not yet exist and a
 *             legacy inbox_v1 table is found. inbox_v1 is read here but never
 *             written to, and is otherwise left in place untouched (removal
 *             is left for a future release). New code should not read from
 *             or write to inbox_v1; use inbox_v2 via the query helpers below
 *             instead.
 */
bool Inbox::migrateV1ToV2() {
    const char* selectSql = "SELECT id, blob FROM inbox_v1 ORDER BY id ASC;";
    sqlite3_stmt* sel = nullptr;
    int rc = sqlite3_prepare_v2(db_, selectSql, -1, &sel, nullptr);
    if (rc != SQLITE_OK) return false;

    rc = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(sel);
        return false;
    }

    const char* insertSql =
        "INSERT INTO inbox_v2 "
        "(id, type, utc, from_call, to_call, path, cmd, text, grid, extra, "
        " tdrift, dial, offset, snr, submode) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* ins = nullptr;
    rc = sqlite3_prepare_v2(db_, insertSql, -1, &ins, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(sel);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    int migrated = 0, skipped = 0;

    while ((rc = sqlite3_step(sel)) == SQLITE_ROW) {
        int id = sqlite3_column_int(sel, 0);
        QByteArray blobJson(reinterpret_cast<const char*>(sqlite3_column_text(sel, 1)),
                            sqlite3_column_bytes(sel, 1));

        Message m;
        try {
            m = Message::fromJson(blobJson);
        } catch (...) {
            qCWarning(inbox_js8)
                << "inbox_v1 migration: skipping unparseable row id" << id;
            ++skipped;
            continue;
        }

        const auto params = m.params();
        auto t8  = m.type().toUtf8();
        auto u8  = params.value("UTC").toString().toUtf8();
        auto f8  = params.value("FROM").toString().toUtf8();
        auto to8 = params.value("TO").toString().toUtf8();
        auto p8  = params.value("PATH").toString().toUtf8();
        auto c8  = params.value("CMD").toString().toUtf8();
        auto x8  = params.value("TEXT").toString().toUtf8();
        auto g8  = params.value("GRID").toString().toUtf8();
        auto e8  = params.value("EXTRA").toString().toUtf8();

        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);

        sqlite3_bind_int(ins, 1, id);
        sqlite3_bind_text(ins, 2, t8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, u8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, f8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 5, to8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 6, p8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 7, c8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 8, x8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 9, g8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 10, e8.constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(ins, 11, params.value("TDRIFT").toDouble());
        // 64-bit: a dial is a Radio::Frequency, and the band plan runs
        // past 2^31 Hz - truncating it to 0 would file the sender's
        // activity under no band at all
        sqlite3_bind_int64(ins, 12,
                           (sqlite3_int64)params.value("DIAL").toULongLong());
        sqlite3_bind_int(ins, 13, params.value("OFFSET").toInt());
        sqlite3_bind_int(ins, 14, params.value("SNR").toInt());
        sqlite3_bind_int(ins, 15, params.value("SUBMODE").toInt());

        if (sqlite3_step(ins) != SQLITE_DONE) {
            qCWarning(inbox_js8)
                << "inbox_v1 migration: failed to insert row id" << id;
            ++skipped;
            continue;
        }

        ++migrated;
    }

    sqlite3_finalize(ins);
    sqlite3_finalize(sel);

    // Migrate the group-recipient delivery table too, if present.
    if (hasTable("inbox_group_recip_v1")) {
        const char* copyGroup =
            "INSERT INTO inbox_group_recip_v2 (msg_id, callsign) "
            "SELECT msg_id, callsign FROM inbox_group_recip_v1;";
        if (sqlite3_exec(db_, copyGroup, nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    qCInfo(inbox_js8) << "inbox_v1 -> inbox_v2 migration complete:" << migrated
                      << "row(s) migrated," << skipped << "row(s) skipped";

    return rc == SQLITE_OK;
}

/**
 * Query helpers (now using inbox_v2)
 */

namespace {
// The public API takes a JSON-path-shaped `query` string (a holdover from
// the old json_extract()-based interface, e.g. "$.params.TO") to select
// which column a `type`/`match` lookup filters on. Every real call site
// passes one of the two mapped paths below; anything else (including the
// literal "$" used by the debug/dummy-data path in initializeDummyData.cpp,
// which historically matched every row of a type) is treated as "no extra
// column filter."
QString query_to_column(QString const& query) {
    auto const q = query.trimmed();
    if (q == "$.params.TO") return "to_call";
    if (q == "$.params.FROM") return "from_call";
    return {};
}
} // namespace

int Inbox::count(QString type, QString query, QString match) {
    if (!isOpen()) return -1;

    auto const column = query_to_column(query);

    QString sql = "SELECT COUNT(*) FROM inbox_v2 WHERE type = ?";
    if (!column.isEmpty()) {
        sql += QString(" AND %1 LIKE ?").arg(column);
    }
    sql += ";";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.toUtf8().constData(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    auto t8 = type.toUtf8();
    sqlite3_bind_text(stmt, 1, t8.constData(), -1, SQLITE_TRANSIENT);

    QByteArray m8;
    if (!column.isEmpty()) {
        m8 = match.toUtf8();
        sqlite3_bind_text(stmt, 2, m8.constData(), -1, SQLITE_TRANSIENT);
    }

    int count = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

QList<QPair<int, Message>> Inbox::values(QString type, QString query,
                                         QString match, int offset, int limit) {
    if (!isOpen()) return {};

    auto const column = query_to_column(query);

    QString sql = QString("SELECT %1 FROM inbox_v2 WHERE type = ?").arg(SELECT_COLUMNS);
    if (!column.isEmpty()) {
        sql += QString(" AND %1 LIKE ?").arg(column);
    }
    sql += " ORDER BY id ASC LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.toUtf8().constData(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return {};

    int idx = 1;
    auto t8 = type.toUtf8();
    sqlite3_bind_text(stmt, idx++, t8.constData(), -1, SQLITE_TRANSIENT);

    QByteArray m8;
    if (!column.isEmpty()) {
        m8 = match.toUtf8();
        sqlite3_bind_text(stmt, idx++, m8.constData(), -1, SQLITE_TRANSIENT);
    }

    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);

    QList<QPair<int, Message>> v;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        v.append({sqlite3_column_int(stmt, 0), row_to_message(stmt)});
    }

    sqlite3_finalize(stmt);
    return v;
}

QList<QPair<int, Message>> Inbox::fetchForCall(const QString& callPattern) {
    if (!isOpen()) return {};

    QList<QPair<int, Message>> msgs;
    msgs.append(values("STORE", "$.params.TO",   callPattern, 0, 1000));
    msgs.append(values("READ",  "$.params.FROM", callPattern, 0, 1000));
    msgs.append(values("UNREAD","$.params.FROM", callPattern, 0, 1000));

    std::stable_sort(msgs.begin(), msgs.end(),
        [](const QPair<int, Message>& a, const QPair<int, Message>& b) {
            return QVariant::compare(a.second.params().value("UTC"),
                                     b.second.params().value("UTC")) ==
                   QPartialOrdering::Greater;
        });

    return msgs;
}

namespace {
// UTF-8 byte buffers for a Message's text fields, held alive across a
// bind/step pair (sqlite3_bind_text below uses SQLITE_TRANSIENT, so this
// isn't strictly required for correctness, but keeps every bind call
// working from a stable, named buffer instead of a temporary).
struct MessageBuffers {
    QByteArray type, utc, from, to, path, cmd, text, grid, extra;
};

MessageBuffers make_buffers(Message const& m) {
    auto const params = m.params();
    return MessageBuffers{
        m.type().toUtf8(),
        params.value("UTC").toString().toUtf8(),
        params.value("FROM").toString().toUtf8(),
        params.value("TO").toString().toUtf8(),
        params.value("PATH").toString().toUtf8(),
        params.value("CMD").toString().toUtf8(),
        params.value("TEXT").toString().toUtf8(),
        params.value("GRID").toString().toUtf8(),
        params.value("EXTRA").toString().toUtf8(),
    };
}

// Binds the 14 non-id Message fields (type, utc, from_call, to_call, path,
// cmd, text, grid, extra, tdrift, dial, offset, snr, submode) to `stmt`
// starting at 1-based parameter index `idx`. Returns the next free
// parameter index, for callers with a trailing parameter (e.g. an
// UPDATE ... WHERE id = ?).
int bind_message_fields(sqlite3_stmt* const stmt, int idx, Message const& m,
                        MessageBuffers const& b) {
    auto const params = m.params();

    sqlite3_bind_text(stmt, idx++, b.type.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.utc.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.from.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.to.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.path.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.cmd.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.text.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.grid.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, b.extra.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, idx++, params.value("TDRIFT").toDouble());
    sqlite3_bind_int64(stmt, idx++,
                       (sqlite3_int64)params.value("DIAL").toULongLong());
    sqlite3_bind_int(stmt, idx++, params.value("OFFSET").toInt());
    sqlite3_bind_int(stmt, idx++, params.value("SNR").toInt());
    sqlite3_bind_int(stmt, idx++, params.value("SUBMODE").toInt());

    return idx;
}
} // namespace

Message Inbox::value(int key) {
    if (!isOpen()) return {};

    QString sql = QString("SELECT %1 FROM inbox_v2 WHERE id = ? LIMIT 1;").arg(SELECT_COLUMNS);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.toUtf8().constData(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return {};

    sqlite3_bind_int(stmt, 1, key);

    Message m;
    if ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        m = row_to_message(stmt);
    }

    sqlite3_finalize(stmt);
    return m;
}

int Inbox::append(Message value) {
    if (!isOpen()) return -1;

    const char* sql =
        "INSERT INTO inbox_v2 "
        "(type, utc, from_call, to_call, path, cmd, text, grid, extra, "
        " tdrift, dial, offset, snr, submode) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -2;

    auto const buffers = make_buffers(value);
    bind_message_fields(stmt, 1, value, buffers);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

bool Inbox::set(int key, Message value) {
    if (!isOpen()) return false;

    const char* sql =
        "UPDATE inbox_v2 SET "
        "type=?, utc=?, from_call=?, to_call=?, path=?, cmd=?, text=?, "
        "grid=?, extra=?, tdrift=?, dial=?, offset=?, snr=?, submode=? "
        "WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    auto const buffers = make_buffers(value);
    int idx = bind_message_fields(stmt, 1, value, buffers);
    sqlite3_bind_int(stmt, idx, key);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Inbox::del(int key) {
    if (!isOpen()) return false;

    const char* sql = "DELETE FROM inbox_v2 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, key);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

/**
 * High-Level Interface (now using inbox_v2)
 */

int Inbox::getLookaheadMessageIdForCallsign(const QString& callsign, int afterMsgId) {
    if (!isOpen()) return -1;

    const char* sql =
        "SELECT id, text FROM inbox_v2 "
        "WHERE id != ? AND type = 'STORE' AND to_call LIKE ? "
        "ORDER BY id ASC LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    auto c8 = callsign.toLocal8Bit();

    sqlite3_bind_int(stmt, 1, afterMsgId);
    sqlite3_bind_text(stmt, 2, c8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, 10);
    sqlite3_bind_int(stmt, 4, 0);

    int next_id = -1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int i = sqlite3_column_int(stmt, 0);
        auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (text && !QString::fromLocal8Bit(text).trimmed().isEmpty()) {
            next_id = i;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return next_id;
}

int Inbox::countUnreadFrom(QString from) {
    return count("UNREAD", "$.params.FROM", from);
}

QPair<int, Message> Inbox::firstUnreadFrom(QString from) {
    auto v = values("UNREAD", "$.params.FROM", from, 0, 1);
    if (v.isEmpty()) return {};
    return v.first();
}

QMap<QString, int> Inbox::getGroupMessageCounts() {
    if (!isOpen()) return {};

    QMap<QString, int> messageCounts;

    const char* sql =
        "SELECT count(id) as msg_count, to_call as group_name "
        "FROM inbox_v2 "
        "WHERE type = 'STORE' "
        "AND group_name LIKE '@%' "
        "AND utc > ? "
        "GROUP BY group_name;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return messageCounts;

    auto d8 = DriftingDateTime::currentDateTimeUtc()
                  .addDays(-2)
                  .toString("yyyy-MM-dd HH:mm:ss")
                  .toLocal8Bit();
    sqlite3_bind_text(stmt, 1, d8.constData(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        auto group = sqlite3_column_text(stmt, 1);
        messageCounts.insert(
            QString::fromLocal8Bit(reinterpret_cast<const char*>(group)), count);
    }

    sqlite3_finalize(stmt);
    return messageCounts;
}

bool Inbox::markGroupMsgDeliveredForCallsign(int msgId, QString callsign) {
    if (!isOpen()) return false;

    const char* existsSql =
        "SELECT count(id) FROM inbox_group_recip_v2 "
        "WHERE msg_id = ? AND callsign = ? LIMIT 1;";
    sqlite3_stmt* exists_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, existsSql, -1, &exists_stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    auto cs8 = callsign.toLocal8Bit();
    sqlite3_bind_int(exists_stmt, 1, msgId);
    sqlite3_bind_text(exists_stmt, 2, cs8.constData(), -1, SQLITE_TRANSIENT);

    bool recordExists = false;
    if (sqlite3_step(exists_stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(exists_stmt, 0);
        recordExists = (count > 0);
    }
    sqlite3_finalize(exists_stmt);

    if (!recordExists) {
        const char* insertSql =
            "INSERT INTO inbox_group_recip_v2 (msg_id, callsign) VALUES (?,?);";
        sqlite3_stmt* insert_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, insertSql, -1, &insert_stmt, nullptr);
        if (rc != SQLITE_OK) return false;

        sqlite3_bind_int(insert_stmt, 1, msgId);
        sqlite3_bind_text(insert_stmt, 2, cs8.constData(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(insert_stmt);
        sqlite3_finalize(insert_stmt);
        if (rc != SQLITE_DONE) return false;
    }

    return true;
}

int Inbox::getNextGroupMessageIdForCallsign(const QString& group_name,
                                            const QString& callsign) {
    if (!isOpen()) return -1;

    const char* sql =
        "SELECT inbox_v2.id, inbox_v2.text "
        "FROM inbox_v2 "
        "LEFT JOIN inbox_group_recip_v2 ON "
        "(inbox_group_recip_v2.msg_id = inbox_v2.id AND inbox_group_recip_v2.callsign = ?) "
        "WHERE inbox_v2.type = 'STORE' "
        "AND inbox_v2.to_call LIKE ? "
        "AND inbox_v2.utc > ? "
        "AND inbox_group_recip_v2.id IS NULL "
        "ORDER BY inbox_v2.id ASC "
        "LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    auto c8 = callsign.toLocal8Bit();
    auto g8 = group_name.toLocal8Bit();
    auto d8 = DriftingDateTime::currentDateTimeUtc()
                  .addDays(-2)
                  .toString("yyyy-MM-dd HH:mm:ss")
                  .toLocal8Bit();

    sqlite3_bind_text(stmt, 1, c8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, g8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, d8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, 10);
    sqlite3_bind_int(stmt, 5, 0);

    int next_id = -1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int i = sqlite3_column_int(stmt, 0);
        auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (text && !QString::fromLocal8Bit(text).trimmed().isEmpty()) {
            next_id = i;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return next_id;
}

int Inbox::getLookaheadGroupMessageIdForCallsign(const QString& group_name,
                                                 const QString& callsign,
                                                 int afterMsgId) {
    if (!isOpen()) return -1;

    const char* sql =
        "SELECT inbox_v2.id, inbox_v2.text "
        "FROM inbox_v2 "
        "LEFT JOIN inbox_group_recip_v2 ON "
        "(inbox_group_recip_v2.msg_id = inbox_v2.id AND inbox_group_recip_v2.callsign = ?) "
        "WHERE inbox_v2.id != ? "
        "AND inbox_v2.type = 'STORE' "
        "AND inbox_v2.to_call LIKE ? "
        "AND inbox_v2.utc > ? "
        "AND inbox_group_recip_v2.id IS NULL "
        "ORDER BY inbox_v2.id ASC "
        "LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    auto c8 = callsign.toLocal8Bit();
    auto g8 = group_name.toLocal8Bit();
    auto d8 = DriftingDateTime::currentDateTimeUtc()
                  .addDays(-2)
                  .toString("yyyy-MM-dd HH:mm:ss")
                  .toLocal8Bit();

    sqlite3_bind_text(stmt, 1, c8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, afterMsgId);
    sqlite3_bind_text(stmt, 3, g8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, d8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, 10);
    sqlite3_bind_int(stmt, 6, 0);

    int next_id = -1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int i = sqlite3_column_int(stmt, 0);
        auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (text && !QString::fromLocal8Bit(text).trimmed().isEmpty()) {
            next_id = i;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return next_id;
}

int Inbox::countUnreadForCallsign(const QString& callsign) {
    if (!isOpen()) return 0;

    const char* sql =
        "SELECT text FROM inbox_v2 "
        "WHERE type = 'STORE' AND to_call LIKE ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    auto c8 = callsign.toLocal8Bit();
    sqlite3_bind_text(stmt, 1, c8.constData(), -1, SQLITE_TRANSIENT);

    int count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text && !QString::fromLocal8Bit(text).trimmed().isEmpty()) {
            ++count;
        }
    }

    sqlite3_finalize(stmt);
    return count;
}

int Inbox::countGroupUnreadForCallsign(const QString& group_name,
                                       const QString& callsign) {
    if (!isOpen()) return -1;

    const char* sql =
        "SELECT inbox_v2.text FROM inbox_v2 "
        "LEFT JOIN inbox_group_recip_v2 ON "
        "(inbox_group_recip_v2.msg_id = inbox_v2.id AND inbox_group_recip_v2.callsign = ?) "
        "WHERE inbox_v2.type = 'STORE' "
        "AND inbox_v2.to_call LIKE ? "
        "AND inbox_v2.utc > ? "
        "AND inbox_group_recip_v2.id IS NULL;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    auto c8 = callsign.toLocal8Bit();
    auto g8 = group_name.toLocal8Bit();
    auto d8 = DriftingDateTime::currentDateTimeUtc()
                  .addDays(-2)
                  .toString("yyyy-MM-dd HH:mm:ss")
                  .toLocal8Bit();

    sqlite3_bind_text(stmt, 1, c8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, g8.constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, d8.constData(), -1, SQLITE_TRANSIENT);

    int count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text && !QString::fromLocal8Bit(text).trimmed().isEmpty()) {
            ++count;
        }
    }

    sqlite3_finalize(stmt);
    return count;
}

Q_LOGGING_CATEGORY(inbox_js8, "inbox.js8", QtWarningMsg)
