#pragma once

#include <QDateTime>
#include <QString>

/**
 * @class HBBlockingDB
 * @brief Tracks per-callsign ALLCALL timestamps for rate-limiting replies.
 *
 * Stores the last-seen timestamp for each callsign in a dedicated SQLite database
 * (hb_blocking.db3). If a station ALLCALL's again within the 55 minute block window,
 * the auto-reply is suppressed. This prevents spamming for ALLCALL-related
 * commands (HB, QUERY MSGS, QUERY CALL).
 *
 * ALLCALL reply cooldown replaces the former QCache method in
 * processCommandActivity.cpp
 */

struct sqlite3;
struct sqlite3_stmt;

class HBBlockingDB {
public:
    explicit HBBlockingDB(const QString &path);
    ~HBBlockingDB();

    bool open();
    void close();
    bool isOpen() const;
    QString error() const;

    // ALLCALL reply cooldown (60 min)
    bool     upsertAllcallReplyTimestamp(const QString &callsign, const QDateTime &ts);
    QDateTime getAllcallReplyTimestamp(const QString &callsign);
    bool     deleteAllcallReplyTimestamp(const QString &callsign);

private:
    QString  path_;
    sqlite3 *db_;
};
