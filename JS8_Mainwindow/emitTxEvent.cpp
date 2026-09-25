/**
 * @file emitTxEvent.cpp
 * @brief Member function of the UI_Constructor class.
 * Sends the TX.START and TX.END API events that bracket one transmitted
 * message.
 */

#include "JS8_UI/mainwindow.h"

/**
 * @brief Sends TX.START at a message's first keyed frame and TX.END when
 *        the message ends or is halted, one of each per message.
 * @param start true at the first keyed frame, false when the frame queue
 *        is reset.
 * @note API 3.1+
 */
void UI_Constructor::emitTxEvent(bool start) {
    if (m_txEventStarted == start) {
        return;
    }
    m_txEventStarted = start;

    sendNetworkMessage(
        start ? "TX.START" : "TX.END", "",
        {
            {"_ID", QVariant(-1)},
            {"UTC",
             QVariant(
                 DriftingDateTime::currentDateTimeUtc().toMSecsSinceEpoch())},
        });
}
