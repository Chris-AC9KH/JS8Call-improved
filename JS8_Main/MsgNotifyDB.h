#pragma once

#include <QDateTime>
#include <QString>

/**
 * @class MsgNotifyDB
 * @brief Tracks stored message notification attempts, keyed by the inbox message id.
 *
 * Stores the last time we proactively sent a directed "<CALL> ACK MSG ID <mid>"
 * notification for a given stored message, in a dedicated SQLite database
 * (msg_notify.db3). This lets processStoredMessageNotifications() throttle
 * re-notification to no more than once every 12 hours per message, and
 * persists across program restarts
 *
 * The row for a message id is removed once the message is delivered
 * (QUERY MSG retrieval) or deleted from the inbox, at which point
 * notifications for it simply stop.
 */

struct sqlite3;
struct sqlite3_stmt;

class MsgNotifyDB {
public:
    explicit MsgNotifyDB(const QString &path);
    ~MsgNotifyDB();

    bool open();
    void close();
    bool isOpen() const;
    QString error() const;

    // Record that we just sent (or are about to send) a notification for
    // this message id, addressed to the given callsign.
    bool upsertSent(int msgId, const QString &callsign, const QDateTime &ts);

    // Returns the last time we notified for this message id, or an invalid
    // QDateTime if we've never notified (or the row has been cleared).
    QDateTime getLastSent(int msgId);

    // Called once the message has been delivered (QUERY MSG retrieval) or
    // deleted from the inbox, so notifications for it stop.
    bool deleteByMsgId(int msgId);

private:
    QString  path_;
    sqlite3 *db_;
};
