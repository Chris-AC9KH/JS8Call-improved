

/** \file
 * @brief member function of the UI_Constructor class
 *  process Rx queue activity
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::processRxActivity() {
    if (m_rxActivityQueue.isEmpty()) {
        return;
    }

    int freqOffset = freq();

    qCDebug(mainwindow_js8)
        << m_messageBuffer.count() << "message buffers open";

    // one transaction for the whole drain - each logged station persists,
    // and a busy cycle would otherwise pay one autocommit per row
    m_activityStorage->beginBatch();

    while (!m_rxActivityQueue.isEmpty()) {
        ActivityDetail d = m_rxActivityQueue.dequeue();

        if (canSendNetworkMessage()) {
            sendNetworkMessage(
                "RX.ACTIVITY", d.text,
                {{"_ID", QVariant(-1)},
                 {"FREQ", QVariant(d.dial + d.offset)},
                 {"DIAL", QVariant(d.dial)},
                 {"OFFSET", QVariant(d.offset)},
                 {"SNR", QVariant(d.snr)},
                 {"SPEED", QVariant(d.submode)},
                 {"TDRIFT", QVariant(d.tdrift)},
                 {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())},
                 {"BITS", QVariant(d.bits)}});
        }

        // use the actual frequency and check its delta from our current
        // frequency meaning, if our current offset is 1502 and the d.freq is
        // 1492, the delta is <= 10;
        bool shouldDisplay =
            abs(d.offset - freqOffset) <= JS8::Submode::rxThreshold(d.submode);

        int prevOffset = d.offset;
        if (hasExistingMessageBuffer(d.submode, d.offset, false, &prevOffset) &&
            ((m_messageBuffer[prevOffset].cmd.to == m_config.my_callsign()) ||
             // (isAllCallIncluded(m_messageBuffer[prevOffset].cmd.to))     ||
             // // uncomment this if we want to incrementally print allcalls
             (isGroupCallIncluded(m_messageBuffer[prevOffset].cmd.to)))) {
            d.isBuffered = true;
            shouldDisplay = true;

            if (!m_messageBuffer[prevOffset].compound.isEmpty()) {
                // qCDebug(mainwindow_js8) << "should display compound too
                // because at this point it hasn't been displayed" <<
                // m_messageBuffer[prevOffset].compound.last().call;

                auto lastCompound = m_messageBuffer[prevOffset].compound.last();

                // fixup compound call incremental text
                d.text = QString("%1: %2").arg(lastCompound.call).arg(d.text);
                d.utcTimestamp =
                    qMin(d.utcTimestamp, lastCompound.utcTimestamp);
            }

        } else if (hasClosedExistingMessageBuffer(d.offset)) {
            // incremental typeahead should just be displayed...
            // TODO: should the buffer be reopened?
            shouldDisplay = true;

        } else if (d.isDirected && d.text.contains("<....>")) {
            // if this is a _partial_ directed message, skip until the complete
            // call comes through.
            continue;

        } else if (d.isDirected &&
                   (d.text.contains(": HB ") ||
                    d.text.contains(": @ALLCALL HB"))) { // TODO: HEARTBEAT
            // if this is a heartbeat, process elsewhere...
            continue;
        }

        // if this is the first data frame of a standard message, parse the
        // first word callsigns and spot them :)
        if ((d.bits & Varicode::JS8CallFirst) == Varicode::JS8CallFirst &&
            !d.isDirected && !d.isCompound) {
            auto calls = Varicode::parseCallsigns(d.text);
            if (!calls.isEmpty()) {
                auto theirCall = calls.first();
                if (d.text.startsWith(theirCall) &&
                    d.text.mid(theirCall.length(), 1) == ":") {
                    CallDetail cd = {};
                    cd.call = theirCall;
                    cd.dial = d.dial;
                    cd.offset = d.offset;
                    cd.snr = d.snr;
                    cd.bits = d.bits;
                    cd.tdrift = d.tdrift;
                    cd.utcTimestamp = d.utcTimestamp;
                    cd.submode = d.submode;
                    logCallActivity(cd, true);
                }
            }
        }

        // TODO: incremental printing of directed messages
        // Display if:
        // 1) this is a directed message header "to" us and should be
        // buffered... 2) or, this is a buffered message frame for a buffer with
        // us as the recipient.

        if (!shouldDisplay) {
            continue;
        }

        bool isFirst =
            (d.bits & Varicode::JS8CallFirst) == Varicode::JS8CallFirst;
        bool isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;

        // if we're the last message, let's display our EOT character
        if (isLast) {
            d.text = QString("%1 %2 ")
                         .arg(Varicode::rstrip(d.text))
                         .arg(m_config.eot());
        }

        // log it to the display!
        displayTextForFreq(d.text, d.offset, d.utcTimestamp, false, isFirst,
                           isLast);

        // If we've received a message to be displayed, we should no longer call
        // CQ.
        if (m_cq_loop->isActive()) {
            qCDebug(mainwindow_js8)
                << "Canceling calling CQ loop to priorize incoming messages.";
            m_cq_loop->onLoopCancel();
        }

        if (isLast) {
            clearOffsetDirected(d.offset);
        }

        if (isLast && !d.isBuffered) {
            // buffered commands need the rxFrameBlockNumbers cache so it can
            // fixup its display all other "last" data frames can clear the
            // rxFrameBlockNumbers cache so the next message will be on a new
            // line.
            m_rxFrameBlockNumbers.remove(d.offset);
        }
    }

    m_activityStorage->endBatch();

#if 0
    // TODO: this works but should also print in the rx window.
    foreach(auto offset, m_bandActivity.keys()){
        if(seen.contains(offset)){
            continue;
        }

        if(m_bandActivity[offset].isEmpty()){
            continue;
        }

        auto last = m_bandActivity[offset].last();
        if((last.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast){
            continue;
        }

        auto now = DriftingDateTime::currentDateTimeUtc();
        if(last.utcTimestamp.secsTo(now) < m_TRperiod){
            continue;
        }

        ActivityDetail d = {};
        d.text = " . . . ";
        d.utcTimestamp = now;
        d.snr = -99;

        m_bandActivity[offset].append(d);
    }
#endif
}
