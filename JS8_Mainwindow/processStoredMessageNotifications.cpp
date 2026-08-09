/** @file
 * @brief member function of the UI_Constructor class
 *  periodically checks pending (STORE) inbox messages against recent call
 *  activity, and transmits a directed "<CALL> MSG ID <mid>" notification when
 *  the recipient is heard on frequency rather than waiting for them to send an HB
 *  or QUERY MSGS and possibly miss our reply. Re-notification for a given message id
 *  is throttled to no more than once every 24 hours via MsgNotifyDB.
 */

#include "JS8_Main/MsgNotifyDB.h"
#include "JS8_UI/mainwindow.h"

namespace {
constexpr int RECENCY_WINDOW_SECS  = 15 * 60;        // "seen" window - callsign must be recent
constexpr int RENOTIFY_WINDOW_SECS = 12 * 60 * 60;   // re-notify throttle set to every 12hrs
constexpr int MAX_PENDING_SCANNED  = 1000;           // sanity bound, likely not necessary
} // namespace

void UI_Constructor::processStoredMessageNotifications() {
    // Don't transmit unsolicited traffic if the operator has autoreply
    // disabled - this feature only nudges along replies that the automatic
    // system would have sent anyway, given an HB or QUERY MSGS.
    if (!ui->actionModeAutoreply->isChecked()) {
        return;
    }

    // Never step on an in-progress transmission or queue.
    if (isMessageQueuedForTransmit() || !m_txMessageQueue.isEmpty()) {
        return;
    }

    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return;
    }

    // All pending (undelivered) directed messages, any recipient. Mirrors
    // the pattern used by getNextMessageIdForCallsign() / refreshInboxCounts().
    auto pending =
        inbox.values("STORE", "$.params.TO", "%", 0, MAX_PENDING_SCANNED);
    if (pending.isEmpty()) {
        return;
    }

    MsgNotifyDB notifyDb(msgNotifyPath());
    if (!notifyDb.open()) {
        qCDebug(mainwindow_js8)
            << "MsgNotifyDB failed to open:" << notifyDb.error();
        return;
    }

    auto const now = DriftingDateTime::currentDateTimeUtc();

    foreach (auto pair, pending) {
        int const msgId = pair.first;
        auto const params = pair.second.params();

        auto const to = params.value("TO").toString().trimmed();
        if (to.isEmpty()) {
            continue;
        }

        // Group messages already get surfaced via HB/QUERY MSGS replies to
        // whoever HBs the group; skip them for auto notification.
        if (to.startsWith("@")) {
            continue;
        }

        // Is the recipient (or their base callsign) recently heard?
        bool seen = false;
        if (m_callActivity.contains(to) &&
            m_callActivity[to].utcTimestamp.secsTo(now) <=
                RECENCY_WINDOW_SECS) {
            seen = true;
        } else {
            auto const base = Radio::base_callsign(to);
            if (m_callActivity.contains(base) &&
                m_callActivity[base].utcTimestamp.secsTo(now) <=
                    RECENCY_WINDOW_SECS) {
                seen = true;
            }
        }

        if (!seen) {
            continue;
        }

        // Throttle: don't re-notify inside the 24h window.
        auto const lastSent = notifyDb.getLastSent(msgId);
        if (lastSent.isValid() &&
            lastSent.secsTo(now) < RENOTIFY_WINDOW_SECS) {
            continue;
        }

        qCDebug(mainwindow_js8)
            << "opportunistic stored message notification to" << to
            << "for msg id" << msgId;

        enqueueMessage(PriorityNormal,
                       QString("%1 ACK MSG ID %2").arg(to).arg(msgId), -1,
                       nullptr);

        notifyDb.upsertSent(msgId, to, now);

        // One notification per 15 minute sweep is enough - avoid bursting several
        // transmissions back to back and dominating the transmit queue.
        break;
    }
}
