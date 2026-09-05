/**
 * @file ActivityStorageController.cpp
 * @brief Orchestration above ActivityDB - see the class documentation.
 */

#include "ActivityStorageController.h"

#include "JS8_Main/ActivitySettingsKeys.h"
#include "JS8_Main/Bands.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/Varicode.h"
#include "JS8_UI/Configuration.h"

#include <QDir>
#include <QLoggingCategory>
#include <QScrollBar>
#include <QSettings>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QUuid>
#include <QVariant>

Q_DECLARE_LOGGING_CATEGORY(activitystoragecontroller_js8)

namespace {
/**
 * @brief Order two timestamps, treating an invalid one as the older.
 * @param lhs The candidate timestamp.
 * @param rhs The timestamp it must beat.
 * @return true if lhs is valid and strictly newer than rhs.
 *
 * Explicit rather than QDateTime's operators: since Qt 6.8 a comparison
 * involving an invalid QDateTime is unordered, so both < and <= return
 * false and a row carrying no timestamp - every manually added station,
 * and every legacy-imported row - would win. Shared by the seed and by
 * persistCallActivity()'s cache merge, so both resolve a stored row
 * against a live one identically.
 */
bool newerThan(QDateTime const &lhs, QDateTime const &rhs) {
    if (!lhs.isValid()) return false;
    if (!rhs.isValid()) return true;
    return rhs < lhs;
}

/**
 * @brief Carry a displaced row's enrichment onto the row that won.
 * @param winner The newer row, enriched in place.
 * @param loser The row it replaces.
 *
 * Mirrors the UPSERT's own CASE guards: a bare re-hearing carries no
 * grid and neither timestamp, so taking them from the row it displaces
 * keeps what the database correctly kept.
 */
void carryEnrichment(CallDetail &winner, CallDetail const &loser) {
    if (winner.grid.isEmpty()) winner.grid = loser.grid;
    if (!winner.ackTimestamp.isValid())
        winner.ackTimestamp = loser.ackTimestamp;
    if (!winner.cqTimestamp.isValid())
        winner.cqTimestamp = loser.cqTimestamp;
}
} // namespace

ActivityStorageController::ActivityStorageController(Context context,
                                                     QObject *parent)
    : QObject(parent), m_ctx(std::move(context)) {
    m_rxTextSaveTimer.setSingleShot(true);
    m_rxTextSaveTimer.setInterval(5000);
    m_rxTextSaveMaxTimer.setSingleShot(true);
    m_rxTextSaveMaxTimer.setInterval(30000);
}

ActivityStorageController::~ActivityStorageController() = default;

/**
 * @brief Wire up the debounced write-on-change persistence of the RX pane.
 *
 * Any change re-arms the short debounce, so the stored copy trails the
 * pane by at most a few seconds instead of being written only at
 * shutdown. The second timer is armed once and deliberately not
 * restarted by further changes: sustained sub-interval traffic (fast
 * submodes, busy nets) would otherwise re-arm the debounce faster than
 * it can fire, deferring the write - and growing the crash-loss window -
 * indefinitely.
 */
void ActivityStorageController::setupRxTextAutosave() {
    auto const flushRxText = [this]() {
        m_rxTextSaveTimer.stop();
        m_rxTextSaveMaxTimer.stop();
        saveRxTextForBand(m_activityBand);
    };
    connect(&m_rxTextSaveTimer, &QTimer::timeout, this, flushRxText);
    connect(&m_rxTextSaveMaxTimer, &QTimer::timeout, this, flushRxText);
    connect(m_ctx.rxTextEdit->document(), &QTextDocument::contentsChanged,
            this, [this]() {
                m_rxTextSaveTimer.start();
                if (!m_rxTextSaveMaxTimer.isActive()) {
                    m_rxTextSaveMaxTimer.start();
                }
            });
}

/**
 * @brief Start the grace period for an as-yet unconfirmed bucket.
 *
 * While it runs, RX text for the startup guess is held in the pane rather
 * than filed under a band the rig has not reported; see
 * saveRxTextForBand().
 */
void ActivityStorageController::beginStartupGrace() {
    m_activityStartupTimer.start();
}

/**
 * @brief Show the legacy ini RX text on a degraded or disabled start.
 *
 * Purely for display, and only on a degraded start or a disabled
 * session: nothing here can reach the store either way. The window's
 * startup already restored that copy from the ini and never rewrites it,
 * so showing it beats showing an empty pane.
 *
 * The copy's extent is recorded in m_rxTextLegacyBand and
 * m_rxTextLegacyBlocks, because the recovery seed and the close-time
 * sweep treat only the text below it as this session's own.
 */
void ActivityStorageController::showLegacyRxTextIfDegraded() {
    if ((activityDB()->isOpen() && !m_activityStoreDisabled) ||
        m_ctx.config->reset_activity()) {
        return;
    }
    m_ctx.settings->beginGroup("UI_Constructor");
    auto const legacy = m_ctx.settings->value("RXActivity", "").toString();
    m_ctx.settings->endGroup();
    if (legacy.isEmpty()) {
        return;
    }
    m_ctx.rxTextEdit->setHtml(legacy);
    m_ctx.clearRxFrameBlockNumbers();
    auto const *doc = m_ctx.rxTextEdit->document();
    m_rxTextLegacyShown = true;
    m_rxTextLegacyBand = m_activityBand;
    m_rxTextLegacyBlocks = doc->blockCount();
    if (doc->lastBlock().length() <= 1) {
        // an append may splice into this trailing empty block
        --m_rxTextLegacyBlocks;
    }
}

/**
 * @brief The rig reported the band the window already believes it is on.
 * @param band The band reported.
 *
 * Confirms the bucket, moving the panes first if they are still showing
 * the startup guess. The confirmed flag is set only after
 * switchActivityBucket(), so the flush inside that switch still sees the
 * outgoing bucket as unconfirmed - otherwise the startup guess's pane,
 * which by then holds this band's decodes too, is written under the
 * guessed band.
 */
void ActivityStorageController::bandUnchanged(QString const &band) {
    if (m_activityBandLoaded && m_activityBand != band) {
        switchActivityBucket(band);
    }
    m_activityBandConfirmed = true;
    m_activityBandConfirmedBands.insert(band);
}

/**
 * @brief The rig reported a different band from the one on screen.
 * @param band The band reported.
 *
 * The confirmed flag is set after switchActivityBucket(), so the flush
 * inside it still treats the outgoing bucket as the startup guess it is.
 * The band is also recorded as confirmed, so the close-time sweep does
 * not skip its RX text.
 */
void ActivityStorageController::bandChanged(QString const &band) {
    switchActivityBucket(band);
    m_activityBandConfirmed = true;
    m_activityBandConfirmedBands.insert(band);
}

/**
 * @brief Move the activity panes from one storage bucket to another.
 * @param band The bucket to show: a band name, or "" for out-of-plan.
 *
 * Every caller - a band change, and the rig's first report correcting the
 * startup guess - goes through here, so no path can leave one bucket's
 * activity on screen under another's name, which is issue #267 itself.
 *
 * The outgoing bucket, when it is still the startup guess, has its cached
 * panes dropped rather than parked: its pane may hold text and stations
 * heard on the band the rig has just reported, and restoring that cache
 * on a later visit would file them under the guessed band. The bucket
 * reloads from the store at its next visit. Where the rig has not
 * reported at all only the two per-bucket panes move; clearActivity()
 * would also empty the compose box and the decode, command and spot
 * queues the session has accumulated. That state is the confirmation
 * flag, not an empty bucket name: "" is also the out-of-plan bucket,
 * whose Band Activity must not follow the panes to a band the rig later
 * reports.
 */
void ActivityStorageController::switchActivityBucket(QString const &band) {
    if (m_activityBandLoaded && m_activityBand == band) {
        return;
    }
    if (m_activityBandLoaded) {
        if (m_activityBandConfirmed) {
            saveRxTextForBand(m_activityBand);
            m_ctx.cacheActivity(m_activityBand);
        } else {
            m_ctx.dropBandCache(m_activityBand);
            m_activitySeeded.remove(m_activityBand);
            m_rxTextDirtyBands.remove(m_activityBand);
            if (m_rxTextLegacyShown && m_activityBand == m_rxTextLegacyBand) {
                m_rxTextLegacyShown = false;
                m_rxTextLegacyBand.clear();
                m_rxTextLegacyBlocks = 0;
            }
        }
    }

    if (!m_activityBandConfirmed) {
        m_ctx.rxTextEdit->clear();
        m_ctx.clearRxFrameBlockNumbers();
        m_ctx.clearCallActivityPane();
    } else {
        m_ctx.clearActivityPanes();
    }
    m_ctx.restoreActivity(band);
}

/**
 * @brief Take up the bucket the window has just restored on screen.
 * @param band The bucket now displayed.
 * @param paneReloaded True when the panes were actually reloaded from the
 *        RAM band caches; false when re-entering the bucket already on
 *        screen, where the live panes are newer than any cache entry.
 *
 * A reload disarms the saver and resyncs its change tracking, since what
 * was restored is what the cache holds. A band whose last flush failed is
 * the exception: its tracking is left at the -1 sentinel so it keeps
 * reporting unsaved, and its debounce is restarted here, because only
 * document activity would otherwise arm it and a quiet band would never
 * retry.
 */
void ActivityStorageController::bucketRestored(QString const &band,
                                               bool paneReloaded) {
    if (paneReloaded) {
        m_rxTextSaveTimer.stop();
        m_rxTextSaveMaxTimer.stop();
        m_rxTextLastSavedRevision =
            m_rxTextDirtyBands.contains(band)
                ? -1
                : m_ctx.rxTextEdit->document()->revision();
        m_rxTextLastSavedBand = band;
        if (m_rxTextDirtyBands.contains(band)) {
            m_rxTextSaveTimer.start();
        }
    }

    m_activityBand = band;
    m_activityBandLoaded = true;

    if (!m_activitySeeded.contains(band)) {
        seedActivityForBand(band);
    }
}

/**
 * @brief Path of the per-band activity store, beside the message inbox.
 * @return The absolute native path of activity.db3.
 */
QString ActivityStorageController::activityPath() const {
    return QDir::toNativeSeparators(
        m_ctx.config->writeable_data_dir().absoluteFilePath("activity.db3"));
}

/**
 * @brief The activity store, opened on first use.
 *
 * A store that fails to open - or that closed itself after repeated
 * failures - is retried on a throttle rather than per call. Callers may
 * always use the returned object: every method on it is a no-op while it
 * is closed. The throttle is armed however the handle ended up closed, so
 * a store that breaks mid-session is retried too, and discovering that
 * self-close here is its one operator-visible signal, the per-write
 * warnings being suppressed once the handle is closed.
 *
 * The status message is emitted once per failure episode rather than once
 * per retry because showStatusMessage() temporarily hides the statusbar's
 * mode and frequency readouts, and a blink every thirty seconds would
 * blank the operator's primary readouts indefinitely. The handle is never
 * replaced while a batch holds it, or callers inside beginBatch() and
 * endBatch() would be left with a dangling pointer and a vanished
 * transaction.
 */
ActivityDB *ActivityStorageController::activityDB() {
    if (m_activityDB && !m_activityDB->isOpen() &&
        !m_activityDBRetryTimer.isValid()) {
        m_activityDBRetryTimer.start();
        qCWarning(activitystoragecontroller_js8)
            << "activity store closed after repeated failures:"
            << m_activityDB->error();
        m_ctx.showStatusMessage(
            tr("Activity database unavailable - activity is not being "
               "saved (%1)")
                .arg(m_activityDB->error()));
    }
    bool const retrying = m_activityDB && !m_activityDB->isOpen() &&
                          m_activityDBRetryTimer.isValid() &&
                          m_activityDBRetryTimer.hasExpired(30 * 1000) &&
                          m_activityBatchDepth == 0;
    if (retrying) {
        m_activityDB.reset();
    }
    if (!m_activityDB) {
        m_activityDB = std::make_unique<ActivityDB>(activityPath());
        if (!m_activityDB->open()) {
            qCWarning(activitystoragecontroller_js8)
                << "could not open" << activityPath() << ":"
                << m_activityDB->error();
            if (!retrying) {
                m_ctx.showStatusMessage(
                    tr("Activity database unavailable - activity will "
                       "not be saved (%1)")
                        .arg(m_activityDB->error()));
            }
            m_activityDBRetryTimer.start();
        } else {
            m_activityDBRetryTimer.invalidate();
            if (retrying && m_activityBandLoaded &&
                !m_activitySeeded.contains(m_activityBand)) {
                // deferred: a seed re-enters this accessor
                QTimer::singleShot(0, this, [this]() {
                    if (m_activityBandLoaded &&
                        !m_activitySeeded.contains(m_activityBand)) {
                        seedActivityForBand(m_activityBand);
                        m_ctx.displayActivity();
                    }
                });
            }
        }
    }
    return m_activityDB.get();
}

/**
 * @brief Open a write batch; nested batches share one transaction.
 *
 * Batches are lazy: opening one records only the intent and the
 * transaction starts at the first write inside it, so a decode cycle that
 * persists nothing costs nothing - no BEGIN IMMEDIATE taking the write
 * lock on the GUI thread with its 5 s busy timeout, and no store-reopen
 * probe on behalf of a caller that never writes. The outermost begin also
 * clears the previous batch's failed-BEGIN mark, so a batch whose
 * transaction could not be opened does not suppress the next one's.
 */
void ActivityStorageController::beginBatch() {
    if (m_activityBatchDepth == 0) {
        m_activityBatchBeginFailed = false;
    }
    ++m_activityBatchDepth;
}

/**
 * @brief Start the transaction a batch deferred, at its first write.
 *
 * A BEGIN that fails is attempted once per batch rather than once per
 * write, each attempt being able to block the GUI thread for the busy
 * timeout. That batch's writes then run as autocommits, as they do after
 * any failed BEGIN, and the failure counts toward the store's self-close.
 */
void ActivityStorageController::startActivityBatchIfPending() {
    if (m_activityBatchDepth > 0 && !m_activityBatchBeginFailed &&
        m_activityDB && m_activityDB->isOpen() &&
        !m_activityDB->inTransaction()) {
        m_activityBatchBeginFailed = !m_activityDB->begin();
    }
}

/// @brief Close the outermost batch, committing its transaction.
void ActivityStorageController::endBatch() {
    if (m_activityBatchDepth == 1) {
        // the handle begin() used, so the accessor cannot swap in a
        // fresh one first
        if (m_activityDB && m_activityDB->inTransaction() &&
            !m_activityDB->commit()) {
            qCWarning(activitystoragecontroller_js8)
                << "could not commit activity batch:"
                << m_activityDB->error();
        }
        m_activityBatchBeginFailed = false;
    }
    --m_activityBatchDepth;
}

/**
 * @brief The storage key for this MultiSettings configuration.
 * @return A UUID generated once into the configuration's own settings.
 *
 * Activity is keyed by that id rather than by the configuration name: it
 * follows the configuration through renames automatically, is purged
 * with the settings by MultiSettings' "Reset Configuration" - orphaning
 * the old rows and starting clean, with no wipe heuristics that could
 * misfire - and a clone, arriving with a marker naming its source in
 * place of an id, mints a fresh one here and takes a copy of its
 * source's rows under it at its first start, so the two then diverge.
 * The id is captured once, because during a configuration switch
 * MultiSettings updates its state before this window closes and the
 * outgoing configuration's final flush must not land under the incoming
 * configuration's key.
 */
QString ActivityStorageController::activityConfigId() const {
    if (m_activityConfigId.isEmpty()) {
        auto id = m_ctx.settings->value(ActivitySettings::ACTIVITY_DB_ID_KEY)
                      .toString();
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_ctx.settings->setValue(ActivitySettings::ACTIVITY_DB_ID_KEY,
                                     id);
        }
        m_activityConfigId = id;
    }
    return m_activityConfigId;
}

/// @brief Convert a displayed call detail into a storable row.
ActivityDB::CallRecord
ActivityStorageController::toCallRecord(CallDetail const &d) const {
    ActivityDB::CallRecord r;
    r.callsign = d.call.trimmed();
    r.through = d.through;
    r.snr = d.snr;
    r.grid = d.grid;
    r.dial = d.dial;
    r.offset = d.offset;
    r.bits = d.bits;
    r.tdrift = d.tdrift;
    r.cqTimestamp = d.cqTimestamp;
    r.ackTimestamp = d.ackTimestamp;
    r.utcTimestamp = d.utcTimestamp;
    r.submode = d.submode;
    return r;
}

/// @brief Convert a stored row back into a displayed call detail.
CallDetail ActivityStorageController::fromCallRecord(
    ActivityDB::CallRecord const &r) const {
    CallDetail cd = {};
    cd.call = r.callsign;
    cd.through = r.through;
    cd.snr = r.snr;
    cd.grid = r.grid;
    cd.dial = r.dial;
    cd.offset = r.offset;
    cd.bits = r.bits;
    cd.tdrift = r.tdrift;
    cd.cqTimestamp = r.cqTimestamp;
    cd.ackTimestamp = r.ackTimestamp;
    cd.utcTimestamp = r.utcTimestamp;
    cd.submode = r.submode;
    return cd;
}

/**
 * @brief Write one call-activity row to the store.
 * @param d The row to persist.
 * @param fallbackToCurrentBand File a row carrying no dial under the
 *        bucket on screen; passed only where that is genuinely where it
 *        belongs (a manually added station, a logbook grid backfill, and
 *        qsy()'s offset write-back for those same rows).
 *
 * Rows are filed under the band of their own dial frequency, because
 * processDecodeEvent() deliberately stamps records with the capture-time
 * dial so that decodes completing after a QSY, and inbox senders, keep
 * the band they were heard on - keying by the live band would be issue
 * #267 all over again. A dial that resolves to no band files under "",
 * the out-of-plan bucket.
 *
 * A row filed under a bucket other than the one on screen is merged into
 * that bucket's cached table rather than forcing a re-seed of it: the
 * seed's RX-text merge is not idempotent against a pane restored from the
 * RAM cache, so un-seeding a bucket to refresh its call table would
 * splice that bucket's stored history in behind itself on every return
 * visit. The merge follows the seed's own rules - a cached row with a
 * strictly newer timestamp stands, otherwise the incoming row replaces it
 * and carries the cached row's enrichment across.
 */
void ActivityStorageController::persistCallActivity(
    CallDetail const &d, bool fallbackToCurrentBand) {
    if (m_activityStoreDisabled || d.call.trimmed().isEmpty()) {
        return;
    }

    auto band = m_ctx.config->bands()->find(d.dial);
    if (band.isEmpty() && d.dial == 0 && fallbackToCurrentBand &&
        m_activityBandConfirmed) {
        band = m_activityBand;
    }

    auto const r = toCallRecord(d);
    activityDB(); // resolve (and possibly recover) before the batch opens
    startActivityBatchIfPending();
    if (band != m_activityBand) {
        auto cached = m_ctx.callActivityBandCache->find(band);
        if (cached != m_ctx.callActivityBandCache->end()) {
            auto const key = d.call.trimmed();
            auto const shown = cached->constFind(key);
            if (shown == cached->constEnd() ||
                !newerThan(shown->utcTimestamp, d.utcTimestamp)) {
                auto row = d;
                if (shown != cached->constEnd()) {
                    carryEnrichment(row, *shown);
                }
                (*cached)[key] = row;
            }
        }
    }

    if (!activityDB()->upsertCall(activityConfigId(), band, r) &&
        activityDB()->isOpen()) {
        qCWarning(activitystoragecontroller_js8)
            << "could not persist call activity for" << r.callsign << ":"
            << lastStoreError();
    }
}

/**
 * @brief Rewrite every displayed offset after a waterfall nudge.
 * @param hzDelta The shift applied to the receiver's offsets.
 *
 * Only rows whose own dial belongs to the current band are written back:
 * entries displayed here but keyed to another band (post-QSY stragglers,
 * inbox senders carrying their message's dial) did not QSY, and dial-less
 * RAM-only entries must not be promoted into the store by a nudge. The
 * dial-less rows this bucket does store - manual adds - take the
 * fallback path instead, since Bands::find(0) resolves to "".
 */
void ActivityStorageController::adjustCallActivityOffsets(int hzDelta) {
    if (m_ctx.callActivity->isEmpty()) {
        return;
    }
    beginBatch();
    for (auto [key, value] : m_ctx.callActivity->asKeyValueRange()) {
        value.offset -= hzDelta;
        auto const rowBand = m_ctx.config->bands()->find(value.dial);
        if (rowBand == m_activityBand) {
            persistCallActivity(value);
        } else if (value.dial == 0 && !m_activityBand.isEmpty()) {
            persistCallActivity(value, true);
        }
    }
    endBatch();
}

/**
 * @brief HTML of everything below the degraded start's legacy ini copy.
 * @param doc The RX pane's document, or a cached copy of it.
 * @return The session text below the copy, or "" when there is none.
 */
QString
ActivityStorageController::htmlBelowLegacyCopy(QTextDocument *doc) const {
    if (doc->blockCount() <= m_rxTextLegacyBlocks) {
        return {};
    }
    QTextCursor cursor(doc->findBlockByNumber(m_rxTextLegacyBlocks));
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    if (cursor.selection().toPlainText().trimmed().isEmpty()) {
        return {};
    }
    return cursor.selection().toHtml();
}

/**
 * @brief Merge a bucket's stored history into the session, once.
 * @param band The bucket to seed.
 *
 * Runs at a bucket's first visit each session. Stored rows are merged
 * under whatever the session already holds - the newer of the two wins
 * per callsign, and stored enrichment (grid, ACK and CQ marks) survives a
 * bare re-hearing - and the stored RX text is placed behind the pane's
 * own lines. Until a bucket has been seeded its RX-text saves are
 * suppressed, so a document that does not contain the stored history can
 * never overwrite it; a failed seed is retried at the flush cadence, at
 * the next visit to the bucket, and after the store reopens. A failed
 * load returns without touching the RX pane, or the retry would splice
 * the stored text in a second time.
 *
 * The callsign-aging setting is applied to what the store contributes,
 * mirroring master's save-time prune: it bounds what a session can load,
 * while the store itself keeps everything, and without it stale rows
 * reach consumers that put an SNR on the air. Rows the session has
 * already heard are never pruned. The enrichment carried across from a
 * stored row mirrors the UPSERT's own CASE guards, so a bare re-hearing
 * cannot drop from the table what the database correctly kept.
 */
void ActivityStorageController::seedActivityForBand(QString const &band) {
    if (m_activityStoreDisabled || !activityDB()->isOpen()) {
        return;
    }
    auto const config = activityConfigId();

    bool callsOk = false;
    auto const stored = activityDB()->loadCalls(config, band, &callsOk);
    if (callsOk) {
        auto const aging = m_ctx.config->callsign_aging();
        auto const agingNow = DriftingDateTime::currentDateTimeUtc();

        QMap<QString, CallDetail> merged;
        foreach (auto const &r, stored) {
            auto const cd = fromCallRecord(r);
            if (aging && !m_ctx.callActivity->contains(cd.call) &&
                m_ctx.inboxCounts->value(cd.call, 0) <= 0 &&
                cd.utcTimestamp.isValid() &&
                cd.utcTimestamp.secsTo(agingNow) / 60 >= aging) {
                continue;
            }
            merged[cd.call] = cd;
        }

        QList<CallDetail> ramWinners;
        for (auto it = m_ctx.callActivity->constBegin();
             it != m_ctx.callActivity->constEnd(); ++it) {
            auto const found = merged.constFind(it.key());
            if (found != merged.constEnd() &&
                newerThan(found->utcTimestamp, it->utcTimestamp)) {
                continue;
            }

            auto live = it.value();
            if (found != merged.constEnd()) {
                carryEnrichment(live, *found);
                if (newerThan(it->utcTimestamp, found->utcTimestamp)) {
                    ramWinners.append(live);
                }
            } else {
                ramWinners.append(live);
            }
            merged[it.key()] = live;
        }
        *m_ctx.callActivity = merged;
        if (!ramWinners.isEmpty()) {
            beginBatch();
            foreach (auto const &cd, ramWinners) {
                if (cd.dial == 0) {
                    // Bands::find(0) is "": this would file it out-of-plan
                    continue;
                }
                persistCallActivity(cd);
            }
            endBatch();
        }
    } else {
        qCWarning(activitystoragecontroller_js8)
            << "could not load call activity for band" << band << ":"
            << lastStoreError();
        return;
    }

    // re-resolve: a load can self-close the handle a pointer came from
    bool rxOk = false;
    auto const storedHtml = activityDB()->loadRxText(config, band, &rxOk);
    if (rxOk) {
        auto sessionHtml = QString{};
        if (m_rxTextLegacyShown && band == m_rxTextLegacyBand) {
            sessionHtml = htmlBelowLegacyCopy(m_ctx.rxTextEdit->document());
        } else if (!m_ctx.rxTextEdit->toPlainText().trimmed().isEmpty()) {
            sessionHtml = m_ctx.rxTextEdit->toHtml();
        }
        if (!storedHtml.isEmpty()) {
            m_ctx.rxTextEdit->setHtml(storedHtml);
            if (!sessionHtml.isEmpty()) {
                auto cursor = QTextCursor(m_ctx.rxTextEdit->document());
                cursor.movePosition(QTextCursor::End);
                if (cursor.block().length() > 1) {
                    cursor.insertBlock();
                }
                cursor.insertHtml(sessionHtml);
            }
            m_ctx.clearRxFrameBlockNumbers();
            QTimer::singleShot(0, this, [this]() {
                m_ctx.rxTextEdit->verticalScrollBar()->setValue(
                    m_ctx.rxTextEdit->verticalScrollBar()->maximum());
            });
        }
        if (m_rxTextLegacyShown && band == m_rxTextLegacyBand) {
            m_rxTextLegacyShown = false;
            m_rxTextLegacyBand.clear();
            m_rxTextLegacyBlocks = 0;
        }
        m_rxTextSaveTimer.stop();
        m_rxTextSaveMaxTimer.stop();
        m_rxTextLastSavedBand = band;
        if (sessionHtml.isEmpty()) {
            m_rxTextLastSavedRevision =
                m_ctx.rxTextEdit->document()->revision();
        } else {
            m_rxTextLastSavedRevision = -1;
        }
    } else {
        qCWarning(activitystoragecontroller_js8)
            << "could not load RX text for band" << band << ":"
            << lastStoreError();
    }

    if (callsOk && rxOk) {
        m_activitySeeded.insert(band);
        qCDebug(activitystoragecontroller_js8)
            << "loaded" << stored.size() << "stored calls for band" << band;
        if (m_rxTextLastSavedRevision == -1) {
            saveRxTextForBand(band);
        }
    }
}

/**
 * @brief Persist the RX pane for a bucket, if it is safe to do so.
 * @param band The bucket the pane's contents belong to.
 *
 * Declines while the bucket is only the startup guess (RX text carries no
 * per-line frequency, so it could be filed under a band it was never
 * heard on) or while the bucket is unseeded, marking it for the
 * close-time sweep in both cases. An empty pane removes the stored row
 * rather than storing an empty one, or the debounced save after a Clear
 * would resurrect it. The startup grace period covers a station running
 * without CAT, and an unseeded bucket retries its seed at this flush
 * cadence, so one transient store failure cannot disable persistence for
 * a whole parked session.
 *
 * After a failed save the debounce is re-armed here, because the flush
 * stopped both timers before calling and only new document activity would
 * otherwise restart them - a band that fell quiet after one failed save
 * would never try again. It is re-armed at sixty seconds rather than
 * five, the thirty-second cap timer then setting the actual retry
 * cadence: a store that stays unwritable would otherwise serialise the
 * whole unbounded RX document on the GUI thread every five seconds
 * forever. That back-off lasts until the next successful save.
 */
void ActivityStorageController::saveRxTextForBand(QString const &band) {
    if (m_activityStoreDisabled) {
        return;
    }
    if (!m_activityBandConfirmed && band == m_activityBand &&
        m_activityStartupTimer.isValid() &&
        !m_activityStartupTimer.hasExpired(60 * 1000)) {
        m_rxTextDirtyBands.insert(band);
        return;
    }
    if (!m_activitySeeded.contains(band)) {
        m_rxTextDirtyBands.insert(band);
        if (band == m_activityBand && !m_activityShuttingDown &&
            activityDB()->isOpen() &&
            (!m_seedRetryTimer.isValid() ||
             m_seedRetryTimer.hasExpired(30 * 1000))) {
            m_seedRetryTimer.start();
            seedActivityForBand(band);
            m_ctx.displayActivity();
        }
        return;
    }

    int const revision = m_ctx.rxTextEdit->document()->revision();
    if (revision == m_rxTextLastSavedRevision &&
        band == m_rxTextLastSavedBand) {
        return;
    }

    activityDB(); // resolve (and possibly recover) before the batch opens
    startActivityBatchIfPending();

    bool ok;
    if (m_ctx.rxTextEdit->document()->isEmpty()) {
        ok = activityDB()->clearRxText(activityConfigId(), band);
    } else {
        ok = activityDB()->saveRxText(activityConfigId(), band,
                                      m_ctx.rxTextEdit->toHtml());
    }

    if (ok) {
        m_rxTextLastSavedRevision = revision;
        m_rxTextLastSavedBand = band;
        m_rxTextDirtyBands.remove(band);
        if (m_rxTextSaveTimer.interval() != 5000) {
            m_rxTextSaveTimer.setInterval(5000);
        }
    } else {
        m_rxTextDirtyBands.insert(band);
        if (!m_activityShuttingDown && band == m_activityBand) {
            m_rxTextSaveTimer.start(60 * 1000);
            if (!m_rxTextSaveMaxTimer.isActive()) {
                m_rxTextSaveMaxTimer.start();
            }
        }
        if (activityDB()->isOpen()) {
            qCWarning(activitystoragecontroller_js8)
                << "could not save RX text for band" << band << ":"
                << lastStoreError();
        }
    }
}

/**
 * @brief Erase the legacy [CallActivity] group and RXActivity key.
 *
 * Master's writeSettings() rewrote both at every close, so a reset
 * genuinely destroyed the old data; leaving them in place would keep a
 * full, readable copy of everything the user asked to erase, and would
 * let an older build or a later un-tick resurrect it.
 */
void ActivityStorageController::purgeLegacyActivityIni() {
    m_ctx.settings->beginGroup("CallActivity");
    m_ctx.settings->remove("");
    m_ctx.settings->endGroup();
    m_ctx.settings->beginGroup("UI_Constructor");
    m_ctx.settings->remove("RXActivity");
    m_ctx.settings->endGroup();
}

/**
 * @brief One-time import of the legacy .ini activity data into
 *        activity.db3, following the inbox_v1 -> inbox_v2 pattern: the
 *        legacy [CallActivity] group and RXActivity key are read once and
 *        left in place for older versions of the software.
 * @return True when the configuration needs no further import.
 *
 * Rows are attributed to the band each record was heard on, via its
 * stored dial; the RX text blob has no per-line frequency, so it goes to
 * the band of the last-known dial. The fire-once marker is a row in the
 * store keyed by the configuration id, so it follows the data it gates;
 * one that cannot be read defers the import rather than risk a re-import.
 *
 * A clone arrives with a marker naming its source in place of an id of
 * its own; the source's rows are copied under the fresh id first, and the
 * marker is removed only once that copy has run, so a failure retries at
 * the next start rather than losing the inherited history. The import is
 * one transaction with the marker inside it: marking a partial import
 * done would lose the failed rows, the ini group never being re-read.
 *
 * A requested reset that cannot run disables the session, and the legacy
 * ini keys are purged even then: they hold a copy of the same activity,
 * which a later reopen or un-tick would otherwise splice back.
 */
bool ActivityStorageController::importLegacyActivityIfNeeded() {
    auto *db = activityDB();
    auto const cloneFrom =
        m_ctx.settings->value(ActivitySettings::ACTIVITY_DB_CLONE_FROM_KEY)
            .toString();
    if (!db->isOpen()) {
        if (m_ctx.config->reset_activity()) {
            m_activityStoreDisabled = true;
            purgeLegacyActivityIni();
        }
        if (!cloneFrom.isEmpty()) {
            m_activityStoreDisabled = true;
            qCWarning(activitystoragecontroller_js8)
                << "clone copy deferred: the activity store is closed";
            m_ctx.showStatusMessage(
                tr("Activity database unavailable - the cloned "
                   "configuration's history will be copied at the next "
                   "start"));
        }
        return false;
    }

    auto const config = activityConfigId();

    if (!cloneFrom.isEmpty()) {
        if (m_ctx.config->reset_activity()) {
            m_ctx.settings->remove(
                ActivitySettings::ACTIVITY_DB_CLONE_FROM_KEY);
        } else if (db->copyConfig(cloneFrom, config)) {
            m_ctx.settings->remove(
                ActivitySettings::ACTIVITY_DB_CLONE_FROM_KEY);
            qCDebug(activitystoragecontroller_js8)
                << "copied stored activity of configuration" << cloneFrom
                << "into" << config;
        } else {
            qCWarning(activitystoragecontroller_js8)
                << "could not copy the cloned configuration's activity:"
                << db->error();
            m_ctx.showStatusMessage(
                tr("Could not copy the cloned configuration's activity - "
                   "activity will not be saved this session (%1)")
                    .arg(db->error()));
            m_activityStoreDisabled = true;
            return false;
        }
    }

    if (m_ctx.config->reset_activity()) {
        if (!db->clearConfig(config)) {
            qCWarning(activitystoragecontroller_js8)
                << "could not reset stored activity:" << db->error();
            m_ctx.showStatusMessage(
                tr("Could not reset stored activity - activity will not "
                   "be saved this session (%1)")
                    .arg(db->error()));
            m_activityStoreDisabled = true;
            purgeLegacyActivityIni();
            return false;
        }

        purgeLegacyActivityIni();
        if (!db->markImported(config)) {
            qCWarning(activitystoragecontroller_js8)
                << "could not record the import marker:" << db->error();
        }
        return true;
    }

    bool probeOk = false;
    if (db->hasImported(config, &probeOk)) return true;
    if (!probeOk) {
        qCWarning(activitystoragecontroller_js8)
            << "could not read the import marker:" << db->error();
        return false;
    }

    m_ctx.settings->beginGroup("Common");
    auto const lastDial =
        m_ctx.settings
            ->value("DialFreq", QVariant::fromValue<Radio::Frequency>(
                                    m_ctx.defaultDial))
            .value<Radio::Frequency>();
    m_ctx.settings->endGroup();
    auto const fallbackBand = m_ctx.config->bands()->find(lastDial);

    if (!db->begin()) {
        qCWarning(activitystoragecontroller_js8)
            << "could not start legacy activity import:" << db->error();
        return false;
    }

    bool allOk = true;
    int imported = 0;
    m_ctx.settings->beginGroup("CallActivity");
    foreach (auto call, m_ctx.settings->allKeys()) {
        auto values = m_ctx.settings->value(call).toMap();

        ActivityDB::CallRecord r;
        r.callsign = call;
        r.snr = values.value("snr", -64).toInt();
        r.grid = values.value("grid", "").toString();
        r.dial = values.value("dial", 0).value<Radio::Frequency>();
        r.offset = values.value("freq", 0).toInt();
        r.tdrift = values.value("tdrift", 0).toFloat();
        r.ackTimestamp = values.value("ackTimestamp").toDateTime();
        r.utcTimestamp = values.value("utcTimestamp").toDateTime();
        r.submode = values.value("submode", Varicode::JS8CallNormal).toInt();

        auto band = m_ctx.config->bands()->find(r.dial);
        if (band.isEmpty() && r.dial == 0) {
            band = fallbackBand;
        }

        if (db->upsertCall(config, band, r)) {
            ++imported;
        } else {
            allOk = false;
        }
    }
    m_ctx.settings->endGroup();

    m_ctx.settings->beginGroup("UI_Constructor");
    auto const html = m_ctx.settings->value("RXActivity", "").toString();
    m_ctx.settings->endGroup();

    if (!html.isEmpty()) {
        bool haveOk = false;
        auto const have = db->loadRxText(config, fallbackBand, &haveOk);
        if (haveOk && have.isEmpty()) {
            allOk = db->saveRxText(config, fallbackBand, html) && allOk;
        } else if (!haveOk) {
            allOk = false;
        }
    }

    allOk = db->markImported(config) && allOk;
    if (!allOk || !db->commit()) {
        db->rollback();
        qCWarning(activitystoragecontroller_js8)
            << "legacy activity import failed - will retry on next start:"
            << db->error();
        return false;
    }

    qCDebug(activitystoragecontroller_js8)
        << "imported" << imported << "legacy call activity rows into"
        << activityPath();
    return true;
}

/**
 * @brief Storage half of the "Clear All Activity" action.
 *
 * Called after the window has cleared the panes: clearActivity()'s inbox
 * refresh re-persists every unread sender and would otherwise repopulate
 * the store the wipe had just emptied.
 *
 * "All" spans every band and the session's RAM band caches, not just the
 * bucket on screen: dropping only the current band's rows would let the
 * others reload as if never cleared, and a hop back to a previously
 * visited band would restore its pre-clear snapshot for the next
 * debounced save to write straight back into the store. Every bucket is
 * then marked unseeded - on success because the store is empty, on
 * failure because the panes no longer hold the stored history and a save
 * would destroy it. A session whose store is disabled empties the panes
 * and says so, rather than reporting a delete that did not happen.
 */
void ActivityStorageController::clearAllActivity() {
    if (m_activityStoreDisabled) {
        m_ctx.showStatusMessage(
            tr("Activity is not being saved this session - the stored "
               "history was not cleared"));
    }
    bool const stored = m_activityStoreDisabled ||
                        activityDB()->clearConfig(activityConfigId());
    if (!stored) {
        qCWarning(activitystoragecontroller_js8)
            << "clear all activity failed:" << lastStoreError();
        m_ctx.showStatusMessage(tr("Could not clear stored activity (%1)")
                                    .arg(lastStoreError()));
        m_rxTextSaveTimer.stop();
        m_rxTextSaveMaxTimer.stop();
        m_seedRetryTimer.start();
    }

    purgeLegacyActivityIni();

    m_ctx.clearBandCaches();
    m_rxTextDirtyBands.clear();
    m_activitySeeded.clear();
    m_rxTextLegacyShown = false;
    m_rxTextLegacyBand.clear();
    m_rxTextLegacyBlocks = 0;
    m_rxTextLastSavedRevision = m_ctx.rxTextEdit->document()->revision();
    m_rxTextLastSavedBand = m_activityBand;
}

/**
 * @brief The "Clear RX Activity" action, store and pane.
 *
 * The store is cleared before the pane: if the delete fails the pane
 * keeps its text, so no later seed can splice the stored history back in
 * and re-persist it, silently undoing the clear. On that failure the
 * bucket is forced unseeded before saves resume - the pane no longer
 * holds the stored text, so a save would overwrite it - and the debounce
 * the pane clear arms is stopped, or its retry would seed the bucket and
 * put the "cleared" text back on screen. A session whose store is
 * disabled clears the pane and says so, rather than reporting a delete
 * that did not happen.
 */
void ActivityStorageController::clearRxActivity() {
    if (m_activityStoreDisabled) {
        m_ctx.showStatusMessage(
            tr("Activity is not being saved this session - the stored "
               "history was not cleared"));
    }
    bool const stored =
        m_activityStoreDisabled ||
        activityDB()->clearRxText(activityConfigId(), m_activityBand);
    if (!stored) {
        qCWarning(activitystoragecontroller_js8)
            << "clear RX activity failed:" << lastStoreError();
        m_ctx.showStatusMessage(tr("Could not clear stored RX text (%1)")
                                    .arg(lastStoreError()));
        m_activitySeeded.remove(m_activityBand);
        m_rxTextSaveTimer.stop();
        m_rxTextSaveMaxTimer.stop();
        m_seedRetryTimer.start();
    }
    if (m_rxTextLegacyShown && m_activityBand == m_rxTextLegacyBand) {
        m_rxTextLegacyShown = false;
        m_rxTextLegacyBand.clear();
        m_rxTextLegacyBlocks = 0;
    }
    // clears unconditionally - this is also the compose box's way out
    m_ctx.clearRxPane();
    m_rxTextLastSavedRevision = m_ctx.rxTextEdit->document()->revision();
    m_rxTextLastSavedBand = m_activityBand;
    m_rxTextDirtyBands.remove(m_activityBand);
}

/**
 * @brief The "Clear Call Activity" action, store and pane.
 *
 * Bucket-scoped by design: a row displayed here but stored under another
 * band (its own dial's band - e.g. a decode that completed just after a
 * QSY) is that band's history, which this action cannot see and must not
 * destroy. Unread inbox senders reappear regardless - they are
 * re-synthesized from inbox.db3, the source of truth for unread mail.
 *
 * On a failed delete the bucket is deliberately not un-seeded: the RX
 * pane is untouched and still contains this bucket's stored text, so
 * forcing a re-seed would splice that history in a second time. The rows
 * simply return at the next session, which the message above reports. A
 * session whose store is disabled clears the pane and says so, rather
 * than reporting a delete that did not happen.
 */
void ActivityStorageController::clearCallActivity() {
    if (m_activityStoreDisabled) {
        m_ctx.showStatusMessage(
            tr("Activity is not being saved this session - the stored "
               "history was not cleared"));
    }
    if (!m_activityStoreDisabled &&
        !activityDB()->deleteCalls(activityConfigId(), m_activityBand)) {
        qCWarning(activitystoragecontroller_js8)
            << "clear call activity failed:" << lastStoreError();
        m_ctx.showStatusMessage(
            tr("Could not clear stored call activity (%1)")
                .arg(lastStoreError()));
    }
    m_ctx.clearCallActivityPane();
}

/**
 * @brief Drop one station's stored row for the bucket on screen.
 * @param call The callsign the operator removed from the table.
 *
 * Bucket-scoped: only the on-screen bucket's stored row is deleted. A row
 * the same station holds on another band is that band's history, which
 * this context cannot see and must not destroy; an unread inbox sender
 * reappears regardless, re-synthesized from inbox.db3. Rows are stored
 * under the trimmed callsign. A closed store is reported as such, rather
 * than as the row being filed under another band; that state is read
 * before the delete, so a delete whose own failure closes the handle
 * still reports only the removal. That message is read off the handle
 * directly, as endBatch() does, so a third-strike self-close cannot add
 * the accessor's own status message on top of this one.
 */
void ActivityStorageController::removeStoredCall(QString const &call) {
    if (m_activityStoreDisabled || !m_activityBandLoaded) {
        return;
    }
    bool const wasOpen = activityDB()->isOpen();
    if (activityDB()->deleteCall(activityConfigId(), m_activityBand,
                                 call.trimmed())) {
        return;
    }
    if (!wasOpen) {
        m_ctx.showStatusMessage(
            tr("Activity database unavailable - %1 was not removed from "
               "stored activity")
                .arg(call));
        return;
    }
    auto const why = lastStoreError();
    m_ctx.showStatusMessage(
        why.isEmpty()
            ? tr("%1 is stored under another band and will return there")
                  .arg(call)
            : tr("Could not remove %1 from stored activity (%2)")
                  .arg(call, why));
}

/**
 * @brief Final flush of everything the session has not written yet.
 *
 * Covers anything the debounced save timer has not written. A bucket that
 * was never seeded - the store was unusable at its first visit - gets one
 * last seed attempt first, because saves are suppressed while unseeded
 * and the whole session's RX text would otherwise be discarded, where the
 * legacy ini path always wrote it. m_activityShuttingDown is then set so
 * that the flush cannot re-enter the seed's own retry, and its table
 * rebuild, while the window is tearing down.
 *
 * Bands whose text never reached the store - visited while it was down,
 * or left before a seed succeeded - survive only in the RAM band cache,
 * and this is their last chance. A seeded bucket's cached document
 * already contains that bucket's stored history, so it replaces the
 * stored row; an unseeded one does not, so it is appended to whatever is
 * stored rather than replacing it.
 */
void ActivityStorageController::flushOnClose() {
    if (!m_activitySeeded.contains(m_activityBand) &&
        !m_activityStoreDisabled && activityDB()->isOpen()) {
        seedActivityForBand(m_activityBand);
    }
    m_activityShuttingDown = true;
    m_activityBandConfirmed = true;
    saveRxTextForBand(m_activityBand);

    if (!m_activityStoreDisabled) {
        foreach (auto const &dirty, m_rxTextDirtyBands) {
            if (dirty == m_activityBand ||
                !m_ctx.rxTextBandCache->contains(dirty)) {
                continue;
            }
            if (!m_activityBandConfirmedBands.contains(dirty)) {
                continue;
            }
            auto cached = m_ctx.rxTextBandCache->value(dirty);
            QTextDocument cachedDoc;
            cachedDoc.setHtml(cached);
            if (m_rxTextLegacyShown && dirty == m_rxTextLegacyBand) {
                cached = htmlBelowLegacyCopy(&cachedDoc);
                if (cached.isEmpty()) {
                    continue;
                }
                cachedDoc.setHtml(cached);
            }
            if (cachedDoc.isEmpty()) {
                // a cleared pane serialises to a full HTML skeleton
                activityDB()->clearRxText(activityConfigId(), dirty);
                continue;
            }

            if (m_activitySeeded.contains(dirty)) {
                activityDB()->saveRxText(activityConfigId(), dirty,
                                         cached);
                continue;
            }

            bool ok = false;
            auto const storedHtml =
                activityDB()->loadRxText(activityConfigId(), dirty, &ok);
            if (!ok) {
                continue;
            }
            if (storedHtml.isEmpty()) {
                activityDB()->saveRxText(activityConfigId(), dirty,
                                         cached);
                continue;
            }
            QTextDocument doc;
            doc.setHtml(storedHtml);
            QTextCursor cursor(&doc);
            cursor.movePosition(QTextCursor::End);
            if (cursor.block().length() > 1) {
                cursor.insertBlock();
            }
            cursor.insertHtml(cached);
            activityDB()->saveRxText(activityConfigId(), dirty,
                                     doc.toHtml());
        }
    }
}

Q_LOGGING_CATEGORY(activitystoragecontroller_js8,
                   "activitystoragecontroller.js8", QtWarningMsg)
