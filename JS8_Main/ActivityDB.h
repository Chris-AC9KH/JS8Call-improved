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
 * behind (orphaned, never shown); a clone shares its source's id and
 * hence its activity.
 *
 * Writes happen as activity arrives (write-on-change), not at shutdown,
 * so a crash, power loss, or SIGKILL loses at most the in-flight row
 * rather than everything since the last clean close. The database is
 * opened in WAL mode with synchronous=NORMAL: commits are not
 * individually fsynced, keeping frequent small writes off the GUI
 * thread's critical path, while the WAL keeps an interrupted write from
 * corrupting the store. Where the filesystem refuses WAL, the default
 * rollback journal and full synchronous level are kept instead.
 *
 * Rows are never aged out of the store. The callsign-aging setting is
 * applied to what a band load contributes, bounding what a session
 * shows, while the store itself keeps everything - so long-term
 * activity survives restarts and upgrades without flooding the table.
 *
 * The legacy [CallActivity] group and RXActivity key in the .ini are
 * imported once per configuration, in a single transaction, so a failed
 * import retries on the next start instead of being silently lost. They
 * are left in place for older versions of the software, following the
 * inbox_v1 -> inbox_v2 migration pattern. The import itself happens in
 * UI_Constructor, which knows the ini layout and the band plan. Its
 * fire-once marker lives in the configuration's own settings, so it
 * travels with the configuration through renames and clones; because
 * the data it gates lives here instead, the import also re-runs if the
 * marker is set while this configuration has nothing stored (an ini
 * restored without its database).
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

    // False after a failed open(), and after enough consecutive
    // read/write failures that the handle closed itself - so a store
    // that breaks mid-session (vanished volume, creeping corruption)
    // eventually reports unusable instead of failing every call forever.
    bool isOpen() const;

    // The message captured at the most recent failure. Meaningful after
    // any call here returns false - including after the failing call ran
    // further (successful) statements such as a rollback, which would
    // have reset sqlite3_errmsg() to "not an error".
    QString error() const;

    // Batch several writes into a single transaction (legacy import, QSY
    // offset rewrites). A failed begin() leaves autocommit in effect, so
    // the individual writes still land - just unbatched; a failed
    // commit() rolls the batch back itself, so the connection can never
    // be left stuck inside a transaction that would silently swallow
    // every later write.
    bool begin();
    bool commit();
    void rollback();
    bool inTransaction() const { return inTransaction_; }

    // Call activity
    bool upsertCall(const QString &config, const QString &band,
                    const CallRecord &record);
    // True only when a row was actually removed: a statement that ran
    // but matched nothing (the row is filed under another band) reports
    // false, so the caller can tell the user rather than appear to have
    // deleted something.
    bool deleteCall(const QString &config, const QString &band,
                    const QString &callsign);
    bool deleteCalls(const QString &config, const QString &band);

    // Loads a band's rows. A read error is not the same as an empty band:
    // when ok is provided it reports whether the read completed, so a
    // transient I/O failure can be kept from being treated as - and then
    // overwriting - genuinely absent data.
    QList<CallRecord> loadCalls(const QString &config, const QString &band,
                                bool *ok = nullptr);

    // RX text history (one HTML document per configuration + band)
    bool saveRxText(const QString &config, const QString &band,
                    const QString &html);
    QString loadRxText(const QString &config, const QString &band,
                       bool *ok = nullptr);
    bool clearRxText(const QString &config, const QString &band);

    // Whether anything is stored for a configuration. When ok is
    // provided it reports whether the probe completed, so a failed read
    // is never mistaken for "nothing stored".
    bool hasConfig(const QString &config, bool *ok = nullptr);

    // Wipe everything stored for a configuration (the startup
    // "Reset ... Call Activity and RX History" behavior, the user-facing
    // "Clear All Activity" actions, and the fresh-start wipe after a
    // configuration reset).
    bool clearConfig(const QString &config);

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
    // The two hot-path statements are prepared once at open() - which
    // doubles as a schema validation probe against a pre-existing
    // incompatible table - and reused for the life of the handle.
    sqlite3_stmt *upsertCallStmt_;
    sqlite3_stmt *saveRxTextStmt_;
};
