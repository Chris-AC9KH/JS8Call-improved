
/** \file
 * @brief member function of the UI_Constructor class
 *  builds the callsign query menu
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::buildQueryMenu(QMenu *menu, QString call) {
    bool isAllCall = isAllCallIncluded(call);

    // for now, we're going to omit displaying the call...delete this if we want
    // the other functionality
    call = "";

    auto grid = m_config.my_grid();

    bool emptyInfo = m_config.my_info().isEmpty();
    bool emptyGrid = m_config.my_grid().isEmpty();

    auto callAction = menu->addAction(
        QString("Send a directed message to selected callsign"));
    connect(callAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 ").arg(selectedCall), true);
    });

    menu->addSeparator();

    auto sendReplyAction = menu->addAction(
        QString("%1 Reply - Send reply message to selected callsign")
            .arg(call)
            .trimmed());
    connect(sendReplyAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        auto message = m_config.reply_message();
        message = replaceMacros(message, buildMacroValues(), true);
        addMessageText(QString("%1 %2").arg(selectedCall).arg(message), true);
    });

    auto sendSNRAction = menu->addAction(
        QString("%1 SNR - Send a signal report to the selected callsign")
            .arg(call)
            .trimmed());
    sendSNRAction->setEnabled(m_callActivity.contains(callsignSelected()));
    connect(sendSNRAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        if (!m_callActivity.contains(selectedCall)) {
            return;
        }

        auto d = m_callActivity[selectedCall];
        addMessageText(QString("%1 SNR %2")
                           .arg(selectedCall)
                           .arg(Varicode::formatSNR(d.snr)),
                       true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto infoAction = menu->addAction(
        QString("%1 INFO - Send my station information").arg(call).trimmed());
    infoAction->setDisabled(emptyInfo);
    connect(infoAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(
            QString("%1 INFO %2").arg(selectedCall).arg(m_config.my_info()),
            true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto gridAction = menu->addAction(
        QString("%1 GRID %2 - Send my current station Maidenhead grid locator")
            .arg(call)
            .arg(grid)
            .trimmed());
    gridAction->setDisabled(emptyGrid);
    connect(gridAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(
            QString("%1 GRID %2").arg(selectedCall).arg(m_config.my_grid()),
            true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    menu->addSeparator();

    auto snrQueryAction = menu->addAction(
        QString("%1 SNR? - What is my signal report?").arg(call).trimmed());
    snrQueryAction->setDisabled(isAllCall);
    connect(snrQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 SNR?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto infoQueryAction =
        menu->addAction(QString("%1 INFO? - What is your station information?")
                            .arg(call)
                            .trimmed());
    infoQueryAction->setDisabled(isAllCall);
    connect(infoQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 INFO?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto gridQueryAction =
        menu->addAction(QString("%1 GRID? - What is your current grid locator?")
                            .arg(call)
                            .trimmed());
    gridQueryAction->setDisabled(isAllCall);
    connect(gridQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 GRID?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto stationIdleQueryAction = menu->addAction(
        QString("%1 STATUS? - What is your station status message?")
            .arg(call)
            .trimmed());
    stationIdleQueryAction->setDisabled(isAllCall);
    connect(stationIdleQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 STATUS?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto heardQueryAction = menu->addAction(
        QString("%1 HEARING? - What are the stations are you hearing? (Top 4 "
                "ranked by most recently heard)")
            .arg(call)
            .trimmed());
    heardQueryAction->setDisabled(isAllCall);
    connect(heardQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 HEARING?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto alertAction = menu->addAction(
        QString("%1>[MESSAGE] - Please relay this message to its destination")
            .arg(call)
            .trimmed());
    alertAction->setDisabled(isAllCall);
    connect(alertAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1>[MESSAGE]").arg(selectedCall), true, true);
    });

    auto msgAction = menu->addAction(
        QString("%1 MSG [MESSAGE] - Please store this message in your inbox")
            .arg(call)
            .trimmed());
    msgAction->setDisabled(isAllCall);
    connect(msgAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 MSG [MESSAGE]").arg(selectedCall), true,
                       true);
    });

    auto msgToAction = menu->addAction(
        QString("%1 MSG TO:[CALLSIGN] [MESSAGE] - Please store this message at "
                "your station for later retreival by [CALLSIGN]")
            .arg(call)
            .trimmed());
    msgToAction->setDisabled(isAllCall);
    connect(msgToAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(
            QString("%1 MSG TO:[CALLSIGN] [MESSAGE]").arg(selectedCall), true,
            true);
    });

    auto qsoQueryAction = menu->addAction(
        QString("%1 QUERY CALL [CALLSIGN]? - Please acknowledge you can "
                "communicate directly with [CALLSIGN]")
            .arg(call)
            .trimmed());
    connect(qsoQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY CALL [CALLSIGN]?").arg(selectedCall),
                       true, true);
    });

    auto qsoQueryMsgsAction = menu->addAction(
        QString("%1 QUERY MSGS - Do you have any messages for me?")
            .arg(call)
            .trimmed());
    connect(qsoQueryMsgsAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY MSGS").arg(selectedCall), true, true);
    });

    auto qsoQueryMsgAction =
        menu->addAction(QString("%1 QUERY MSG [ID] - Please deliver the "
                                "complete message identified by ID")
                            .arg(call)
                            .trimmed());
    connect(qsoQueryMsgAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY MSG [ID]").arg(selectedCall), true,
                       true);
    });

    menu->addSeparator();

    auto agnAction = menu->addAction(
        QString("%1 AGN? - Please repeat your last transmission")
            .arg(call)
            .trimmed());
    connect(agnAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 AGN?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto qslQueryAction = menu->addAction(
        QString("%1 QSL? - Did you receive my last transmission?")
            .arg(call)
            .trimmed());
    connect(qslQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QSL?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto qslAction = menu->addAction(
        QString("%1 QSL - I confirm I received your last transmission")
            .arg(call)
            .trimmed());
    connect(qslAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QSL").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto yesAction = menu->addAction(
        QString("%1 YES - I confirm your last inquiry").arg(call).trimmed());
    connect(yesAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 YES").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto noAction =
        menu->addAction(QString("%1 NO - I do not confirm your last inquiry")
                            .arg(call)
                            .trimmed());
    connect(noAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 NO").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto hwAction = menu->addAction(
        QString("%1 HW CPY? - How do you copy?").arg(call).trimmed());
    connect(hwAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 HW CPY?").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto rrAction = menu->addAction(
        QString("%1 RR - Roger. Received. I copy.").arg(call).trimmed());
    connect(rrAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 RR").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto fbAction =
        menu->addAction(QString("%1 FB - Fine Business").arg(call).trimmed());
    connect(fbAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 FB").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto sevenThreeAction = menu->addAction(
        QString("%1 73 - I send my best regards").arg(call).trimmed());
    connect(sevenThreeAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 73").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto skAction =
        menu->addAction(QString("%1 SK - End of contact").arg(call).trimmed());
    connect(skAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 SK").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });

    auto ditDitAction = menu->addAction(
        QString("%1 DIT DIT - End of contact / Two bits").arg(call).trimmed());
    connect(ditDitAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 DIT DIT").arg(selectedCall), true);

        if (m_config.transmit_directed())
            toggleTx(true);
    });
}
