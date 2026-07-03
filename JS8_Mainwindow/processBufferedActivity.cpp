/**
 * @file processBufferedActivity.cpp
 * @brief member function of the UI_Constructor class
 *  processes buffered MSG's in m_messageBuffer
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::processBufferedActivity() {
    if (m_messageBuffer.isEmpty())
        return;

    foreach (auto freq, m_messageBuffer.keys()) {
        auto buffer = m_messageBuffer[freq];

        // check to make sure we empty old buffers by getting the latest
        // timestamp and checking to see if it's older than one minute.
        auto dt = DriftingDateTime::currentDateTimeUtc().addDays(-1);
        if (buffer.cmd.utcTimestamp.isValid()) {
            dt = qMax(dt, buffer.cmd.utcTimestamp);
        }
        if (!buffer.compound.isEmpty()) {
            dt = qMax(dt, buffer.compound.last().utcTimestamp);
        }
        if (!buffer.msgs.isEmpty()) {
            dt = qMax(dt, buffer.msgs.last().utcTimestamp);
        }
        
        int ageSecs = dt.secsTo(DriftingDateTime::currentDateTimeUtc());

        // if the buffer has messages older than 1 minute, and we still haven't
        // closed it, let's mark it as the last frame
        if (ageSecs > 60 && !buffer.msgs.isEmpty()) {
            buffer.msgs.last().bits |= Varicode::JS8CallLast;
        }

        // but, if the buffer is older than 1.5 minutes, and we still haven't
        // closed it, just remove it and skip
        if (ageSecs > 90) {
            m_messageBuffer.remove(freq);
            continue;
        }

        // if the buffer has no messages, skip
        if (buffer.msgs.isEmpty()) {
            continue;
        }

        // if the buffered message hasn't seen the last message, skip
        if ((buffer.msgs.last().bits & Varicode::JS8CallLast) !=
            Varicode::JS8CallLast) {
            continue;
        }

        QString message;
        foreach (auto part, buffer.msgs) {
            message.append(part.text);
        }
        message = Varicode::rstrip(message);

        QString checksum;

        bool valid = false;

        if (Varicode::isCommandBuffered(buffer.cmd.cmd)) {
            int checksumSize = Varicode::isCommandChecksumed(buffer.cmd.cmd);

            if (checksumSize == 32) {
                message = Varicode::lstrip(message);
                checksum = message.right(6);
                message = message.left(message.length() - 7);
                valid = Varicode::checksum32Valid(checksum, message);
            } else if (checksumSize == 16) {
                message = Varicode::lstrip(message);
                checksum = message.right(3);
                message = message.left(message.length() - 4);
                valid = Varicode::checksum16Valid(checksum, message);
            } else if (checksumSize == 0) {
                valid = true;
            }
        } else {
            valid = true;
        }

        if (valid) {
            buffer.cmd.bits |= Varicode::JS8CallLast;
            buffer.cmd.text = message;
            buffer.cmd.isBuffered = true;
            m_rxCommandQueue.append(buffer.cmd);
        } else {
            qCDebug(mainwindow_js8)
                << "Buffered message failed checksum...discarding";
            qCDebug(mainwindow_js8) << "Checksum:" << checksum;
            qCDebug(mainwindow_js8) << "Message:" << message;
        }

        // regardless of valid or not, remove the "complete" buffered message
        // from the buffer cache
        m_messageBuffer.remove(freq);
        m_lastClosedMessageBufferOffset = freq;
    }
}
