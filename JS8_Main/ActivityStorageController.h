/**
 * @file ActivityStorageController.h
 * @brief Declares the orchestration layer above ActivityDB (issue #267).
 */
#pragma once

#include "JS8_Main/ActivityDB.h"
#include "JS8_Main/CallDetail.h"

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class Configuration;
class QSettings;
class QTextDocument;
class QTextEdit;

/**
 * @class ActivityStorageController
 * @brief Orchestrates per-band persistent activity storage (issue #267).
 *
 * ActivityDB is the SQLite wrapper; this is everything above it - which
 * bucket the panes belong to, when a bucket's stored history may be
 * merged into the session, when the RX pane may be written back, the
 * write batching, the one-time legacy .ini import, and the close-time
 * sweep. It deliberately does not know about UI_Constructor: the window
 * state it needs arrives through Context, so the window keeps only a
 * pointer to this object and thin delegating calls.
 *
 * "Bucket" rather than "band" throughout: the bucket a pane belongs to
 * is a band name, or "" - the out-of-plan bucket, which persists
 * activity heard on dials outside the band plan (transverter IFs,
 * channelized operation) the way the legacy un-banded ini did.
 */
class ActivityStorageController : public QObject {
    Q_OBJECT

public:
    /**
     * @brief The window state and operations this controller needs.
     *
     * A deliberately narrow, explicit interface. The pointers are to
     * objects that outlive this controller; the callbacks are the only
     * way it reaches back into the window.
     */
    struct Context {
        Configuration const *config = nullptr;
        /// The current configuration's settings (legacy import, config id).
        QSettings *settings = nullptr;
        /// The dial the ini falls back to when it has never recorded one.
        Radio::Frequency defaultDial = 0;
        QTextEdit *rxTextEdit = nullptr;
        /// The live Call Activity table; a seed merges stored rows into it.
        QMap<QString, CallDetail> *callActivity = nullptr;
        /// Unread-message counts; unread senders are exempt from aging.
        QMap<QString, int> const *inboxCounts = nullptr;
        QMap<QString, QMap<QString, CallDetail>> *callActivityBandCache =
            nullptr;
        QMap<QString, QString> const *rxTextBandCache = nullptr;

        std::function<void(QString const &)> showStatusMessage;
        std::function<void()> displayActivity;
        std::function<void()> clearRxFrameBlockNumbers;
        /// UI_Constructor::clearActivity - every pane and session queue.
        std::function<void()> clearActivityPanes;
        /// UI_Constructor::clearRXActivity.
        std::function<void()> clearRxPane;
        /// UI_Constructor::clearCallActivity.
        std::function<void()> clearCallActivityPane;
        /// UI_Constructor::cacheActivity - park the panes under a bucket.
        std::function<void(QString const &)> cacheActivity;
        /// Forget every per-band cache the window holds for one bucket.
        std::function<void(QString const &)> dropBandCache;
        /// UI_Constructor::restoreActivity - show a bucket's panes.
        std::function<void(QString const &)> restoreActivity;
        /// Empty every RAM band cache (the "Clear All Activity" action).
        std::function<void()> clearBandCaches;
    };

    /**
     * @brief Construct the controller over an already-built window.
     * @param context The window state and callbacks it may use.
     * @param parent Optional QObject parent. UI_Constructor passes none
     *  and holds the sole owning std::unique_ptr instead: the window is
     *  the only thing that ever refers to this object, and single
     *  ownership keeps the teardown order obvious rather than relying on
     *  a member unique_ptr running before ~QObject reaps its children.
     */
    explicit ActivityStorageController(Context context,
                                       QObject *parent = nullptr);
    ~ActivityStorageController() override;

    // Startup

    /**
     * @brief Import the legacy .ini activity once per configuration.
     * @return true if an import ran to completion this call.
     */
    bool importLegacyActivityIfNeeded();
    /**
     * @brief Start the grace period allowed for the rig to report a band.
     *
     * Until it does (or this lapses, covering Rig=None), RX text is not
     * filed: it carries no per-line frequency, so the bucket would only
     * be the previous session's guess.
     */
    void beginStartupGrace();
    /**
     * @brief Show the legacy .ini RX text when the store is unavailable.
     *
     * Display only, and recorded as such, so a later recovery does not
     * bank a second copy of history the import already holds.
     */
    void showLegacyRxTextIfDegraded();
    /**
     * @brief Connect the debounced RX-text write-back timers.
     */
    void setupRxTextAutosave();

    // The bucket on screen

    /// @brief The bucket the on-screen panes belong to ("" = out-of-plan).
    QString currentBucket() const { return m_activityBand; }
    /// @brief Whether a bucket has been restored into the panes yet.
    bool isBucketLoaded() const { return m_activityBandLoaded; }
    /**
     * @brief The rig reported the band the panes already hold.
     * @param band The confirmed band name.
     */
    void bandUnchanged(QString const &band);
    /**
     * @brief The rig reported a different band; park and switch buckets.
     * @param band The newly confirmed band name.
     */
    void bandChanged(QString const &band);
    /**
     * @brief Record that the window has restored a bucket's panes.
     * @param band The bucket restored.
     * @param paneReloaded true if the RX pane was replaced wholesale.
     */
    void bucketRestored(QString const &band, bool paneReloaded);

    // Writes

    /**
     * @brief Upsert one station into the store.
     * @param d The station as displayed.
     * @param fallbackToCurrentBand File under the on-screen bucket when
     *  the record carries no usable dial frequency of its own.
     */
    void persistCallActivity(CallDetail const &d,
                             bool fallbackToCurrentBand = false);
    /**
     * @brief Shift every stored offset for the current bucket.
     * @param hzDelta Signed offset change in Hz.
     */
    void adjustCallActivityOffsets(int hzDelta);
    /**
     * @brief Open a write batch; nested batches share one transaction.
     */
    void beginBatch();
    /**
     * @brief Close a write batch, committing at the outermost level.
     */
    void endBatch();

    // User-facing clears

    /// @brief Drop every stored row, across all buckets and both panes.
    void clearAllActivity();
    /// @brief Drop the current bucket's stored RX text.
    void clearRxActivity();
    /// @brief Drop the current bucket's stored Call Activity rows.
    void clearCallActivity();
    /**
     * @brief Drop one station from the current bucket.
     * @param call The callsign to remove.
     */
    void removeStoredCall(QString const &call);

    // Shutdown

    /**
     * @brief Final flush: sweep every confirmed bucket's pending writes.
     */
    void flushOnClose();

private:
    QString activityPath() const;
    ActivityDB *activityDB();
    QString activityConfigId() const;
    // single conversion pair, so a CallDetail field added later fails to
    // persist/restore loudly at the one site instead of silently in
    // hand-copied blocks
    ActivityDB::CallRecord toCallRecord(CallDetail const &) const;
    CallDetail fromCallRecord(ActivityDB::CallRecord const &) const;
    void seedActivityForBand(QString const &band);
    QString htmlBelowLegacyCopy(QTextDocument *doc) const;
    void purgeLegacyActivityIni();
    /**
     * @brief The store's last error, read off the handle directly.
     *
     * Failure branches read this rather than activityDB()->error(): a
     * failure that was the handle's third strike has just closed it, and
     * going through the accessor would push its own status message on top
     * of the one the caller is about to show.
     */
    QString lastStoreError() const {
        return m_activityDB ? m_activityDB->error() : QString{};
    }
    void startActivityBatchIfPending();
    void saveRxTextForBand(QString const &band);
    void switchActivityBucket(QString const &band);

    Context m_ctx;

    std::unique_ptr<ActivityDB> m_activityDB;
    QElapsedTimer m_activityDBRetryTimer; // throttles reopen attempts
    QString m_activityBand;
    bool m_activityBandLoaded = false; // false until the first restore
    QSet<QString> m_activitySeeded;
    // Degraded start: the store was down, so readSettings showed the
    // legacy ini copy in the RX pane.
    bool m_rxTextLegacyShown = false;
    QString m_rxTextLegacyBand;
    int m_rxTextLegacyBlocks = 0;
    // Depth counter
    int m_activityBatchDepth = 0;
    bool m_activityBatchBeginFailed = false;
    QSet<QString> m_rxTextDirtyBands;
    // Set when a requested startup reset, or a clone's inherited-activity
    // copy, could not be applied
    bool m_activityStoreDisabled = false;
    // Set in flushOnClose
    bool m_activityShuttingDown = false;
    // False until the rig has actually reported a band, and set
    // unconditionally at close
    bool m_activityBandConfirmed = false;
    // Buckets the rig has actually reported this session
    QSet<QString> m_activityBandConfirmedBands;
    QElapsedTimer m_activityStartupTimer;
    QElapsedTimer m_seedRetryTimer; // throttles failed-seed retries
    QTimer m_rxTextSaveTimer; // debounces RX-text writes to activity.db3
    QTimer m_rxTextSaveMaxTimer;
    // Storage id captured at first use
    mutable QString m_activityConfigId;
    // Skip unchanged RX-text rewrites via the document's revision counter
    int m_rxTextLastSavedRevision = -1;
    QString m_rxTextLastSavedBand;
};
