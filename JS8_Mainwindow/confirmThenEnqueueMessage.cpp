/**
 * @file confirmThenEnqueueMessage.cpp
 * @brief Member function of the UI_Constructor class.
 * Handles autoreply confirmation via both local UI notification and
 * the network API, ensuring the operator and external clients are
 * kept in sync on pending autoreply transmissions.
 * @ingroup API
 */

#include "../JS8_UI/mainwindow.h"

/**
 * @brief Presents a local confirmation dialog and fires a network confirmation request.
 *
 * Called from the decoder thread; all Qt and network operations are marshalled
 * to the GUI thread via QMetaObject::invokeMethod. A SelfDestructMessageBox is
 * shown to the local operator while simultaneously sending
 * STATION.AUTOREPLY_CONFIRM_REQUEST to any connected API clients.
 *
 * Whichever resolves first — local operator response or timeout — cancels the
 * other. On acceptance, enqueueMessage() is called and
 * STATION.AUTOREPLY_CONFIRM_ACCEPTED is sent. On rejection or timeout,
 * STATION.AUTOREPLY_CONFIRM_EXPIRED is sent.
 *
 * @param timeout  Seconds before the confirmation auto-rejects.
 * @param priority Transmission priority passed to enqueueMessage().
 * @param message  The autoreply message text shown to the operator and sent over the API.
 * @param offset   Frequency offset passed to enqueueMessage().
 * @param c        Callback passed to enqueueMessage() on acceptance.
 *
 * @note Requires @ref PendingConfirmation::box to be present in the struct.
 * @note API 2.6+
 */
void UI_Constructor::confirmThenEnqueueMessage(int timeout, int priority,
                                               QString message, int offset,
                                               Callback c) {
    // CRITICAL: called from decoder thread → QTimer/sendNetworkMessage must run in GUI thread
    QMetaObject::invokeMethod(this, [this, timeout, priority, message, offset, c]() {
        int id = m_nextConfirmId++;

        // Timer auto-reject after timeout
        QTimer *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (m_pendingConfirmations.contains(id)) {
                auto pc = m_pendingConfirmations.take(id);
                pc.timer->deleteLater();
                sendNetworkMessage("STATION.AUTOREPLY_CONFIRM_EXPIRED", "",
                    {{"_ID", QVariant(-1)},
                     {"CONFIRM_ID", QVariant(id)},
                     {"MESSAGE", QVariant(pc.message)}});
                pc.box->close();
            }
        });

        // Local popup — same timeout as the network timer
        SelfDestructMessageBox *m = new SelfDestructMessageBox(
            timeout, "Autoreply Confirmation Required",
            QString("A transmission is queued for autoreply:\n\n%1\n\nWould you "
                    "like to send this transmission?")
                .arg(message),
            QMessageBox::Question, QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No, false, this);
        m->setWindowModality(Qt::NonModal);

        // Build and insert the PendingConfirmation before any signals can fire
        PendingConfirmation pc;
        pc.id       = id;
        pc.priority = priority;
        pc.message  = message;
        pc.offset   = offset;
        pc.callback = c;
        pc.timer    = timer;
        pc.box      = m;
        m_pendingConfirmations.insert(id, pc);

        // Local operator response
        connect(m, &SelfDestructMessageBox::finished, this,
            [this, m, id](int) {
                m->deleteLater();

                if (!m_pendingConfirmations.contains(id))
                    return; // already resolved by timer

                auto pc = m_pendingConfirmations.take(id);
                pc.timer->stop();
                pc.timer->deleteLater();

                if (m->result() == QMessageBox::Yes) {
                    enqueueMessage(pc.priority, pc.message, pc.offset, pc.callback);
                    sendNetworkMessage("STATION.AUTOREPLY_CONFIRM_ACCEPTED", pc.message,
                        {{"_ID", QVariant(-1)},
                         {"CONFIRM_ID", QVariant(id)},
                         {"PRIORITY", QVariant(pc.priority)},
                         {"OFFSET", QVariant(pc.offset)}});
                } else {
                    sendNetworkMessage("STATION.AUTOREPLY_CONFIRM_EXPIRED", "",
                        {{"_ID", QVariant(-1)},
                         {"CONFIRM_ID", QVariant(id)},
                         {"MESSAGE", QVariant(pc.message)}});
                }
            });

        // Both show() and the network message go out after the map is populated
        m->show();
        timer->start(timeout * 1000);

        sendNetworkMessage("STATION.AUTOREPLY_CONFIRM_REQUEST", message,
            {{"_ID", QVariant(-1)},
             {"CONFIRM_ID", QVariant(id)},
             {"PRIORITY", QVariant(priority)},
             {"OFFSET", QVariant(offset)},
             {"TIMEOUT", QVariant(timeout)}});
    });
}
