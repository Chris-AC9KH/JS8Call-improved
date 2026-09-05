/**
 * @file ActivityDB.h
 * @brief Declares the SQLite store behind activity.db3 (issue #267).
 */
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

/**
 * @class ActivityDB
 * @brief Persistent per-band activity storage (activity.db3).
 *
 * Stores the Call Activity table and the RX text history in a dedicated
 * SQLite database, keyed by (configuration, band, callsign) so that
 * activity heard on one band can never be attributed to another (issue
 * #267), and keyed by configuration name so that MultiSettings
 * configurations each keep their own activity, mirroring the way each
 * configuration keeps its own settings group in the .ini. The key is
 * a UUID each configuration generates once into its own settings, so
 * renames follow automatically, a settings reset orphans the old rows
 * and starts clean, and nothing ever infers that stored rows should
 * be deleted or moved. Deleting a configuration leaves its rows
 * behind (orphaned, never shown); a clone mints an id of its own and
 * takes a copy of its source's rows under it at its first start, so the
 * two then diverge.
 *
 * Writes happen as activity arrives (write-on-change), not at shutdown,
 * so a crash, power loss, or SIGKILL loses at most the in-flight row
 * rather than everything since the last clean close; open() covers the
 * journal and synchronous levels that keep those writes cheap and safe.
 *
 * Rows are never aged out of the store. The callsign-aging setting is
 * applied to what a band load contributes, bounding what a session
 * shows, while the store itself keeps everything - so long-term
 * activity survives restarts and upgrades without flooding the table.
 *
 * The legacy [CallActivity] group and RXActivity key in the .ini are
 * imported once per configuration by ActivityStorageController, which
 * knows the ini layout and the band plan, and left in place for older
 * versions of the software, following the inbox_v1 -> inbox_v2
 * migration pattern. The fire-once marker is a row here keyed by the
 * configuration id, so it follows the data.
 */

struct sqlite3;
struct sqlite3_stmt;

class ActivityDB {
public:
    /**
     * @brief One persisted Call Activity row - the full CallDetail field
     *        set, so a band round-trip within a session loses nothing
     *        (the legacy .ini group stored only a subset).
     */
    struct CallRecord {
        QString   callsign;
        QString   through;
        int       snr = 0;
        QString   grid;
        quint64   dial = 0;
        int       offset = 0;
        int       bits = 0;
        float     tdrift = 0.0f;
        QDateTime cqTimestamp;
        QDateTime ackTimestamp;
        QDateTime utcTimestamp;
        int       submode = 0;
    };

    explicit ActivityDB(const QString &path);
    ~ActivityDB();

    ActivityDB(const ActivityDB &) = delete;
    ActivityDB &operator=(const ActivityDB &) = delete;

    bool open();
    void close();

    /**
     * @brief Whether the handle is usable.
     *
     * False after a failed open(), and after enough consecutive
     * read/write failures that the handle closed itself - so a store
     * that breaks mid-session (vanished volume, creeping corruption)
     * eventually reports unusable instead of failing every call forever.
     */
    bool isOpen() const;

    /**
     * @brief The message captured at the most recent failure.
     *
     * Meaningful after any call here returns false - including after the
     * failing call ran further (successful) statements such as a
     * rollback, which would have reset sqlite3_errmsg() to "not an
     * error".
     */
    QString error() const;

    /**
     * @brief Batch several writes into a single transaction (legacy
     *        import, QSY offset rewrites).
     *
     * A failed begin() leaves autocommit in effect, so the individual
     * writes still land - just unbatched.
     */
    bool begin();

    /**
     * @brief Commit the open transaction.
     *
     * A failed commit() rolls the batch back itself, so the connection
     * can never be left stuck inside a transaction that would silently
     * swallow every later write.
     */
    bool commit();

    /// @brief Roll the open transaction back.
    void rollback();

    /// @brief Whether a transaction is currently open.
    bool inTransaction() const { return inTransaction_; }

    // Call activity
    bool upsertCall(const QString &config, const QString &band,
                    const CallRecord &record);
    /**
     * @brief Remove one stored call from a band.
     * @return True only when a row was actually removed.
     *
     * A statement that ran but matched nothing (the row is filed under
     * another band) reports false, so the caller can tell the user rather
     * than appear to have deleted something. error() is empty in that
     * case, and carries a message only when the statement itself failed.
     */
    bool deleteCall(const QString &config, const QString &band,
                    const QString &callsign);
    bool deleteCalls(const QString &config, const QString &band);

    /**
     * @brief Load a band's stored calls.
     * @param ok When provided, reports whether the read completed.
     *
     * A read error is not the same as an empty band, so a transient I/O
     * failure can be kept from being treated as - and then overwriting -
     * genuinely absent data.
     */
    QList<CallRecord> loadCalls(const QString &config, const QString &band,
                                bool *ok = nullptr);

    // RX text history (one HTML document per configuration + band)
    bool saveRxText(const QString &config, const QString &band,
                    const QString &html);
    QString loadRxText(const QString &config, const QString &band,
                       bool *ok = nullptr);
    bool clearRxText(const QString &config, const QString &band);

    /**
     * @brief Whether the legacy .ini import has run for a configuration.
     * @param config The id to probe.
     * @param ok When provided, reports whether the probe completed.
     * @return True when the marker row is present.
     */
    bool hasImported(const QString &config, bool *ok = nullptr);

    /**
     * @brief Record that the legacy .ini import has run.
     * @param config The id the marker is filed under.
     * @return True when the marker was written or already there.
     *
     * Written inside the import's transaction, so a failed import rolls
     * it back with the rows. clearConfig() deliberately leaves it.
     */
    bool markImported(const QString &config);

    /**
     * @brief Wipe everything stored for a configuration.
     *
     * The startup "Reset ... Call Activity and RX History" behavior, the
     * user-facing "Clear All Activity" actions, and the fresh-start wipe
     * after a configuration reset. Both tables or neither: where a
     * transaction cannot be opened it does nothing, rather than destroy
     * one table and fail on the other.
     */
    bool clearConfig(const QString &config);

    /**
     * @brief Copy everything stored for one configuration under a second
     *        configuration's id.
     * @param from The id whose rows are read.
     * @param to The id the copies are written under.
     * @return True when all three tables were copied.
     *
     * The clone half of the scheme above: a cloned configuration mints
     * its own id and takes a private copy of its source's rows, rather
     * than sharing them. Rows the destination already holds win, because
     * a clone whose copy was deferred by a degraded start may have
     * written newer ones of its own in the meantime. All three tables
     * or none: the copy runs in one transaction, so a failure leaves
     * nothing half-copied and the whole copy retries at the next start.
     * That holds when the copy opens the transaction itself; inside a
     * caller's transaction a failure is only reported, and the caller's
     * rollback governs.
     */
    bool copyConfig(const QString &from, const QString &to);

private:
    void captureError();
    bool noteResult(bool ok);
    void noteFailureCaptured();

    QString       path_;
    sqlite3      *db_;
    QString       lastError_;
    bool          inTransaction_;
    bool          pendingClose_; // third strike landed mid-transaction
    int           consecutiveFailures_;
    /// Prepared once at open() and reused for the life of the handle.
    sqlite3_stmt *upsertCallStmt_;
    sqlite3_stmt *saveRxTextStmt_;
};
