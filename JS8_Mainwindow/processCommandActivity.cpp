

/** \file
 * @brief member function of the UI_Constructor class
 *  processes JS8 commands
 */

#include "JS8_Main/HBBlockingDB.h"
#include "JS8_UI/mainwindow.h"

void UI_Constructor::processCommandActivity() {
#if 0
    if (!m_txFrameQueue.isEmpty()) {
        return;
    }
#endif

    if (m_rxCommandQueue.isEmpty()) {
        return;
    }

#if 0
    bool processed = false;

    int f = currentFreq();
#endif

    auto now = DriftingDateTime::currentDateTimeUtc();

    while (!m_rxCommandQueue.isEmpty()) {
        auto d = m_rxCommandQueue.dequeue();

        auto selectedCallsign = callsignSelected();
        bool isAllCall = isAllCallIncluded(d.to);
        bool isGroupCall = isGroupCallIncluded(d.to) || d.to == "@APRSIS";

        qCDebug(mainwindow_js8)
            << "try processing command" << d.from << d.to << d.cmd << d.dial
            << d.offset << d.grid << d.extra << isAllCall << isGroupCall;

        // if we need a compound callsign but never got one...skip
        if (d.from == "<....>" || d.to == "<....>") {
            continue;
        }

        // we're only processing a subset of queries at this point
        if (!Varicode::isCommandAllowed(d.cmd)) {
            continue;
        }

        // is this to me?
        bool toMe =
            d.to == m_config.my_callsign().trimmed() ||
            d.to == Radio::base_callsign(m_config.my_callsign()).trimmed();

        // log call activity...
        CallDetail cd = {};
        cd.call = d.from;
        cd.grid = d.grid;
        cd.snr = d.snr;
        cd.dial = d.dial;
        cd.offset = d.offset;
        cd.bits = d.bits;
        cd.ackTimestamp =
            d.text.contains(": ACK") || toMe ? d.utcTimestamp : QDateTime{};
        cd.utcTimestamp = d.utcTimestamp;
        cd.tdrift = d.tdrift;
        cd.submode = d.submode;
        logCallActivity(cd, true);
        logHeardGraph(d.from, d.to);

        // Placeholder for callbacks to be passed to confirmThenEnqueueMessage
        // and enqueueMessage
        Callback callback = nullptr;

        // PROCESS BUFFERED HEARING FOR EVERYONE
        if (d.cmd == " HEARING") {
            // 1. parse callsigns
            // 2. log it to the heard graph
            auto calls = Varicode::parseCallsigns(d.text);
            foreach (auto call, calls) {
                logHeardGraph(d.from, call);
            }
        }

        // PROCESS BUFFERED GRID FOR EVERYONE
        if (d.cmd == " GRID") {
            // 1. parse grids
            // 2. log it to our call activity
            auto grids = Varicode::parseGrids(d.text);
            foreach (auto grid, grids) {
                CallDetail cd = {};
                cd.bits = d.bits;
                cd.call = d.from;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.grid = grid;
                cd.snr = d.snr;
                cd.utcTimestamp = d.utcTimestamp;
                cd.tdrift = d.tdrift;
                cd.submode = d.submode;

                // PROCESS GRID SPOTS TO APRSIS FOR EVERYONE
                /**
                 * APRS relay: inbound `MSG TO:` stores for our local station.
                 * Dedupe within 60s using TO + TEXT + DE <SENDER>.
                 */
                if (d.to == "@APRSIS") {

                    spotAprsGrid(cd.dial, cd.offset, cd.snr, cd.call, cd.grid);
                }

                logCallActivity(cd, true);
            }
        }

        // PROCESS @JS8NET, @APRSIS, AND OTHER GROUP SPOTS FOR EVERYONE
        if (d.to.startsWith("@")) {
            spotCmd(d);
        }

        // PROCESS @APRSIS CMD SPOTS FOR EVERYONE
        if (d.to == "@APRSIS") {
            spotAprsCmd(d);
        }

        // PREPARE CMD TEXT
        // Build baseText without the FROM prefix: "TO CMD EXTRA TEXT [eot]"
        // This is used for the TCP API where FROM is sent as a separate field.
        // The full text (with FROM prefix) is used for file logging and display.
        QStringList baseList = {QString("%1%2").arg(d.to).arg(d.cmd)};

        if (!d.extra.isEmpty()) {
            baseList.append(d.extra);
        }

        if (!d.text.isEmpty()) {
            baseList.append(d.text);
        }

        QString baseText = baseList.join(" ");

        // Append the user-configured eot (end of transmission) character
        // if this is the final frame of the message
        bool isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;
        if (isLast) {
            baseText = QString("%1 %2 ")
                           .arg(Varicode::rstrip(baseText))
                           .arg(m_config.eot());
        }

        // Prepend FROM for file logging and UI display: "FROM: TO CMD EXTRA TEXT [eot]"
        QString text = QString("%1: %2").arg(d.from).arg(baseText);

        // Log to DIRECTED.txt (includes FROM prefix)
        writeMsgTxt(text, d.snr, d.offset);

        // Send to TCP API (FROM is a separate JSON field, so use baseText without prefix)
        if (canSendNetworkMessage()) {
            sendNetworkMessage(
                "RX.DIRECTED", baseText,
                {{"_ID", QVariant(-1)},
                 {"FROM", QVariant(d.from)},
                 {"TO", QVariant(d.to)},
                 {"CMD", QVariant(d.cmd)},
                 {"GRID", QVariant(d.grid)},
                 {"EXTRA", QVariant(d.extra)},
                 {"TEXT", QVariant(baseText)},
                 {"FREQ", QVariant(d.dial + d.offset)},
                 {"DIAL", QVariant(d.dial)},
                 {"OFFSET", QVariant(d.offset)},
                 {"SNR", QVariant(d.snr)},
                 {"SPEED", QVariant(d.submode)},
                 {"TDRIFT", QVariant(d.tdrift)},
                 {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())}});
        }

        // we're only responding to allcalls if we are participating in the
        // allcall group but, don't avoid for heartbeats...those are technically
        // allcalls but are processed differently
        if (isAllCall && m_config.avoid_allcall() && d.cmd != " CQ" &&
            d.cmd != " HB" && d.cmd != " HEARTBEAT") {
            continue;
        }

        // we're only responding to allcall, groupcalls, and our callsign at
        // this point, so we'll end after logging the callsigns we've heard
        if (!isAllCall && !toMe && !isGroupCall) {
            continue;
        }

        ActivityDetail ad = {};
        ad.isLowConfidence = false;
        ad.isDirected = true;
        ad.bits = d.bits;
        ad.dial = d.dial;
        ad.offset = d.offset;
        ad.snr = d.snr;
        ad.text = text;
        ad.utcTimestamp = d.utcTimestamp;

        // we'd be double printing here if were on frequency, so let's be
        // "smart" about this...
        bool shouldDisplay = true;

        // don't display ping allcalls
        if (isAllCall && (d.cmd != " " || ad.text.contains("@HB HEARTBEAT"))) {
            shouldDisplay = false;
        }

        if (shouldDisplay) {
            auto c = ui->textEditRX->textCursor();
            c.movePosition(QTextCursor::End);
            ui->textEditRX->setTextCursor(c);

            // ACKs and SNRs are the most likely source of items to be
            // overwritten (multiple responses at once)... so don't overwrite
            // those (i.e., print each on a new line)
            bool shouldOverwrite = (!d.cmd.contains(" ACK") && !d.cmd.contains(" SNR"));

            if (shouldOverwrite &&
                ui->textEditRX->find(d.utcTimestamp.time().toString(), QTextDocument::FindBackward)) {
                auto candidate = ui->textEditRX->textCursor();
                candidate.select(QTextCursor::BlockUnderCursor);
                QString selectedText = candidate.selectedText();

                // Only treat this as "the same message being redrawn" if it's
                // actually from the same sender - otherwise it's a distinct
                // reply that happens to share a rendered timestamp.
                if (selectedText.contains(QString("%1:").arg(d.from))) {
                    c = ui->textEditRX->textCursor();
                    c.movePosition(QTextCursor::StartOfBlock);
                    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    c.removeSelectedText();
                    c.deletePreviousChar();
                    c.deletePreviousChar();
                }
            }

            // log it to the display!
            displayTextForFreq(ad.text, ad.offset, ad.utcTimestamp, false, true,
                               false);

            /*
            // and send it to the network in case we want to interact with it
            from an external app... if(canSendNetworkMessage()){
                sendNetworkMessage("RX.DIRECTED.ME", ad.text, {
                    {"_ID", QVariant(-1)},
                    {"FROM", QVariant(d.from)},
                    {"TO", QVariant(d.to)},
                    {"CMD", QVariant(d.cmd)},
                    {"GRID", QVariant(d.grid)},
                    {"EXTRA", QVariant(d.extra)},
                    {"TEXT", QVariant(d.text)},
                    {"FREQ", QVariant(ad.dial+ad.offset)},
                    {"DIAL", QVariant(ad.dial)},
                    {"OFFSET", QVariant(ad.offset)},
                    {"SNR", QVariant(ad.snr)},
                    {"SPEED", QVariant(ad.submode)},
                    {"TDRIFT", QVariant(ad.tdrift)},
                    {"UTC", QVariant(ad.utcTimestamp.toMSecsSinceEpoch())}
                });
            }
            */

            if (!isAllCall) {
                // If we've received a message to be displayed,
                // we should no longer call CQ
                if (m_cq_loop->isActive()) {
                    qCDebug(mainwindow_js8)
                        << "Canceling CQ loop to prioritize incoming messages, "
                           "case II";
                    m_cq_loop->onLoopCancel();
                }

                // notification for directed message
                tryNotify("directed");
            }
        }

        // we're only responding to callsigns in our whitelist if we have one
        // defined... make sure the whitelist is empty (no restrictions) or the
        // from callsign or its base callsign is on it
        auto whitelist = m_config.auto_whitelist();
        if (!whitelist.isEmpty() &&
            !(whitelist.contains(d.from) ||
              whitelist.contains(Radio::base_callsign(d.from)))) {
            qCDebug(mainwindow_js8)
                << "skipping command for whitelist" << d.from;
            continue;
        }

        // we'll never reply to a blacklisted callsign or base callsign
        auto blacklist = m_config.auto_blacklist();
        if (!blacklist.isEmpty() &&
            (blacklist.contains(d.from) ||
             blacklist.contains(Radio::base_callsign(d.from)))) {
            qCDebug(mainwindow_js8)
                << "skipping command for blacklist" << d.from;
            continue;
        }

        // if this is an allcall, check to make sure we haven't replied to their
        // allcall recently (in the past ten minutes) that way we never get
        // spammed by allcalls at too high of a frequency
        if (isAllCall && m_txAllcallCommandCache.contains(d.from) &&
            m_txAllcallCommandCache[d.from]->secsTo(now) / 60 < 15) {
            qCDebug(mainwindow_js8)
                << "skipping command for allcall timeout" << d.from;
            continue;
        }

        // don't actually process any automatic message replies while in idle
        if (m_tx_watchdog) {
            qCDebug(mainwindow_js8)
                << "skipping command for idle timeout" << d.from;
            continue;
        }

        // HACK: if this is an autoreply cmd and relay path is populated and cmd
        // is not MSG or MSG TO:, then swap out the relay path
        if (Varicode::isCommandAutoreply(d.cmd) && !d.relayPath.isEmpty() &&
            !d.cmd.startsWith(" MSG") && !d.cmd.startsWith(" QUERY")) {
            d.from = d.relayPath;
        }

        // construct a reply, if needed
        QString reply;
        int priority = PriorityNormal;
        int freq = -1;

        // QUERIED SNR
        if (d.cmd == " SNR?" && !isAllCall) {
            reply = QString("%1 SNR %2")
                        .arg(d.from)
                        .arg(Varicode::formatSNR(d.snr));
        }

        // QUERIED INFO
        else if (d.cmd == " INFO?" && !isAllCall) {
            QString info = m_config.my_info();
            if (info.isEmpty()) {
                continue;
            }

            reply = QString("%1 INFO %2")
                        .arg(d.from)
                        .arg(replaceMacros(info, buildMacroValues(), true));
        }

        // QUERIED ACTIVE
        else if (d.cmd == " STATUS?" && !isAllCall) {
            QString status = m_config.my_status();
            if (status.isEmpty()) {
                continue;
            }

            reply = QString("%1 STATUS %2")
                        .arg(d.from)
                        .arg(replaceMacros(status, buildMacroValues(), true));
        }

        // QUERIED GRID
        else if (d.cmd == " GRID?" && !isAllCall) {
            QString grid = m_config.my_grid();
            if (grid.isEmpty()) {
                continue;
            }

            reply = QString("%1 GRID %2").arg(d.from).arg(grid);
        }

        // QUERIED STATIONS HEARD
        else if (d.cmd == " HEARING?" && !isAllCall) {
            auto calls = m_callActivity.keys();

            std::stable_sort(calls.begin(), calls.end(),
                             [this](QString const &lhs, QString const &rhs) {
                                 return m_callActivity[rhs].utcTimestamp <
                                        m_callActivity[lhs].utcTimestamp;
                             });

            auto const callsignAging = m_config.callsign_aging();
            auto const maxStations = 4;
            auto i = 0;
            QStringList lines;

            for (auto const &call : calls) {
                if (i >= maxStations)
                    break;
                if (call == d.from)
                    continue;

                auto const &cd = m_callActivity[call];

                if (callsignAging &&
                    cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                    continue;
                }

                lines.append(cd.call);
                i++;
            }

            lines.prepend(QString("%1 HEARING").arg(d.from));
            reply = lines.join(' ');
        }

        // PROCESS RELAY
        else if (d.cmd == ">" && !isAllCall && !m_config.relay_off()) {

            // 1. see if there are any more hops to process
            // 2. if so, forward
            // 3. otherwise, display alert & reply dialog

            QString callToPattern = {
                R"(^(?<callsign>\b(?<prefix>[A-Z0-9]{1,4}\/)?(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<suffix>\/[A-Z0-9]{1,4})?(?<type>[> ]))\b)"};
            QRegularExpression re(callToPattern);
            auto text = d.text;
            auto match = re.match(text);

            // if the text starts with a callsign, and relay is not disabled,
            // and this is not a group callsign, then relay.
            if (match.hasMatch() && !isGroupCall) {
                // replace freetext with relayed free text
                if (match.captured("type") != ">") {
                    text = text.replace(match.capturedStart("type"),
                                        match.capturedLength("type"), ">");
                }
                reply = QString("%1 *DE* %2").arg(text).arg(d.from);

                // otherwise, as long as we're not an ACK...alert the user and
                // either send an ACK or Message
            } else if (!d.text.startsWith("ACK")) {

                // parse out the callsign path
                auto calls = parseRelayPathCallsigns(d.from, d.text);

                // put these third party calls in the heard list
                foreach (auto call, calls) {
                    CallDetail cd = {};
                    cd.call = call;
                    cd.snr = -64;
                    cd.dial = d.dial;
                    cd.offset = d.offset;
                    cd.through = d.from;
                    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
                    cd.tdrift = d.tdrift;
                    cd.submode = d.submode;
                    logCallActivity(cd, false);
                }

                d.relayPath = calls.join('>');

                reply = QString("%1 ACK").arg(d.relayPath);

                // check to see if the relay text contains a command that should
                // be replied to instead of an ack.
                QStringList relayedCmds = d.text.split(" ");
                if (!relayedCmds.isEmpty()) {
                    auto first = relayedCmds.first();

                    auto valid = Varicode::isCommandAllowed(first);
                    if (!valid) {
                        first = " " + first;
                        valid = Varicode::isCommandAllowed(first);
                        if (valid) {
                            relayedCmds.removeFirst();
                        }
                    }

                    // HACK: "MSG TO:" should be supported but contains a space
                    // :(
                    if (!relayedCmds.isEmpty()) {
                        if (first == " MSG") {
                            auto second = relayedCmds.first();
                            if (second == "TO:") {
                                first = " MSG TO:";
                                relayedCmds.removeFirst();
                            } else if (second.startsWith("TO:")) {
                                first = " MSG TO:";
                                relayedCmds.replace(0, second.mid(3));
                            }
                        } else if (first == " QUERY") {
                            auto second = relayedCmds.first();
                            if (second == "MSGS" || second == "MSGS?") {
                                first = " QUERY MSGS";
                                relayedCmds.removeFirst();
                            } else if (second == "CALL") {
                                first = " QUERY CALL";
                                relayedCmds.removeFirst();
                            }
                        }
                    }

                    if (Varicode::isCommandAllowed(first) &&
                        Varicode::isCommandAutoreply(first)) {
                        CommandDetail rd = {};
                        rd.bits = d.bits;
                        rd.cmd = first;
                        rd.dial = d.dial;
                        rd.offset = d.offset;
                        rd.from =
                            d.from; // note, MSG and QUERY commands are not set
                                    // with from as the relay path.
                        rd.relayPath = d.relayPath;
                        rd.text = relayedCmds.join(" "); // d.text;
                        rd.to = d.to;
                        rd.utcTimestamp = d.utcTimestamp;

                        m_rxCommandQueue.insert(0, rd);
                        continue;
                    }
                }

#if STORE_RELAY_MSGS_TO_INBOX
                // if we make it here, this is a message
                addCommandToMyInbox(d);
#endif
            }
        }

        // PROCESS MESSAGE STORAGE
        else if (d.cmd == " MSG TO:" && !isAllCall) {

            // store message
            QStringList segs = d.text.split(" ");
            if (segs.isEmpty()) {
                continue;
            }

            auto to = segs.first();
            segs.removeFirst();

            auto text = segs.join(" ").trimmed();

            /**
             * APRS relay: inbound `MSG TO:` stores for our local station.
             * Dedupe within 60s using TO + TEXT + DE <SENDER>.
             */
            if (d.to == "@APRSIS") {
                if (to != m_config.my_callsign().trimmed()) {
                    continue;
                }

                static QRegularExpression aprsDeRe("\\s+DE\\s+(\\S+)\\s*$");
                auto aprsSender = QString("APRS");
                auto deMatch = aprsDeRe.match(text);
                if (deMatch.hasMatch()) {
                    aprsSender = deMatch.captured(1);
                }
                auto dedupeKey =
                    QString("%1|%2|%3").arg(to, text, aprsSender).toUpper();
                auto now = DriftingDateTime::currentDateTimeUtc();
                auto cutoff = now.addSecs(-60);
                auto it = m_aprsRelayDedupCache.begin();
                while (it != m_aprsRelayDedupCache.end()) {
                    if (it.value() < cutoff) {
                        it = m_aprsRelayDedupCache.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (m_aprsRelayDedupCache.contains(dedupeKey)) {
                    continue;
                }

                CommandDetail cd = {};
                cd.bits = d.bits;
                cd.cmd = " MSG ";
                cd.extra = d.extra;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.from = "APRS";
                cd.relayPath = "APRS";
                cd.grid = d.grid;
                cd.snr = d.snr;
                cd.tdrift = d.tdrift;
                cd.text = text;
                cd.to = to;
                cd.utcTimestamp = d.utcTimestamp;
                cd.submode = d.submode;

                addCommandToMyInbox(cd);

                m_aprsRelayDedupCache.insert(dedupeKey, now);
                tryNotify("inbox");
                continue;
            }

            if (m_config.relay_off()) {
                continue;
            }

            auto calls = parseRelayPathCallsigns(d.from, text);
            d.relayPath = calls.join(">");

            CommandDetail cd = {};
            cd.bits = d.bits;
            cd.cmd = d.cmd;
            cd.extra = d.extra;
            cd.dial = d.dial;
            cd.offset = d.offset;
            cd.from = d.from;
            cd.grid = d.grid;
            cd.relayPath = d.relayPath;
            cd.snr = d.snr;
            cd.tdrift = d.tdrift;
            cd.text = text;
            cd.to = Radio::base_callsign(to);
            cd.utcTimestamp = d.utcTimestamp;
            cd.submode = d.submode;

            qCDebug(mainwindow_js8)
                << "storing message to" << to << ":" << text;

            addCommandToStorage("STORE", cd);

            // we haven't replaced the from with the relay path, so we have to
            // use it for the ack if there is one
            reply = QString("%1 ACK").arg(calls.length() > 1 ? d.relayPath
                                                             : d.from);
        }

        // PROCESS AGN
        else if (d.cmd == " AGN?" && !isAllCall && !isGroupCall &&
                 !m_lastTxMessage.isEmpty()) {
            reply = Varicode::rstrip(m_lastTxMessage);
        }

        // PROCESS ACTIVE HEARTBEAT
        // if we have hb mode enabled and auto reply enabled <del>and auto ack
        // enabled and no callsign is selected</del> update: if we're in HB
        // mode, doesn't matter if a callsign is selected.
        else if ((d.cmd == " HB" || d.cmd == " HEARTBEAT") &&
                 canCurrentModeSendHeartbeat() &&
                 ui->actionModeJS8HB->isChecked() &&
                 ui->actionModeAutoreply->isChecked() &&
                 ui->actionHeartbeatAcknowledgements->isChecked()) {
            // do not process HB activity if buffer is not empty, this prevents
            // broken incoming MSG's
            if (!m_messageBuffer.isEmpty()) {
                qCDebug(mainwindow_js8) << "hb paused for messageBuffer";
                continue;
            }

            // check to make sure we aren't pausing HB transmissions (ACKs)
            // while a callsign is selected
            if (m_config.heartbeat_qso_pause() && !selectedCallsign.isEmpty()) {
                qCDebug(mainwindow_js8) << "hb paused during qso";
                continue;
            }

            // check to make sure this callsign isn't blacklisted
            if (m_config.hb_blacklist().contains(d.from) ||
                m_config.hb_blacklist().contains(
                    Radio::base_callsign(d.from))) {
                qCDebug(mainwindow_js8) << "hb blacklist blocking" << d.from;
                continue;
            }

            // check to see if we have a message for a station who is
            // heartbeating
            QString extra;
            auto mid = getNextMessageIdForCallsign(d.from);

            // Get number of unread messages
            int pendingCount = countUnreadForCallsign(d.from);
            if (isGroupCall) {
                pendingCount += countGroupUnreadForCallsign(d.to, d.from);
            }

            // We want to send the number of pending messages IN ADDITION TO
            // the one we are about to send, so decrement the actual count
            pendingCount--;

            if (mid != -1) {
                if (pendingCount > 0) {
                    extra = QString("MSG ID %1 +%2)")
                            .arg(mid)
                            .arg(pendingCount);
                }
                else {
                    extra = QString("MSG ID %1").arg(mid);
                }

            }

            // group messaging - if isGroupCall, check to see if there's a
            // message id for the group and return it if there's not an
            // individual message
            // TODO: include group name in response to differentiate from direct
            // messages?
            else if (isGroupCall) {
                mid = getNextGroupMessageIdForCallsign(d.to, d.from);
                if (mid != -1) {
                    if (pendingCount > 0) {
                        extra = QString("MSG ID %1 +%2)")
                            .arg(mid)
                            .arg(pendingCount);
                    }
                    else {
                        extra = QString("MSG ID %1").arg(mid);
                    }
                }
            }

            // Rate-limit HB ACKs — records timestamp on first contact;
            // blocks and purges callsign from the database if the
            // station HB's again within 55 minutes.
            processHeartbeatRateLimit(d.from);

            if (!m_config.hb_blacklist().contains(d.from) &&
                !m_config.hb_blacklist().contains(
                    Radio::base_callsign(d.from))) {
                sendHeartbeatAck(d.from, d.snr, extra);
            }

            if (isAllCall) {
                // since all pings are technically @ALLCALL, let's bump the
                // allcall cache here...
                m_txAllcallCommandCache.insert(d.from, new QDateTime(now), 5);
            }

            continue;
        }

        // PROCESS HEARTBEAT SNR
        else if (d.cmd == " HEARTBEAT SNR") {
            qCDebug(mainwindow_js8) << "skipping incoming hb snr" << d.text;
            continue;
        }

        // PROCESS CQ
        else if (d.cmd == " CQ") {
            CallDetail cd = {};
            cd.bits = d.bits;
            cd.call = d.from;
            cd.dial = d.dial;
            cd.offset = d.offset;
            cd.grid = d.grid;
            cd.snr = d.snr;
            cd.tdrift = d.tdrift;
            cd.submode = d.submode;
            cd.utcTimestamp = d.utcTimestamp;
            cd.cqTimestamp = d.utcTimestamp.isValid()
                                 ? d.utcTimestamp
                                 : DriftingDateTime::currentDateTimeUtc();

            logCallActivity(cd, true);
            logHeardGraph(d.from, d.to);
            continue;
        }

        // PROCESS MSG
        else if (d.cmd == " MSG" && !isAllCall) {

            auto text = d.text;

            /**
             * APRS relay: inbound `MSG` where the payload starts with
             * `TO:<DEST>` and includes `DE <SENDER>`.
             * Dedupe within 60s using TO + TEXT + DE <SENDER>.
             */
            if (d.to == "@APRSIS") {
                static QRegularExpression aprsToRe("^TO:\\s*(\\S+)\\s+(.*)$");
                auto match = aprsToRe.match(text);
                if (match.hasMatch()) {
                    auto dest = match.captured(1);
                    auto aprsText = match.captured(2).trimmed();
                    if (dest == m_config.my_callsign().trimmed()) {
                        static QRegularExpression aprsDeRe(
                            "\\s+DE\\s+(\\S+)\\s*$");
                        auto aprsSender = QString("APRS");
                        auto deMatch = aprsDeRe.match(aprsText);
                        if (deMatch.hasMatch()) {
                            aprsSender = deMatch.captured(1);
                        }
                        auto dedupeKey = QString("%1|%2|%3")
                                             .arg(dest, aprsText, aprsSender)
                                             .toUpper();
                        auto now = DriftingDateTime::currentDateTimeUtc();
                        auto cutoff = now.addSecs(-60);
                        auto it = m_aprsRelayDedupCache.begin();
                        while (it != m_aprsRelayDedupCache.end()) {
                            if (it.value() < cutoff) {
                                it = m_aprsRelayDedupCache.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        if (m_aprsRelayDedupCache.contains(dedupeKey)) {
                            continue;
                        }

                        CommandDetail cd = d;
                        cd.cmd = " MSG ";
                        cd.from = "APRS";
                        cd.relayPath = "APRS";
                        cd.to = dest;
                        cd.text = aprsText;

                        addCommandToMyInbox(cd);

                        m_aprsRelayDedupCache.insert(dedupeKey, now);
                        tryNotify("inbox");
                    }
                }
                continue;
            }

            qCDebug(mainwindow_js8) << "adding message to inbox" << text;

            auto calls = parseRelayPathCallsigns(d.from, text);

            d.cmd = " MSG ";
            d.relayPath = calls.join(">");
            d.text = text;

            addCommandToMyInbox(d);

            // notification
            tryNotify("inbox");

            // we haven't replaced the from with the relay path, so we have to
            // use it for the ack if there is one
            reply = QString("%1 ACK").arg(calls.length() > 1 ? d.relayPath
                                                             : d.from);

#define SHOW_ALERT_FOR_MSG 1
#if SHOW_ALERT_FOR_MSG
            SelfDestructMessageBox *m = new SelfDestructMessageBox(
                300, "New Message Received",
                QString("A new message was received at %1 UTC from %2")
                    .arg(d.utcTimestamp.time().toString())
                    .arg(d.from),
                QMessageBox::Information, QMessageBox::Ok, QMessageBox::Ok,
                false, this);

            m->show();
#endif
        }

        // PROCESS ACKS
        else if (d.cmd == " ACK" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming ack" << d.text;

            // notification for ack
            tryNotify("ack");

            // make sure this is explicit
            continue;
        }

        // PROCESS BUFFERED CMD
        else if (d.cmd == " CMD" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming command" << d.text;

            // make sure this is explicit
            continue;
        }

        // PROCESS BUFFERED QUERY
        else if (d.cmd == " QUERY" && !isAllCall) {
            auto who = d.from; // keep in mind, this is the sender, not the
                               // original requestor if relayed
            auto replyPath = d.from;

            if (d.relayPath.contains(">")) {
                auto path = d.relayPath.split(">");
                who = path.last();
                replyPath = d.relayPath;
            }

            QStringList segs = d.text.split(" ");
            if (segs.isEmpty()) {
                continue;
            }

            auto cmd = segs.first();
            segs.removeFirst();

            if (cmd == "MSG" && !segs.isEmpty()) {
                auto inbox = Inbox(inboxPath());
                if (!inbox.open()) {
                    continue;
                }

                bool ok = false;
                int mid = QString(segs.first()).toInt(&ok);
                if (!ok) {
                    continue;
                }

                auto msg = inbox.value(mid);
                auto params = msg.params();
                if (params.isEmpty()) {
                    continue;
                }

                auto from = params.value("FROM").toString().trimmed();

                auto to = params.value("TO").toString().trimmed();

                // group messaging - allow any message to a @GROUP to be
                // retrieved by anybody
                bool isGroupMsg = to.startsWith("@");

                if (!isGroupMsg && to != who &&
                    to != Radio::base_callsign(who)) {
                    continue;
                }

                auto text = params.value("TEXT").toString().trimmed();
                if (text.isEmpty()) {
                    continue;
                }

                /*
                 * mark as delivered (so subsequent HBs and QUERY MSGS don't
                 * receive this message)
                 *
                 * Do this in callbacks so that messages are only marked read
                 * after any potential confirmations have been made by the user,
                 * and the message has been processed in the transaction queue.
                 */
                if (!isGroupMsg) {
                    callback = [this, mid, msg]() {
                        this->markMsgDelivered(mid, msg);
                    };
                } else {
                    callback = [this, mid, who]() {
                        this->markGroupMsgDeliveredForCallsign(mid, who);
                    };
                }

                // Get number of unread messages
                int pendingCount = countUnreadForCallsign(who);
                if (isGroupCall) {
                    pendingCount += countGroupUnreadForCallsign(d.to, who);
                }

                // We want to send the number of pending messages IN ADDITION TO
                // the one we are about to send, so decrement the actual count
                // We need to decrement 2 here, because "mark as read" occurs
                // in a callback after successful transmission
                pendingCount-=2;

                auto lookaheadMid = getLookaheadMessageIdForCallsign(who, mid);
                if (lookaheadMid == -1 && isGroupMsg) {
                    lookaheadMid =
                        getLookaheadGroupMessageIdForCallsign(d.to, who, mid);
                }

                // and reply
                if (lookaheadMid != -1) {
                    if (pendingCount > 0) {
                        reply = QString("%1 MSG %2 FROM %3 NEXT MSG ID %4 +%5");
                        reply = reply.arg(replyPath);
                        reply = reply.arg(text);
                        reply = reply.arg(from);
                        reply = reply.arg(lookaheadMid);
                        reply = reply.arg(pendingCount);
                    }
                    else {
                        reply = QString("%1 MSG %2 FROM %3 NEXT MSG ID %4");
                        reply = reply.arg(replyPath);
                        reply = reply.arg(text);
                        reply = reply.arg(from);
                        reply = reply.arg(lookaheadMid);
                    }

                } else {
                    reply = QString("%1 MSG %2 FROM %3");
                    reply = reply.arg(replyPath);
                    reply = reply.arg(text);
                    reply = reply.arg(from);
                }
            }
        }

        // PROCESS BUFFERED QUERY MSGS
        else if (d.cmd == " QUERY MSGS" &&
                 ui->actionModeAutoreply->isChecked()) {
            auto who = d.from; // keep in mind, this is the sender, not the
                               // original requestor if relayed
            auto replyPath = d.from;

            if (d.relayPath.contains(">")) {
                auto path = d.relayPath.split(">");
                who = path.last();
                replyPath = d.relayPath;
            }

            // if this is an allcall or a directed call, check to see if we have
            // a stored message for user. we reply yes if the user would be able
            // to retreive a stored message
            auto mid = getNextMessageIdForCallsign(who);

            // Get number of unread messages
            int pendingCount = countUnreadForCallsign(who);
            if (isGroupCall) {
                pendingCount += countGroupUnreadForCallsign(d.to, d.from);
            }

            // We want to send the number of pending messages IN ADDITION TO
            // the one we are about to send, so decrement the actual count
            pendingCount--;

            if (mid != -1) {
                if (pendingCount > 0) {
                    reply = QString("%1 YES MSG ID %2 +%3")
                                .arg(replyPath)
                                .arg(mid)
                                .arg(pendingCount);
                }
                else {
                    reply = QString("%1 YES MSG ID %2")
                                .arg(replyPath)
                                .arg(mid);
                }
            }

            // Group messaging - if isGroupCall, check to see if there's a
            // message id for the group and return it if there's not an
            // individual message
            // TODO: include group name in response to differentiate from direct
            // messages?
            else if (isGroupCall) {
                mid = getNextGroupMessageIdForCallsign(d.to, d.from);
                if (mid != -1) {
                    if (pendingCount > 0) {
                        reply = QString("%1 YES MSG ID %2 +%3")
                                    .arg(replyPath)
                                    .arg(mid)
                                    .arg(pendingCount);
                    }
                    else {
                        reply = QString("%1 YES MSG ID %2")
                                    .arg(replyPath)
                                    .arg(mid);
                    }
                }
            }

            // if this is not an allcall and we have no messages, reply no.
            if (!isAllCall && reply.isEmpty()) {
                reply = QString("%1 NO").arg(replyPath);
            }
        }

        // PROCESS BUFFERED QUERY CALL
        else if (d.cmd == " QUERY CALL" &&
                 ui->actionModeAutoreply->isChecked()) {
            auto replyPath = d.from;
            if (d.relayPath.contains(">")) {
                replyPath = d.relayPath;
            }

            auto who = d.text;
            if (who.isEmpty()) {
                continue;
            }

            auto callsigns = Varicode::parseCallsigns(who);
            if (callsigns.isEmpty()) {
                continue;
            }

            QStringList replies;
            int callsignAging = m_config.callsign_aging();
            auto baseCall = callsigns.first();
            foreach (auto cd, m_callActivity.values()) {
                if (callsignAging &&
                    cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                    continue;
                }

                if (baseCall == cd.call ||
                    baseCall == Radio::base_callsign(cd.call)) {
                    auto r = QString("%1 (%2)")
                                 .arg(Varicode::formatSNR(cd.snr))
                                 .arg(since(cd.utcTimestamp))
                                 .trimmed();
                    replies.append(r);
                    break;
                }
            }

            if (!replies.isEmpty()) {
                replies.prepend(QString("%1 YES").arg(replyPath));
            }

            reply = replies.join(" ");

            if (!reply.isEmpty()) {
                if (isAllCall) {
                    m_txAllcallCommandCache.insert(d.from, new QDateTime(now),
                                                   25);
                }
            }
        }

#if 0
        // PROCESS ALERT
        else if (d.cmd == "!" && !isAllCall) {

            // create alert dialog
            processAlertReplyForCommand(d, d.from, " ");

            // make sure this is explicit
            continue;
        }
#endif

        // well, if there's no reply, don't do anything...
        if (reply.isEmpty()) {
            continue;
        }

        // do not queue @ALLCALL replies if auto-reply is not checked
        if (!ui->actionModeAutoreply->isChecked() && isAllCall) {
            continue;
        }

#if 0
        // TODO: jsherer - HB issue here
        // do not queue a reply if it's a HB and HB is not active
        // if((!ui->hbMacroButton->isChecked() || m_hbInterval <= 0) && d.cmd.contains("HB")){
        //     continue;
        // }
#endif

        // do not queue for reply if there's text in the window
        if (!ui->extFreeTextMsgEdit->toPlainText().isEmpty()) {
            continue;
        }

        // do not queue for reply if there's a buffer open to us
        int bufferOffset = 0;
        if (hasExistingMessageBufferToMe(&bufferOffset)) {
            qCDebug(mainwindow_js8) << "skipping reply due to open buffer"
                                    << bufferOffset << m_messageBuffer.count();
            continue;
        }

        // add @ALLCALLs to the @ALLCALL cache
        if (isAllCall) {
            m_txAllcallCommandCache.insert(d.from, new QDateTime(now), 25);
        }

        // queue the reply here to be sent when a free interval is available on
        // the frequency that was sent unless, this is an allcall, to which we
        // should be responding on a clear frequency offset we always want to
        // make sure that the directed cache has been updated at this point so
        // we have the most information available to make a frequency selection.
        if (m_config.autoreply_confirmation()) {
            confirmThenEnqueueMessage(90, priority, reply, freq, callback);
        } else {
            enqueueMessage(priority, reply, freq, callback);
        }
    }
}
