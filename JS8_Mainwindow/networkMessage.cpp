/**
 * @file networkMessage.cpp
 * @brief member function of the UI_Constructor class
 * API commands for external control and data retrieval.
 * sends data to external clients via the Network Message API
 * @addtogroup API
 * @{
 */

#include "JS8_UI/mainwindow.h"

/**
 * @brief Processes an incoming API network message
 * This function acts as the primary router for the JS8Call API. It handles
 * RIG, STATION, RX, and TX commands by either updating the application
 * state or querying current values to send back to the client.
 *
 * @param message The network message to process
 */
void UI_Constructor::networkMessage(Message const &message) {
    auto type = message.type();

    if (type == "PING") {
        return;
    }

    auto id = message.id();

    qCDebug(mainwindow_js8) << "try processing network message" << type << id;

    // Inspired by FLDigi
    // TODO: MAIN.RX - Turn on RX
    // TODO: MAIN.TX - Transmit
    // TODO: MAIN.AUTO - Auto
    // TODO: MAIN.HB - HB

    // RIG.GET_PTT  - Returns PTT status
    // RIG.SET_TUNE - Turns TUNE on and off
    // RIG.TX_HALT  - Stops transmission immediately
    // RIG.GET_FREQ - Get the current Frequency
    // RIG.SET_FREQ - Set the current Frequency
    /**
     * @name RIG Commands
     * RIG related API calls
     */
     /** @{ */

    /** @brief RIG.GET_PTT
     * Returns the PTT status
     * @note API 2.6+
     */
    if (type == "RIG.GET_PTT") {
        bool isPTT = m_transmitting;
        sendNetworkMessage("RIG.PTT_STATUS", "",
            {
                {"_ID", id},
                {"PTT", QVariant(isPTT)},
                {"MESSAGE", QVariant(isPTT ? m_currentMessage : "")}
           });
        return;
    }

    /** @brief RIG.SET_TUNE
     * Turns TUNE on and off
     * @note API 2.6+
     */
    if (type == "RIG.SET_TUNE") {
        auto value = QVariant(message.value());
        UI_Constructor::on_tuneButton_clicked(value.toBool());
          sendNetworkMessage("RIG.SET_TUNE", "", {
            {"_ID", id},
            {"value", ui->tuneButton->isChecked()}
          });
        return;
    }

    /** @brief RIG.TX_HALT
     * Stops transmission immediately, consider this an E-stop for the rig
     * @note API 2.6+
     */
    if (type == "RIG.TX_HALT") {
        auto value = QVariant(message.value());
        UI_Constructor::on_stopTxButton_clicked();
          sendNetworkMessage("RIG.TX_HALT", "", {
            {"_ID", id},
            {"value", ui->monitorTxButton->isChecked()}
          });
        return;
    }

    /**
     * @brief RIG.GET_FREQ: Retrieves the current dial and offset frequencies.
     *
     * If the WSJT-X protocol is enabled, this also triggers a status update
     * via the @ref m_wsjtxMessageMapper.
     */
    if (type == "RIG.GET_FREQ") {
        // Send WSJT-X Status message if protocol is enabled
        if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
            QString dx_call = callsignSelected();
            QString dx_grid = "";
            if (!dx_call.isEmpty() && m_callActivity.contains(dx_call)) {
                dx_grid = m_callActivity[dx_call].grid;
            }
            QString tx_message = m_transmitting ? m_currentMessage : "";

            m_wsjtxMessageMapper->sendStatusUpdate(
                dialFrequency(), freq(),
                "JS8", // mode
                dx_call, m_config.my_callsign(), m_config.my_grid(), dx_grid,
                true, // tx_enabled
                m_transmitting,
                m_decoderBusy, // decoding (match WSJT-X: decoder busy only)
                tx_message);
        }

        // Send native JSON message only if not conflicting with WSJT-X
        bool skip_json = false;
        if (m_config.wsjtx_protocol_enabled() &&
            m_config.wsjtx_server_port() == m_config.udp_server_port() &&
            m_config.wsjtx_server_name() == m_config.udp_server_name()) {
            skip_json = true;
        }

        if (!skip_json) {
            sendNetworkMessage(
                "RIG.FREQ", "",
                {{"_ID", id},
                 {"FREQ", QVariant((quint64)dialFrequency() + freq())},
                 {"DIAL", QVariant((quint64)dialFrequency())},
                 {"OFFSET", QVariant((quint64)freq())}});
        }
        return;
    }
    /**
     * @brief RIG.SET_FREQ: Updates the rig dial frequency and/or frequency
     * offset.
     */
    if (type == "RIG.SET_FREQ") {
        auto params = message.params();
        if (params.contains("DIAL")) {
            bool ok = false;
            auto f = params["DIAL"].toInt(&ok);
            if (ok) {
                setRig(f);
                displayDialFrequency();
            }
        }
        if (params.contains("OFFSET")) {
            bool ok = false;
            auto f = params["OFFSET"].toInt(&ok);
            if (ok) {
                changeFreq(f);
            }
        }
    }
    /** @} */ // End RIG Commands

    // STATION refers to JS8Call station settings
    // STATION.GET_CALLSIGN - Get the current callsign
    // STATION.GET_GRID - Get the current grid locator
    // STATION.SET_GRID - Set the current grid locator
    // STATION.GET_INFO - Get the current station qth
    // STATION.SET_INFO - Set the current station qth
    // STATION.GET_SPOT - Get the current spotting status
    // STATION.SET_SPOT - Set the current spotting status
    // STATION.GET_OS   - Get basic info about the OS we are running on
    // STATION.VERSION  - Get the JS8Call version
    // STATION.GET_CONFIG - Get all config states (auto_reply, js8hb, hback, etc.)
    // STATION.SET_AUTO_REPLY - Toggle auto-reply on/off
    // STATION.SET_JS8HB - Toggle JS8 heartbeat on/off
    // STATION.SET_HBACK - Toggle heartbeat acknowledgments on/off
    // STATION.SET_MULTI_DECODER - Toggle multi-decoder on/off
    // STATION.SET_HB_INTERVAL - Set heartbeat interval (seconds)
    // STATION.SET_HB_TIMER - Start/stop heartbeat timer
    // STATION.SEND_HB - Send heartbeat immediately
    // STATION.SET_AUTOREPLY_CONFIRMATION - Toggle autoreply confirmation via TCP
    // STATION.AUTOREPLY_CONFIRM_RESPONSE - Accept/reject pending autoreply
    // STATION.SET_GROUPS - Replace subscribed groups list
    // STATION.SET_AVOID_ALLCALL - Toggle @ALLCALL opt-out
    /**
     * @name STATION Commands
     * STATION related API calls
     * These calls refer to JS8Call station settings
     */
    /** @{ */

    /** @brief STATION.GET_CALLSIGN: Returns the configured station callsign. */
    if (type == "STATION.GET_CALLSIGN") {
        sendNetworkMessage("STATION.CALLSIGN", m_config.my_callsign(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_GRID: Returns the current Maidenhead grid locator. */
    if (type == "STATION.GET_GRID") {
        sendNetworkMessage("STATION.GRID", m_config.my_grid(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.SET_GRID: Updates the dynamic grid locator for the
     * station. */
    if (type == "STATION.SET_GRID") {
        m_config.set_dynamic_location(message.value());
        sendNetworkMessage("STATION.GRID", m_config.my_grid(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_INFO: Retrieves the station information (QTH). */
    if (type == "STATION.GET_INFO") {
        sendNetworkMessage("STATION.INFO", m_config.my_info(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.SET_INFO: Updates the dynamic station information (QTH).
     */
    if (type == "STATION.SET_INFO") {
        m_config.set_dynamic_station_info(message.value());
        sendNetworkMessage("STATION.INFO", m_config.my_info(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_STATUS: Retrieves the current station status message.
     */
    if (type == "STATION.GET_STATUS") {
        sendNetworkMessage("STATION.STATUS", m_config.my_status(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.SET_STATUS: Updates the dynamic station status message.
     */
    if (type == "STATION.SET_STATUS") {
        m_config.set_dynamic_station_status(message.value());
        sendNetworkMessage("STATION.STATUS", m_config.my_status(),
                           {
                               {"_ID", id},
                           });
        return;
    }

    /** @brief STATION.VERSION
     * Returns the JS8Call version
     * @note API 2.6+
     */
    if (type == "STATION.VERSION") {
        QString ver = version();
        sendNetworkMessage("STATION.VERSION", "",
            {
                {"_ID", id},
                {"VERSION", QVariant(ver)}
            });
        return;
    }

    /** @brief STATION.GET_OS
     * Returns OS information for the station
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
    if(type == "STATION.GET_OS"){
      sendNetworkMessage("STATION.GET_OS", "", {
	      {"OS_NAME", QSysInfo::prettyProductName()},
	      {"OS_KERNEL", QSysInfo::kernelType()},
	      {"OS_KERNEL_VERSION", QSysInfo::kernelVersion()},
	      {"_ID", id}
        });
        return;
    }

    /** @brief STATION.GET_SPOT
     * Get the current spotting status
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
    if(type == "STATION.GET_SPOT") {
        sendNetworkMessage("STATION.SPOT", "", {
          {"value", ui->spotButton->isChecked()},
	      {"_ID", id}
        });
        return;
    }

    /** @brief STATION.SET_SPOT
     * Set the current spotting status
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
if(type == "STATION.SET_SPOT") {
        auto value = QVariant(message.value());
          UI_Constructor::on_spotButton_clicked(value.toBool());
          sendNetworkMessage("STATION.SPOT", "", {
            {"value", ui->spotButton->isChecked()},
            {"_ID", id}
          });
          return;
    }

    /** @brief STATION.GET_CONFIG: Returns all mode/config states for remote control.
     *  @note API 2.6+ */
    if (type == "STATION.GET_CONFIG") {
        QVariantList groupsList;
        for (auto const &g : m_config.my_groups()) {
            groupsList.append(g);
        }
        sendNetworkMessage("STATION.CONFIG", "", {
            {"_ID", id},
            {"AUTO_REPLY", QVariant(ui->actionModeAutoreply->isChecked())},
            {"JS8HB", QVariant(ui->actionModeJS8HB->isChecked())},
            {"HBACK", QVariant(ui->actionHeartbeatAcknowledgements->isChecked())},
            {"MULTI_DECODER", QVariant(ui->actionModeMultiDecoder->isChecked())},
            {"HB_INTERVAL", QVariant(m_hbInterval)},
            {"HB_TIMER_ACTIVE", QVariant(m_hb_loop->isActive())},
            {"MONITOR", QVariant(ui->monitorButton->isChecked())},
            {"TX_ENABLED", QVariant(ui->monitorTxButton->isChecked())},
            {"SPEED", QVariant(m_nSubMode)},
            {"CAN_HB", QVariant(canCurrentModeSendHeartbeat())},
            {"AUTOREPLY_CONFIRMATION", QVariant(m_config.autoreply_confirmation())},
            {"MY_GROUPS", QVariant(groupsList)},
            {"AVOID_ALLCALL", QVariant(m_config.avoid_allcall())},
        });
        return;
    }

    /** @brief STATION.SET_AUTO_REPLY: Toggle auto-reply mode.
     *  @note API 2.6+ */
    if (type == "STATION.SET_AUTO_REPLY") {
        auto checked = QVariant(message.value()).toBool();
        ui->actionModeAutoreply->setChecked(checked);
        sendNetworkMessage("STATION.SET_AUTO_REPLY", "", {
            {"_ID", id},
            {"AUTO_REPLY", QVariant(ui->actionModeAutoreply->isChecked())},
        });
        return;
    }

    /** @brief STATION.SET_JS8HB: Toggle heartbeat networking mode.
     *  @note API 2.6+ */
    if (type == "STATION.SET_JS8HB") {
        auto checked = QVariant(message.value()).toBool();
        ui->actionModeJS8HB->setChecked(checked);
        sendNetworkMessage("STATION.SET_JS8HB", "", {
            {"_ID", id},
            {"JS8HB", QVariant(ui->actionModeJS8HB->isChecked())},
        });
        return;
    }

    /** @brief STATION.SET_HBACK: Toggle heartbeat acknowledgments.
     *  @note API 2.6+ */
    if (type == "STATION.SET_HBACK") {
        auto checked = QVariant(message.value()).toBool();
        ui->actionHeartbeatAcknowledgements->setChecked(checked);
        sendNetworkMessage("STATION.SET_HBACK", "", {
            {"_ID", id},
            {"HBACK", QVariant(ui->actionHeartbeatAcknowledgements->isChecked())},
        });
        return;
    }

    /** @brief STATION.SET_MULTI_DECODER: Toggle multi-decoder mode.
     *  @note API 2.6+ */
    if (type == "STATION.SET_MULTI_DECODER") {
        auto checked = QVariant(message.value()).toBool();
        ui->actionModeMultiDecoder->setChecked(checked);
        sendNetworkMessage("STATION.SET_MULTI_DECODER", "", {
            {"_ID", id},
            {"MULTI_DECODER", QVariant(ui->actionModeMultiDecoder->isChecked())},
        });
        return;
    }

    /** @brief STATION.SET_HB_INTERVAL: Set heartbeat interval in minutes.
     *  @note API 2.6+ */
    if (type == "STATION.SET_HB_INTERVAL") {
        auto interval = message.params().value("INTERVAL").toInt();
        m_hbInterval = interval;
        if (ui->hbMacroButton->isChecked() && interval > 0) {
            m_hb_loop->onTxLoopPeriodChangeStart(interval * (qint64)60000);
        }
        updateHBButtonDisplay();
        sendNetworkMessage("STATION.SET_HB_INTERVAL", "", {
            {"_ID", id},
            {"HB_INTERVAL", QVariant(m_hbInterval)},
        });
        return;
    }

    /** @brief STATION.SET_HB_TIMER: Start/stop heartbeat timer loop.
     *  @note API 2.6+ */
    if (type == "STATION.SET_HB_TIMER") {
        auto start = QVariant(message.value()).toBool();
        ui->hbMacroButton->setChecked(start);
        sendNetworkMessage("STATION.SET_HB_TIMER", "", {
            {"_ID", id},
            {"HB_TIMER_ACTIVE", QVariant(ui->hbMacroButton->isChecked())},
        });
        return;
    }

    /** @brief STATION.SEND_HB: Send an immediate heartbeat.
     *  @note API 2.6+ */
    if (type == "STATION.SEND_HB") {
        sendHB();
        sendNetworkMessage("STATION.SEND_HB", "", {
            {"_ID", id},
        });
        return;
    }

    /** @brief STATION.SET_AUTOREPLY_CONFIRMATION: Toggle autoreply confirmation via TCP.
     *  @note API 2.6+ */
    if (type == "STATION.SET_AUTOREPLY_CONFIRMATION") {
        auto checked = QVariant(message.value()).toBool();
        m_config.set_autoreply_confirmation(checked);
        sendNetworkMessage("STATION.AUTOREPLY_CONFIRMATION", "",
            {{"_ID", id},
             {"AUTOREPLY_CONFIRMATION", QVariant(checked)}});
        return;
    }

    /** @brief STATION.AUTOREPLY_CONFIRM_RESPONSE: Accept/reject a pending autoreply confirmation.
     *  @note API 2.6+ */
    if (type == "STATION.AUTOREPLY_CONFIRM_RESPONSE") {
        auto confirmId = message.params().value("CONFIRM_ID").toInt();
        auto accepted = QVariant(message.value()).toBool();
        // CRITICAL: m_pendingConfirmations lives in the GUI thread
        QMetaObject::invokeMethod(this, [this, confirmId, accepted]() {
            if (m_pendingConfirmations.contains(confirmId)) {
                auto pc = m_pendingConfirmations.take(confirmId);
                pc.timer->stop();
                delete pc.timer;
                if (accepted) {
                    enqueueMessage(pc.priority, pc.message, pc.offset, pc.callback);
                }
                sendNetworkMessage("STATION.AUTOREPLY_CONFIRMED", "",
                    {{"_ID", QVariant(-1)},
                     {"CONFIRM_ID", QVariant(confirmId)},
                     {"ACCEPTED", QVariant(accepted)},
                     {"MESSAGE", QVariant(pc.message)}});
            }
        });
        return;
    }

    /** @brief STATION.SET_GROUPS: Replace the subscribed groups list via TCP.
     *  Validates each group (isGroupAllowed + isCompoundCallsign) and returns
     *  a TCP error instead of opening a MessageBox (headless-safe).
     *  @note API 2.6+ */
    if (type == "STATION.SET_GROUPS") {
        QStringList newGroups;
        auto groupsVar = message.params().value("GROUPS");
        if (groupsVar.canConvert<QVariantList>()) {
            for (auto const &v : groupsVar.toList()) {
                auto g = v.toString().trimmed();
                if (g.isEmpty() || !g.startsWith("@")) continue;
                if (!Varicode::isGroupAllowed(g)) {
                    sendNetworkMessage("STATION.SET_GROUPS", "", {
                        {"_ID", id},
                        {"ERROR", QString("Group not allowed: %1").arg(g)},
                    });
                    return;
                }
                if (!Varicode::isCompoundCallsign(g)) {
                    sendNetworkMessage("STATION.SET_GROUPS", "", {
                        {"_ID", id},
                        {"ERROR", QString("Invalid group name: %1").arg(g)},
                    });
                    return;
                }
                newGroups.append(g);
            }
        }
        m_config.setMyGroups(newGroups);

        QVariantList result;
        for (auto const &g : m_config.my_groups()) {
            result.append(g);
        }
        sendNetworkMessage("STATION.SET_GROUPS", "", {
            {"_ID", id},
            {"GROUPS", QVariant(result)},
        });
        return;
    }

    /** @brief STATION.SET_AVOID_ALLCALL: Toggle @ALLCALL opt-out via TCP.
     *  @note API 2.6+ */
    if (type == "STATION.SET_AVOID_ALLCALL") {
        auto checked = QVariant(message.value()).toBool();
        m_config.set_avoid_allcall(checked);
        sendNetworkMessage("STATION.SET_AVOID_ALLCALL", "", {
            {"_ID", id},
            {"AVOID_ALLCALL", QVariant(m_config.avoid_allcall())},
        });
        return;
    }
    /** @} */ // End STATION Commands

    // RX.GET_CALL_ACTIVITY
    // RX.GET_CALL_SELECTED
    // RX.GET_BAND_ACTIVITY
    // RX.GET_TEXT
    // RX.GET_FILTER - Get bandpass filter center, width, enabled state
    // RX.SET_FILTER - Set filter CENTER and/or WIDTH
    // RX.SET_FILTER_ENABLED - Toggle filter overlay on/off
    /**
     * @name RX Commands
     * RX related API calls
     * Refers to received data and activity
     */
    /** @{ */

    /**
     * @brief RX.GET_CALL_ACTIVITY: Returns a list of active callsigns.
     * Filters results based on the `callsign_aging` configuration.
     */
    if (type == "RX.GET_CALL_ACTIVITY") {
        auto now = DriftingDateTime::currentDateTimeUtc();
        int callsignAging = m_config.callsign_aging();
        QVariantMap calls = {
            {"_ID", id},
        };

        foreach (auto cd, m_callActivity.values()) {
            if (callsignAging &&
                cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                continue;
            }
            QVariantMap detail;
            detail["SNR"] = QVariant(cd.snr);
            detail["GRID"] = QVariant(cd.grid);
            detail["UTC"] = QVariant(cd.utcTimestamp.toMSecsSinceEpoch());
            calls[cd.call] = QVariant(detail);
        }

        sendNetworkMessage("RX.CALL_ACTIVITY", "", calls);
        return;
    }
    /** @brief RX.GET_CALL_SELECTED: Returns the currently selected callsign. */
    if (type == "RX.GET_CALL_SELECTED") {
        sendNetworkMessage("RX.CALL_SELECTED", callsignSelected(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /**
     * @brief RX.GET_BAND_ACTIVITY: Returns recent band activity details.
     * Includes frequency, offset, text, SNR, and UTC timestamp for each entry.
     */
    if (type == "RX.GET_BAND_ACTIVITY") {
        QVariantMap offsets = {
            {"_ID", id},
        };
        for (auto const [offset, activity] : m_bandActivity.asKeyValueRange()) {
            if (activity.isEmpty())
                continue;

            auto const d = activity.last();

            offsets[QString("%1").arg(offset)] = QVariant(QVariantMap{
                {"FREQ", QVariant(d.dial + d.offset)},
                {"DIAL", QVariant(d.dial)},
                {"OFFSET", QVariant(d.offset)},
                {"TEXT", QVariant(d.text)},
                {"SNR", QVariant(d.snr)},
                {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())}});
        }

        sendNetworkMessage("RX.BAND_ACTIVITY", "", offsets);
        return;
    }
    /** @brief RX.GET_TEXT: Retrieves the current RX text buffer. */
    if (type == "RX.GET_TEXT") { /** RX.GET_TEXT */
        sendNetworkMessage("RX.TEXT", ui->textEditRX->toPlainText().right(1024),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /**
     * @brief RX.GET_FREE_OFFSETS: Returns contiguous free offset segments in the passband.
     * Accounts for the bandwidth of each active signal's submode when computing occupied
     * zones, so returned segments are guaranteed wide enough for the requested TX mode.
     * Optional params: SPEED (submode int, defaults to current), LOW (Hz, default 500),
     * HIGH (Hz, default 2500). Entries older than 30 seconds are treated as clear.
     * @note API 2.6+
     */
    if (type == "RX.GET_FREE_OFFSETS") {
        int const speed = message.params().value("SPEED", QVariant(m_nSubMode)).toInt();
        int const low   = message.params().value("LOW",   QVariant(500)).toInt();
        int const high  = message.params().value("HIGH",  QVariant(2500)).toInt();
        int const bw    = JS8::Submode::bandwidth(speed);

        auto const now  = DriftingDateTime::currentDateTimeUtc();

        // Build occupied intervals [start, end] for each recently active offset.
        // The blocked zone around each occupied offset accounts for both the signal's
        // own bandwidth and the requested TX bandwidth, ensuring the returned segments
        // won't overlap either transmission.
        QList<QPair<int, int>> occupied;
        for (auto const [occ, activity] : m_bandActivity.asKeyValueRange()) {
            if (activity.isEmpty() || activity.last().utcTimestamp.secsTo(now) >= 30)
                continue;
            int const occ_bw    = JS8::Submode::bandwidth(activity.last().submode);
            int const halfTotal = bw / 2 + occ_bw / 2;
            occupied.append({occ - halfTotal, occ + halfTotal});
        }

        std::sort(occupied.begin(), occupied.end());

        // Merge overlapping intervals.
        QList<QPair<int, int>> merged;
        for (auto const &interval : occupied) {
            if (merged.isEmpty() || merged.last().second < interval.first)
                merged.append(interval);
            else
                merged.last().second = std::max(merged.last().second, interval.second);
        }

        // Find gaps in [low, high] that are at least bw wide.
        QVariantList freeSegments;
        int cursor = low;
        for (auto const &[start, end] : merged) {
            if (end <= cursor)
                continue;
            if (start > cursor) {
                int const gapHigh = std::min(start, high);
                int const width   = gapHigh - cursor;
                if (cursor < high && width >= bw)
                    freeSegments.append(QVariant(QVariantMap{
                        {"LOW", cursor}, {"HIGH", gapHigh}, {"WIDTH", width}}));
            }
            cursor = std::max(cursor, end);
            if (cursor >= high)
                break;
        }
        if (cursor < high && (high - cursor) >= bw)
            freeSegments.append(QVariant(QVariantMap{
                {"LOW", cursor}, {"HIGH", high}, {"WIDTH", high - cursor}}));

        sendNetworkMessage("RX.FREE_OFFSETS", "",
                           {{"_ID", id},
                            {"FREE", QVariant(freeSegments)},
                            {"SPEED", speed},
                            {"BANDWIDTH", bw},
                            {"LOW", low},
                            {"HIGH", high}});
        return;
    }

    /** @brief RX.GET_FILTER: Returns current filter center, width and enabled state.
     *  @note API 2.6+ */
    if (type == "RX.GET_FILTER") {
        sendNetworkMessage("RX.FILTER", "", {
            {"_ID", id},
            {"CENTER", QVariant(m_wideGraph->filterCenter())},
            {"WIDTH", QVariant(m_wideGraph->filterWidth())},
            {"ENABLED", QVariant(m_wideGraph->filterEnabled())},
        });
        return;
    }

    /** @brief RX.SET_FILTER: Set filter CENTER and/or WIDTH, returns updated state.
     *  @note API 2.6+ */
    if (type == "RX.SET_FILTER") {
        auto params = message.params();
        if (params.contains("CENTER")) {
            bool ok = false;
            auto c = params["CENTER"].toInt(&ok);
            if (ok) m_wideGraph->setFilterCenter(c);
        }
        if (params.contains("WIDTH")) {
            bool ok = false;
            auto w = params["WIDTH"].toInt(&ok);
            if (ok) m_wideGraph->setFilterWidth(w);
        }
        sendNetworkMessage("RX.FILTER", "", {
            {"_ID", id},
            {"CENTER", QVariant(m_wideGraph->filterCenter())},
            {"WIDTH", QVariant(m_wideGraph->filterWidth())},
            {"ENABLED", QVariant(m_wideGraph->filterEnabled())},
        });
        return;
    }

    /** @brief RX.SET_FILTER_ENABLED: Toggle filter overlay on/off.
     *  @note API 2.6+ */
    if (type == "RX.SET_FILTER_ENABLED") {
        auto enabled = QVariant(message.value()).toBool();
        m_wideGraph->setFilterEnabled(enabled);
        sendNetworkMessage("RX.SET_FILTER_ENABLED", "", {
            {"_ID", id},
            {"ENABLED", QVariant(m_wideGraph->filterEnabled())},
        });
        return;
    }
    /** @} */ // End RX Commands

    // TX.GET_TEXT - Retrieves the current TX text buffer.
    // TX.SET_TEXT - Updates the TX text buffer with new content.
    // TX.SEND_MESSAGE - Enqueues a message for transmission.
    // TX.GET_QUEUE_DEPTH - Return the number of items in the transmit queue.
    /**
     * @name TX Commands
     * TX related API calls
     * Refers to transmitted data and activity
     */
    /** @{ */

    /** @brief TX.GET_TEXT: Retrieves the current TX text buffer. */
    if (type == "TX.GET_TEXT") {
        sendNetworkMessage("TX.TEXT",
                           ui->extFreeTextMsgEdit->toPlainText().right(1024),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief TX.SET_TEXT: Updates the TX text buffer with new content. */
    if (type == "TX.SET_TEXT") {
        addMessageText(message.value(), true);
        sendNetworkMessage("TX.TEXT",
                           ui->extFreeTextMsgEdit->toPlainText().right(1024),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief TX.SEND_MESSAGE: Enqueues a message for transmission. */
    if (type == "TX.SEND_MESSAGE") {
        auto text = message.value();
        if (!text.isEmpty()) {
            enqueueMessage(PriorityNormal, text, -1, nullptr);
            processTxQueue();
            return;
        }
    }

    /**
     * @brief Return the number of items in the transmit queue.
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
    if(type == "TX.GET_QUEUE_DEPTH"){
      int depth = m_txMessageQueue.size();
      if(m_transmitting && depth==0) depth=1;
      sendNetworkMessage("TX.QUEUE_DEPTH", "", {
	  {"_ID", id},
	  {"DEPTH", QVariant(depth)}
	});
      return;
    }
    /** @} */ // End TX Commands

    // MODE.GET_SPEED
    // MODE.SET_SPEED
    /**
     * @name MODE Commands
     * MODE related API calls
     */
    /** @{ */
    /** @brief MODE.GET_SPEED: Retrieves the current transmission speed mode. */
    if (type == "MODE.GET_SPEED") {
        sendNetworkMessage("MODE.SPEED", "",
                           {
                               {"_ID", id},
                               {"SPEED", m_nSubMode},
                           });
        return;
    }
    /** @brief MODE.SET_SPEED: Updates the transmission speed mode. */
    if (type == "MODE.SET_SPEED") {
        auto ok = false;
        auto const speed =
            message.params().value("SPEED", QVariant(m_nSubMode)).toInt(&ok);
        if (ok) {
            if (speed == Varicode::JS8CallNormal)
                ui->actionModeJS8Normal->setChecked(true);
            else if (speed == Varicode::JS8CallFast)
                ui->actionModeJS8Fast->setChecked(true);
            else if (speed == Varicode::JS8CallTurbo)
                ui->actionModeJS8Turbo->setChecked(true);
            else if (speed == Varicode::JS8CallSlow)
                ui->actionModeJS8Slow->setChecked(true);
            else if (speed == Varicode::JS8CallUltra)
                ui->actionModeJS8Ultra->setChecked(true);
            setupJS8();
        }
        sendNetworkMessage("MODE.SET_SPEED", "",
                           {
                               {"_ID", id},
                               {"SPEED", m_nSubMode},
                           });
        return;
    }

    /** @} */ // End MODE Commands


    // INBOX.GET_MESSAGES
    // INBOX.STORE_MESSAGE
    /**
     * @name INBOX Commands
     * INBOX related API calls
     */
    /** @{ */
    /** @brief INBOX.GET_MESSAGES: Retrieves messages for a specified callsign. */
    if (type == "INBOX.GET_MESSAGES") {
        QString selectedCall =
            message.params().value("CALLSIGN", "").toString();
        if (selectedCall.isEmpty()) {
            selectedCall = "%";
        }

        Inbox inbox(inboxPath());
        if (!inbox.open()) {
            return;
        }

        QList<QPair<int, Message>> msgs;
        msgs.append(
            inbox.values("STORE", "$.params.TO", selectedCall, 0, 1000));
        msgs.append(
            inbox.values("READ", "$.params.FROM", selectedCall, 0, 1000));
        foreach (auto pair, inbox.values("UNREAD", "$.params.FROM",
                                         selectedCall, 0, 1000)) {
            msgs.append(pair);
        }
        std::stable_sort(
            msgs.begin(), msgs.end(),
            [](QPair<int, Message> const &a, QPair<int, Message> const &b) {
                return QVariant::compare(a.second.params().value("UTC"),
                                         b.second.params().value("UTC")) ==
                       QPartialOrdering::Greater;
            });

        QVariantList l;
        foreach (auto pair, msgs) {
            l << pair.second.toVariantMap();
        }

        sendNetworkMessage("INBOX.MESSAGES", "",
                           {
                               {"_ID", id},
                               {"MESSAGES", l},
                           });
        return;
    }
    /** @brief INBOX.STORE_MESSAGE: Stores a message in the inbox for a
     * callsign. */
    if (type == "INBOX.STORE_MESSAGE") {
        QString selectedCall =
            message.params().value("CALLSIGN", "").toString();
        if (selectedCall.isEmpty()) {
            return;
        }

        QString text = message.params().value("TEXT", "").toString();
        if (text.isEmpty()) {
            return;
        }

        CommandDetail d = {};
        d.cmd = " MSG ";
        d.to = selectedCall;
        d.from = m_config.my_callsign();
        d.relayPath = d.from;
        d.text = text;
        d.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
        d.submode = m_nSubMode;

        auto mid = addCommandToStorage("STORE", d);

        sendNetworkMessage("INBOX.MESSAGE", "",
                           {
                               {"_ID", id},
                               {"ID", mid},
                           });
        return;
    }
    /** @} */ // End INBOX Commands

    // WINDOW.RAISE
    /**
     * @name WINDOW Commands
     * WINDOW related API calls
     */
    /** @{ */
    /** @brief WINDOW.RAISE: Brings the main application window to the
     * foreground.
     * NOTE: Some OSes block this from happening
     */
    if (type == "WINDOW.RAISE") {
        setWindowState(Qt::WindowActive);
        activateWindow();
        raise();
        return;
    }

    qCDebug(mainwindow_js8) << "Unable to process networkMessage:" << type;
    /** @} */ // end of WINDOW Commands
    /** @} */ // end of API
}
