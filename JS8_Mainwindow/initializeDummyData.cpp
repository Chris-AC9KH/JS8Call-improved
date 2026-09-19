

/** \file
 * @brief member function of the UI_Constructor class
 *  group messaging function
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::initializeDummyData() {
    if (!QApplication::applicationName().contains("dummy")) {
        return;
    }

    ui->extFreeTextMsgEdit->setPlainText("HELLOBRAVE NEW WORLD");
    ui->extFreeTextMsgEdit->setCharsSent(6);

    logHeardGraph("KN4CRD", "OH8STN");
    logHeardGraph("KN4CRD", "K0OG");
    logHeardGraph("K0OG", "KN4CRD");

    auto path = QDir::toNativeSeparators(
        m_config.writeable_data_dir().absoluteFilePath(QString("test.db3")));
    auto inbox = Inbox(path);
    if (inbox.open()) {
        qCDebug(mainwindow_js8) << "test inbox opened"
                                << inbox.count("test", "$", "%") << "messages";

        // int i = inbox.append(Message("test", "booya1"));

        int i = inbox.append(Message("test", "booya2"));
        qCDebug(mainwindow_js8) << "i" << i;

        qCDebug(mainwindow_js8) << inbox.set(i, Message("test", "booya3"));

        auto m = inbox.value(i);
        qCDebug(mainwindow_js8) << QString(m.toJson());

        qCDebug(mainwindow_js8) << inbox.del(i);

        foreach (auto pair, inbox.values("test", "$", "%", 0, 5)) {
            qCDebug(mainwindow_js8)
                << pair.first << QString(pair.second.toJson());
        }
    }

    auto d = DecodedText("SN5-lUuJkby0", Varicode::JS8CallFirst, 1);
    qCDebug(mainwindow_js8) << "KN4CRD: K0OG ===>" << d.message();

    // qCDebug(mainwindow_js8) << Varicode::isValidCallsign("@GROUP1", nullptr);
    // qCDebug(mainwindow_js8) << Varicode::packAlphaNumeric50("VE7/KN4CRD");
    // qCDebug(mainwindow_js8) <<
    // Varicode::unpackAlphaNumeric50(Varicode::packAlphaNumeric50("VE7/KN4CRD"));
    // qCDebug(mainwindow_js8) <<
    // Varicode::unpackAlphaNumeric50(Varicode::packAlphaNumeric50("@GROUP/42"));
    // qCDebug(mainwindow_js8) <<
    // Varicode::unpackAlphaNumeric50(Varicode::packAlphaNumeric50("SP1ATOM"));

    if (!m_config.my_groups().contains("@GROUP42")) {
        m_config.addGroup("@GROUP42");
    }

    QList<QString> calls = {"KN4CRD", "VE7/KN4CRD", "KN4CRD/P", "KC9QNE",
                            "KI6SSI", "K0OG",       "LB9YH",    "VE7/LB9YH",
                            "M0IAX",  "N0JDS",      "OH8STN",   "VA3OSO",
                            "VK1MIC", "W0FW"};

    auto dt = DriftingDateTime::currentDateTimeUtc().addSecs(-300);

    int i = 0;
    foreach (auto call, calls) {
        CallDetail cd = {};
        cd.call = call;
        cd.through = i == 2 ? "KN4CRD" : "";
        cd.dial = 7078000;
        cd.offset = 500 + 100 * i;
        cd.snr = i == 3 ? -100 : i;
        cd.ackTimestamp = i == 1 ? dt.addSecs(-900) : QDateTime{};
        cd.utcTimestamp = dt;
        cd.grid = i == 5 ? "J042" : i == 6 ? " FN42FN42FN" : "";
        cd.tdrift = 0.1 * i;
        cd.submode = i % 3;
        logCallActivity(cd, false);

        ActivityDetail ad = {};
        ad.bits = Varicode::JS8CallFirst | Varicode::JS8CallLast;
        ad.snr = i == 3 ? -100 : i;
        ad.dial = 7078000;
        ad.offset = 500 + 100 * i;
        ad.text = QString("%1: %2 TEST MESSAGE")
                      .arg(call)
                      .arg(m_config.my_callsign());
        ad.utcTimestamp = dt;
        ad.submode = cd.submode;
        m_bandActivity[500 + 100 * i] = {ad};

        markOffsetDirected(500 + 100 * i, false);

        i++;
    }

    ActivityDetail adHB1 = {};
    adHB1.bits = Varicode::JS8CallFirst;
    adHB1.snr = 0;
    adHB1.dial = 7078000;
    adHB1.offset = 750;
    adHB1.text = QString("KN4CRD: HB AUTO EM73");
    adHB1.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
    adHB1.submode = Varicode::JS8CallNormal;
    m_bandActivity[750].append(adHB1);

    ActivityDetail adHB2 = {};
    adHB2.bits = Varicode::JS8CallLast;
    adHB2.snr = 0;
    adHB2.dial = 7078000;
    adHB2.offset = 750;
    adHB2.text = QString(" MSG ID 1");
    adHB2.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
    adHB2.submode = Varicode::JS8CallNormal;
    m_bandActivity[750].append(adHB2);

    CommandDetail cmd = {};
    cmd.cmd = ">";
    cmd.to = m_config.my_callsign();
    cmd.from = "N0JDS";
    cmd.relayPath = "N0JDS>OH8STN";
    cmd.text = "HELLO BRAVE SOUL";
    cmd.utcTimestamp = dt;
    cmd.submode = Varicode::JS8CallNormal;
    addCommandToMyInbox(cmd);

    QString eot = m_config.eot();

    displayTextForFreq(QString("KN4CRD: @ALLCALL? %1 ").arg(eot), 42,
                       DriftingDateTime::currentDateTimeUtc().addSecs(-315),
                       true, true, true);
    displayTextForFreq(QString("J1Y: KN4CRD SNR -05 %1 ").arg(eot), 42,
                       DriftingDateTime::currentDateTimeUtc().addSecs(-300),
                       false, true, true);
    displayTextForFreq(QString("HELLO BRAVE  NEW   WORLD    %1 ").arg(eot), 42,
                       DriftingDateTime::currentDateTimeUtc().addSecs(-300),
                       false, true, true);

    auto now = DriftingDateTime::currentDateTimeUtc();
    displayTextForFreq(QString("KN4CRD: JY1 ACK -12 %1 ").arg(eot), 780, now,
                       false, true, true);
    displayTextForFreq(QString("KN4CRD: JY1 ACK -12 %1 ").arg(eot), 780, now,
                       false, true, true); // should be hidden (duplicate)
    displayTextForFreq(QString("OH8STN: JY1 ACK -12 %1 ").arg(eot), 780, now,
                       false, true, true);

    displayTextForFreq(QString("KN4CRD: JY1 ACK -10 %1 ").arg(eot), 800, now,
                       false, true, true);
    displayTextForFreq(QString("KN4CRD: JY1 ACK -12 %1 ").arg(eot), 780,
                       now.addSecs(120), false, true, true);

    displayTextForFreq(QString("HELLO\\nBRAVE\\nNEW\\nWORLD %1 ").arg(eot),
                       1500, now, false, true, true);

    displayActivity(true);
}
