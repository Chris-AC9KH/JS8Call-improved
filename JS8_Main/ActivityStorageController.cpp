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
/// @brief True if lhs is valid and strictly newer than rhs (invalid = oldest).
bool newerThan(QDateTime const &lhs, QDateTime const &rhs) {
    if (!lhs.isValid()) return false;
    if (!rhs.isValid()) return true;
    return rhs < lhs;
}

/// @brief Carry a displaced row's grid/ACK/CQ enrichment onto the winner.
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

/// @brief Wire up the debounced write-on-change persistence of the RX pane.
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

/// @brief Start the grace period for an as-yet unconfirmed bucket.
void ActivityStorageController::beginStartupGrace() {
    m_activityStartupTimer.start();
}

/**
 * @brief Show the legacy ini RX text on a degraded or disabled start.
 *
 * Display only. Records the copy's extent in m_rxTextLegacyBand /
 * m_rxTextLegacyBlocks so later saves/seeds treat only session text
 * below it as the session's own.
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
 * Sole path for changing the displayed bucket (issue #267). If the
 * outgoing bucket is still just the startup guess, its cache is dropped
 * rather than parked, since it may hold activity heard on the
 * newly-reported band.
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
 * @param paneReloaded True when panes were reloaded from the RAM band
 *        caches; false when re-entering the bucket already on screen.
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

/// @brief Path of the per-band activity store, beside the message inbox.
QString ActivityStorageController::activityPath() const {
    return QDir::toNativeSeparators(
        m_ctx.config->writeable_data_dir().absoluteFilePath("activity.db3"));
}

/**
 * @brief The activity store, opened on first use.
 *
 * Reopen attempts are throttled rather than retried per call. Callers
 * may always use the returned pointer: every method is a no-op while
 * closed. Never replaced mid-batch.
 */
ActivityDB *ActivityStorageController::activityDB() {
    if (m_activityDB && !m_activityDB->isOpen() &&
        !m_activityDBRetryTimer.isValid()) {
        m_activityDBRetryTimer.start();
        qCWarning(activitystoragecontroller_js8)
            << "activity store closed after repeated failures:"
            << m_activityDB->error();
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

/// @brief Open a write batch; nested batches share one lazily-started transaction.
void ActivityStorageController::beginBatch() {
    if (m_activityBatchDepth == 0) {
        m_activityBatchBeginFailed = false;
    }
    ++m_activityBatchDepth;
}

/// @brief Start the transaction a batch deferred, at its first write.
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
 * Keyed by id rather than name so it survives renames, is purged by
 * "Reset Configuration", and lets a clone diverge from its source after
 * its first-start copy.
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
 * @param fallbackToCurrentBand File a dial-less row under the bucket on
 *        screen; only for manual adds, grid backfills, and qsy()'s
 *        write-back for those same rows.
 *
 * Filed under the band of the row's own dial (not the live band), so
 * post-QSY decodes and inbox senders keep the band they were heard on.
 * If that's a different bucket than the one displayed, the row is merged
 * into that bucket's RAM cache instead of forcing a re-seed.
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
 * Only writes back rows whose own dial belongs to the current band;
 * cross-band stragglers and dial-less RAM-only entries are skipped
 * (dial-less manual adds go through the fallback path instead).
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
 * @brief Merge a bucket's stored history into the session, once per session.
 * @param band The bucket to seed.
 *
 * Newer row wins per callsign; enrichment (grid/ACK/CQ) survives a bare
 * re-hearing. Stored RX text is placed behind the pane's own lines.
 * Saves are suppressed until seeded, and a failed seed retries at the
 * flush cadence, next visit, and after the store reopens.
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
 * Declines (and marks the bucket dirty for the close-time sweep) while
 * it's only the startup guess or still unseeded. An empty pane deletes
 * the stored row rather than storing an empty one. A failed save
 * re-arms the debounce at a 60s back-off until the next success.
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

/// @brief Erase the legacy [CallActivity] group and RXActivity ini key.
void ActivityStorageController::purgeLegacyActivityIni() {
    m_ctx.settings->beginGroup("CallActivity");
    m_ctx.settings->remove("");
    m_ctx.settings->endGroup();
    m_ctx.settings->beginGroup("UI_Constructor");
    m_ctx.settings->remove("RXActivity");
    m_ctx.settings->endGroup();
}

/**
 * @brief One-time import of legacy .ini activity data into activity.db3.
 * @return True when the configuration needs no further import.
 *
 * Rows go to the band of their stored dial; the RX text blob (no
 * per-line frequency) goes to the last-known dial's band. Gated by a
 * fire-once marker row keyed by configuration id, written in the same
 * transaction as the import so a failure retries cleanly. A clone
 * (carrying a source marker instead of its own id) copies its source's
 * rows first.
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
            m_activityStoreDisabled = true;
            return false;
        }
    }

    if (m_ctx.config->reset_activity()) {
        if (!db->clearConfig(config)) {
            qCWarning(activitystoragecontroller_js8)
                << "could not reset stored activity:" << db->error();
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
 * Spans every band, including RAM band caches, not just the bucket on
 * screen. Every bucket is marked unseeded afterward so nothing reloads
 * stale pre-clear history. Called after the window clears its panes, to
 * avoid the inbox refresh repopulating what was just wiped.
 */
void ActivityStorageController::clearAllActivity() {
    bool const stored = m_activityStoreDisabled ||
                        activityDB()->clearConfig(activityConfigId());
    if (!stored) {
        qCWarning(activitystoragecontroller_js8)
            << "clear all activity failed:" << lastStoreError();
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
 * Store is cleared before the pane, so a delete failure leaves the pane
 * intact rather than risking a later seed splicing the stored text back
 * in. On failure the bucket is forced unseeded and its debounce stopped.
 */
void ActivityStorageController::clearRxActivity() {
    if (m_activityStoreDisabled) {
    }
    bool const stored =
        m_activityStoreDisabled ||
        activityDB()->clearRxText(activityConfigId(), m_activityBand);
    if (!stored) {
        qCWarning(activitystoragecontroller_js8)
            << "clear RX activity failed:" << lastStoreError();
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
 * Bucket-scoped: rows stored under another band (e.g. a post-QSY
 * decode) are untouched. Unread inbox senders reappear regardless,
 * re-synthesized from inbox.db3. On failure the bucket is left seeded,
 * since the pane still holds its stored text.
 */
void ActivityStorageController::clearCallActivity() {
    if (!m_activityStoreDisabled &&
        !activityDB()->deleteCalls(activityConfigId(), m_activityBand)) {
        qCWarning(activitystoragecontroller_js8)
            << "clear call activity failed:" << lastStoreError();
    }
    m_ctx.clearCallActivityPane();
}

/**
 * @brief Drop one station's stored row for the bucket on screen.
 * @param call The callsign the operator removed from the table.
 *
 * Bucket-scoped; a row on another band is untouched. Whether the store
 * was open is read before the delete, so a self-close triggered by the
 * delete itself doesn't get misreported as the row being on another band.
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
        return;
    }
}

/**
 * @brief Final flush of everything the session has not written yet.
 *
 * Seeds the current bucket once more if it never seeded, then flushes
 * its RX text. Other dirty bands that only exist in the RAM cache get
 * one last write too: appended to stored text if never seeded, or
 * replacing it if already seeded (since the cached doc then already
 * contains that history).
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
