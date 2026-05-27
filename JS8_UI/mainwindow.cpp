/**
 * @file mainwindow.cpp
 * @brief source file that implements the JS8Call user interface
 *   executes member functions of the UI_Constructor class that provide
 *   all functionality of the JS8call main window
 */

#include "mainwindow.h"

#include "moc_mainwindow.cpp"

// TODO: Move to member:
static char message[29];
static char msgsent[29];
static int msgibits;

// How many milliseconds to wait before releasing PTT at end of transmission.
constexpr int TX_SWITCHOFF_DELAY = 200;

int volatile itone[JS8_NUM_SYMBOLS]; // Audio tones for all Tx symbols
struct dec_data dec_data;            // for sharing with Fortran
struct specData specData;            // Used by plotter
std::mutex fftw_mutex;

namespace {
int ms_minute_error() {
    auto const now = DriftingDateTime::currentDateTimeLocal();
    auto const time = now.time();
    auto const second = time.second();

    return now.msecsTo(now.addSecs(second > 30 ? 60 - second : -second)) -
           time.msec();
}

namespace State {
constexpr QStringView Ready = u"Ready";
constexpr QStringView Send = u"Send";
constexpr QStringView Sending = u"Sending";
constexpr QStringView Tuning = u"Tuning";

QString timed(QStringView const state, int const delay) {
    auto time = std::div(delay, 60);

    if (time.quot && time.rem)
        return QString("%1 (%2m %3s)").arg(state).arg(time.quot).arg(time.rem);
    else if (time.quot)
        return QString("%1 (%2m)").arg(state).arg(time.quot);
    else
        return QString("%1 (%2s)").arg(state).arg(time.rem);
}
} // namespace State

#if 0
  int round(int numToRound, int multiple)
  {
   if(multiple == 0)
   {
    return numToRound;
   }

   int roundDown = ( (int) (numToRound) / multiple) * multiple;

   if(numToRound - roundDown > multiple/2){
    return roundDown + multiple;
   }

   return roundDown;
  }
#endif

int roundUp(int numToRound, int multiple) {
    if (multiple == 0) {
        return numToRound;
    }

    int roundDown = (numToRound / multiple) * multiple;
    return roundDown + multiple;
}

// Copy at most size bytes into the array, filling any unused size
// with spaces if less than size bytes were available to copy. For
// convenience, return a one past the end iterator, i.e., equal to
// array + size.

auto copyByteData(QByteArrayView const bytes, char *const array,
                  qsizetype const size) {
    return std::fill_n(
        std::copy_n(bytes.begin(), std::min(size, bytes.size()), array),
        size - bytes.size(), ' ');
}

// Copy at most size bytes into the array, padding out the message
// with spaces if less than size bytes were available to copy, and
// null-terminate it. Caller is responsible for ensuring that at
// least (size + 1) bytes of space are available.

void copyMessage(QStringView const string, char *const array,
                 qsizetype const size = 28) {
    *copyByteData(string.toLocal8Bit(), array, size) = '\0';
}

} // namespace

// explicit member function of the UI_Constructor class
// this is the forward declaration of the constructor contained in
// JS8_Mainwindow/UI_Constructor.cpp
void UI_Constructor();

void UI_Constructor::ensureMessageDock()
{
    if (messageDock_) return;

    messagePanel_ = new MessagePanel(inboxPath(), this);

    messageDock_ = new QDockWidget(tr("Message Inbox"), this);
    messageDock_->setObjectName("messageInboxDock"); // important for save/restoreState
    messageDock_->setWidget(messagePanel_);

    // Choose where it can dock:
    messageDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::BottomDockWidgetArea);

    // Choose behavior:
    messageDock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);

    // Initial placement:
    addDockWidget(Qt::RightDockWidgetArea, messageDock_);

    // Optional: closing hides (default); ensure no auto-delete:
    messageDock_->setAttribute(Qt::WA_DeleteOnClose, false);

    // Make the menu action reflect visibility automatically:
    ui->actionShow_Message_Inbox->setCheckable(true);
    ui->actionShow_Message_Inbox->setChecked(messageDock_->isVisible());
    connect(messageDock_, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
                QSignalBlocker b(ui->actionShow_Message_Inbox);
                ui->actionShow_Message_Inbox->setChecked(visible);
            });

    // Handle reply function
    connect(messagePanel_, &MessagePanel::replyMessage, this,
                [this](const QString &text) {
                    addMessageText(text, true, true);
                    refreshInboxCounts();
                    displayCallActivity();
                });

    connect(messagePanel_, &MessagePanel::countsUpdated, this, [this]() {
            refreshInboxCounts();
            displayCallActivity();
        });
}

void UI_Constructor::checkStartupWarnings() {
    if (m_config.check_for_updates()) {
        checkVersion(false);
    }
    ensureCallsignSet(false);
}

void UI_Constructor::initialize_fonts() {
    set_application_font(m_config.text_font());

    setTextEditFont(ui->textEditRX, m_config.rx_text_font());
    setTextEditFont(ui->extFreeTextMsgEdit, m_config.tx_text_font());

    displayActivity(true);
}

void UI_Constructor::on_the_minute() {
    if (minuteTimer.isSingleShot()) {
        minuteTimer.setSingleShot(false);
        minuteTimer.start(60 * 1000); // run free
    } else {
        auto const &ms_error = ms_minute_error();
        if (qAbs(ms_error) > 1000) // keep drift within +-1s
        {
            minuteTimer.setSingleShot(true);
            minuteTimer.start(ms_error + 60 * 1000);
        }
    }

    if (m_config.watchdog()) {
        incrementIdleTimer();
        if (!m_tx_watchdog && m_idleMinutes >= m_config.watchdog()) {
            tx_watchdog(true);
        }
    } else {
        tx_watchdog(false);
    }
}

void UI_Constructor::tryBandHop() {
    // see if we need to hop bands...
    if (!m_config.auto_switch_bands()) {
        return;
    }

    // make sure we're not transmitting
    if (isMessageQueuedForTransmit()) {
        return;
    }

    // get the current band
    auto dialFreq = dialFrequency();

    auto currentBand = m_config.bands()->find(dialFreq);

    // get the stations list
    auto stations = m_config.stations()->station_list();

    // order stations by (switch_at, switch_until) time tuple
    std::stable_sort(
        stations.begin(), stations.end(),
        [](StationList::Station const &a, StationList::Station const &b) {
            return (a.switch_at_ < b.switch_at_) ||
                   (a.switch_at_ == b.switch_at_ &&
                    a.switch_until_ < b.switch_until_);
        });

    // we just set the date to a known y/m/d to make the comparisons easier
    QDateTime d = DriftingDateTime::currentDateTimeUtc();
    d.setDate(QDate(2000, 1, 1));

    QDateTime startOfDay =
        QDateTime(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::utc());
    QDateTime endOfDay =
        QDateTime(QDate(2000, 1, 1), QTime(23, 59, 59, 999), QTimeZone::utc());

    StationList::Station *hopStation = nullptr;

    // See if we can find a needed band switch...
    // In the case of overlapping windows, choose the latest one
    foreach (auto station, stations) {
        // we can switch to this frequency if we're in the time range, inclusive
        // of switch_at, exclusive of switch_until and if we are switching to a
        // different frequency than the last hop. this allows us to switch bands
        // at that time, but then later we can later switch to a different band
        // if needed without the automatic band switching to take over
        bool inTimeRange =
            ((station.switch_at_ <= d &&
              d <= station.switch_until_) || // <- normal range, 12-16 && 6-8,
                                             // evaluated as 12 <= d <= 16 || 6
                                             // <= d <= 8

             (station.switch_until_ < station.switch_at_ &&
              ( // <- say for a range of 12->2 & 2->12;  12->2,
                  (station.switch_at_ <= d &&
                   d <= endOfDay) || //    should be evaluated as 12 <= d <=
                                     //    23:59 || 00:00 <= d <= 2
                  (startOfDay <= d && d <= station.switch_until_))));

        if (inTimeRange) {
            delete hopStation;
            hopStation = new StationList::Station(station);
        }
    }

    // If we have a candidate station, see if the hop is valid, and if so, do it
    if (hopStation != nullptr) {
        bool noOverride =
            (m_bandHopped ||
             (!m_bandHopped && hopStation->frequency_ != m_bandHoppedFreq));

        bool freqIsDifferent = (hopStation->frequency_ != dialFreq);

        bool canSwitch = (noOverride && freqIsDifferent);

        // switch, if we can and the band is different than our current band
        if (canSwitch) {
            Frequency frequency = hopStation->frequency_;

            m_bandHopped = false;
            m_bandHoppedFreq = frequency;

            SelfDestructMessageBox *m = new SelfDestructMessageBox(
                30, "Scheduled Frequency Change",
                QString("A scheduled frequency change has arrived. The rig "
                        "frequency will be changed to %1 MHz in %2 second(s).")
                    .arg(Radio::frequency_MHz_string(frequency)),
                QMessageBox::Information, QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok, true, this);

            connect(m, &SelfDestructMessageBox::finished, this,
                    [this, m, frequency]() {
                        if (m->result() == QMessageBox::Ok) {
                            m_bandHopped = true;
                            setRig(frequency);
                        }
                        m->deleteLater();
                    });

            m->show();

#if 0
		  // TODO: jsherer - this is totally a hack because of the signal that gets emitted to clearActivity on band change...
          QTimer *t = new QTimer(this);
          t->setInterval(250);
          t->setSingleShot(true);
          connect(t, &QTimer::timeout, this, [this, frequency, dialFreq](){
              auto message = QString("Scheduled frequency switch from %1 MHz to %2 MHz");
              message = message.arg(Radio::frequency_MHz_string(dialFreq));
              message = message.arg(Radio::frequency_MHz_string(frequency));
              writeNoticeTextToUI(DriftingDateTime::currentDateTimeUtc(), message);
          });
          t->start();
#endif

            return;
        }

        delete hopStation;
    }
}

void UI_Constructor::manualBandHop(const StationList::Station station) {
    // make sure we're not transmitting
    if (isMessageQueuedForTransmit()) {
        return;
    }

    Frequency frequency = station.frequency_;

    m_bandHopped = true;
    m_bandHoppedFreq = frequency;
    setRig(frequency);
}

//--------------------------------------------------- UI_Constructor destructor
UI_Constructor::~UI_Constructor() {
    {
        std::lock_guard<std::mutex> lock(fftw_mutex);
        fftwf_export_wisdom_to_filename(wisdomFileName());
    }

    m_networkThread.quit();
    m_networkThread.wait();

    m_audioThread.quit();
    m_audioThread.wait();

    m_notificationAudioThread.quit();
    m_notificationAudioThread.wait();

    m_decoder.quit();

    remove_child_from_event_filter(this);
}

//-------------------------------------------------------- writeSettings()
void UI_Constructor::writeSettings() {
    m_settings->beginGroup("UI_Constructor");
    m_settings->setValue("geometry", saveGeometry());
    m_settings->setValue("geometryNoControls", m_geometryNoControls);
    m_settings->setValue("state", saveState());

    m_settings->setValue("MainSplitter", ui->mainSplitter->saveState());
    m_settings->setValue("TextHorizontalSplitter",
                         ui->textHorizontalSplitter->saveState());
    m_settings->setValue("BandActivityVisible",
                         ui->tableWidgetRXAll->isVisible());
    m_settings->setValue("BandHBActivityVisible",
                         ui->actionShow_Band_Heartbeats_and_ACKs->isChecked());
    m_settings->setValue("TextVerticalSplitter",
                         ui->textVerticalSplitter->saveState());
    m_settings->setValue("TimeDrift", DriftingDateTime::drift());
    m_settings->setValue("ShowTooltips", ui->actionShow_Tooltips->isChecked());
    m_settings->setValue("ShowStatusbar", ui->statusBar->isVisible());
    m_settings->setValue("RXActivity", ui->textEditRX->toHtml());

    m_settings->endGroup();

    m_settings->beginGroup("Common");
    m_settings->setValue("Freq", freq());
    m_settings->setValue("SubMode", m_nSubMode);
    m_settings->setValue("SubModeHB", ui->actionModeJS8HB->isChecked());
    m_settings->setValue("SubModeHBAck",
                         ui->actionHeartbeatAcknowledgements->isChecked());
    m_settings->setValue("SubModeMultiDecode",
                         ui->actionModeMultiDecoder->isChecked());
    m_settings->setValue("DialFreq",
                         QVariant::fromValue(m_lastMonitoredFrequency));
    m_settings->setValue("OutAttenuation", ui->outAttenuation->value());
    m_settings->setValue("pwrBandTxMemory", m_pwrBandTxMemory);
    m_settings->setValue("pwrBandTuneMemory", m_pwrBandTuneMemory);
    m_settings->setValue("SortBy", QVariant(m_sortCache));
    m_settings->setValue("ShowColumns", QVariant(m_showColumnsCache));
    m_settings->setValue("HBInterval", m_hbInterval);
    m_settings->setValue("CQInterval", m_cqInterval);

    // TODO: jsherer - need any other customizations?
    /*m_settings->setValue("PanelLeftGeometry",
    ui->tableWidgetRXAll->geometry());
    m_settings->setValue("PanelRightGeometry",
    ui->tableWidgetCalls->geometry()); m_settings->setValue("PanelTopGeometry",
    ui->extFreeTextMsg->geometry()); m_settings->setValue("PanelBottomGeometry",
    ui->extFreeTextMsgEdit->geometry());
    m_settings->setValue("PanelWaterfallGeometry",
    ui->bandHorizontalWidget->geometry());*/
    // m_settings->setValue("MainSplitter",
    // QVariant::fromValue(ui->mainSplitter->sizes()));

    m_settings->endGroup();

    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();

    m_settings->beginGroup("CallActivity");
    m_settings->remove(""); // remove all keys in current group
    foreach (auto cd, m_callActivity.values()) {
        if (cd.call.trimmed().isEmpty()) {
            continue;
        }
        if (callsignAging &&
            cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
            continue;
        }
        m_settings->setValue(
            cd.call.trimmed(),
            QVariantMap{
                {"snr", QVariant(cd.snr)},
                {"grid", QVariant(cd.grid)},
                {"dial", QVariant(cd.dial)},
                {"freq", QVariant(cd.offset)},
                {"tdrift", QVariant(cd.tdrift)},
#if CACHE_CALL_DATETIME_AS_STRINGS
                {"ackTimestamp",
                 QVariant(cd.ackTimestamp.toString("yyyy-MM-dd hh:mm:ss"))},
                {"utcTimestamp",
                 QVariant(cd.utcTimestamp.toString("yyyy-MM-dd hh:mm:ss"))},
#else
                {"ackTimestamp", QVariant(cd.ackTimestamp)},
                {"utcTimestamp", QVariant(cd.utcTimestamp)},
#endif
                {"submode", QVariant(cd.submode)},
            });
    }
    m_settings->endGroup();
}

void UI_Constructor::applyPillSettings() {
    if (auto *pr = ui->extFreeTextMsgEdit->pillRenderer()) {
        pr->setPillColors({m_config.color_pill_recipient_bg(),
                           m_config.color_pill_recipient_fg(),
                           m_config.color_pill_command_bg(),
                           m_config.color_pill_command_fg(),
                           m_config.color_pill_group_bg(),
                           m_config.color_pill_group_fg(),
                           m_config.color_pill_sender_bg(),
                           m_config.color_pill_sender_fg()});
        pr->setEnabled(m_config.pills_enabled());
    }
}

//---------------------------------------------------------- readSettings()
void UI_Constructor::readSettings() {
    m_settings->beginGroup("UI_Constructor");
    ensureMessageDock();
    setMinimumSize(800, 400);
    restoreGeometry(
        m_settings->value("geometry", saveGeometry()).toByteArray());
    setMinimumSize(800, 400);

    m_geometryNoControls =
        m_settings->value("geometryNoControls", saveGeometry()).toByteArray();
    restoreState(m_settings->value("state", saveState()).toByteArray());

    auto mainSplitterState = m_settings->value("MainSplitter").toByteArray();
    if (!mainSplitterState.isEmpty()) {
        ui->mainSplitter->restoreState(mainSplitterState);
    }
    auto horizontalState =
        m_settings->value("TextHorizontalSplitter").toByteArray();
    if (!horizontalState.isEmpty()) {
        ui->textHorizontalSplitter->restoreState(horizontalState);
        auto hsizes = ui->textHorizontalSplitter->sizes();

        ui->tableWidgetRXAll->setVisible(hsizes.at(0) > 0);
        ui->tableWidgetCalls->setVisible(hsizes.at(2) > 0);
    }

    m_bandActivityWasVisible =
        m_settings->value("BandActivityVisible", true).toBool();
    ui->tableWidgetRXAll->setVisible(m_bandActivityWasVisible);

    auto verticalState =
        m_settings->value("TextVerticalSplitter").toByteArray();
    if (!verticalState.isEmpty()) {
        ui->textVerticalSplitter->restoreState(verticalState);
    }
    setDrift(m_settings->value("TimeDrift", 0).toInt());
    ui->actionShow_Waterfall_Controls->setChecked(
        m_wideGraph->controlsVisible());
    ui->actionShow_Waterfall_Time_Drift_Controls->setChecked(
        m_wideGraph->timeControlsVisible());
    ui->actionShow_Tooltips->setChecked(
        m_settings->value("ShowTooltips", true).toBool());
    ui->actionShow_Statusbar->setChecked(
        m_settings->value("ShowStatusbar", true).toBool());
    ui->statusBar->setVisible(ui->actionShow_Statusbar->isChecked());
    ui->textEditRX->setHtml(
        m_config.reset_activity()
            ? ""
            : m_settings->value("RXActivity", "").toString());
    QTimer::singleShot(0, this, [this](){
        ui->textEditRX->verticalScrollBar()->setValue(
            ui->textEditRX->verticalScrollBar()->maximum());
    });
    ui->actionShow_Band_Heartbeats_and_ACKs->setChecked(
        m_settings->value("BandHBActivityVisible", true).toBool());
    m_settings->endGroup();

    m_settings->beginGroup("Common");

    // set the frequency offset
    changeFreq(
        m_settings->value("Freq", Default::FREQUENCY).toInt()); // XXX

    setSubmode(m_settings->value("SubMode", Default::SUBMODE).toInt());
    ui->actionModeJS8HB->setChecked(
        m_settings->value("SubModeHB", false).toBool());
    ui->actionHeartbeatAcknowledgements->setChecked(
        m_settings->value("SubModeHBAck", false).toBool());
    ui->actionModeMultiDecoder->setChecked(
        m_settings->value("SubModeMultiDecode", true).toBool());

    m_lastMonitoredFrequency =
        m_settings
            ->value("DialFreq",
                    QVariant::fromValue<Frequency>(Default::DIAL_FREQUENCY))
            .value<Frequency>();
    setFreq(0); // ensure a change is signaled
    setFreq(m_settings->value("Freq", Default::FREQUENCY).toInt());
    // setup initial value of tx attenuator
    m_block_pwr_tooltip = true;
    ui->outAttenuation->setValue(
        m_settings->value("OutAttenuation", 0).toInt());
    m_block_pwr_tooltip = false;
    m_pwrBandTxMemory = m_settings->value("pwrBandTxMemory").toHash();
    m_pwrBandTuneMemory = m_settings->value("pwrBandTuneMemory").toHash();

    m_sortCache = m_settings->value("SortBy").toMap();
    m_showColumnsCache = m_settings->value("ShowColumns").toMap();
    m_hbInterval = m_settings->value("HBInterval", 0).toInt();
    m_cqInterval = m_settings->value("CQInterval", 0).toInt();

    // TODO: jsherer - any other customizations?
    // ui->mainSplitter->setSizes(m_settings->value("MainSplitter",
    // QVariant::fromValue(ui->mainSplitter->sizes())).value<QList<int> >());
    // ui->tableWidgetRXAll->restoreGeometry(m_settings->value("PanelLeftGeometry",
    // ui->tableWidgetRXAll->saveGeometry()).toByteArray());
    // ui->tableWidgetCalls->restoreGeometry(m_settings->value("PanelRightGeometry",
    // ui->tableWidgetCalls->saveGeometry()).toByteArray());
    // ui->extFreeTextMsg->setGeometry( m_settings->value("PanelTopGeometry",
    // ui->extFreeTextMsg->geometry()).toRect());
    // ui->extFreeTextMsgEdit->setGeometry(
    // m_settings->value("PanelBottomGeometry",
    // ui->extFreeTextMsgEdit->geometry()).toRect());
    // ui->bandHorizontalWidget->setGeometry(
    // m_settings->value("PanelWaterfallGeometry",
    // ui->bandHorizontalWidget->geometry()).toRect()); qCDebug(mainwindow_js8)
    // << m_settings->value("PanelTopGeometry") << ui->extFreeTextMsg;

    setTextEditStyle(ui->textEditRX, m_config.color_rx_foreground(),
                     m_config.color_rx_background(), m_config.rx_text_font());
    setTextEditStyle(
        ui->extFreeTextMsgEdit, m_config.color_compose_foreground(),
        m_config.color_compose_background(), m_config.compose_text_font());
    ui->extFreeTextMsgEdit->setFont(m_config.compose_text_font(),
                                    m_config.color_compose_foreground(),
                                    m_config.color_compose_background());
    applyPillSettings();
    ui->extFreeTextMsgEdit->highlight();

    m_settings->endGroup();

    // use these initialisation settings to tune the audio o/p buffer
    // size and audio thread priority
    m_settings->beginGroup("Tune");
    m_msAudioOutputBuffered = m_settings->value("Audio/OutputBufferMs").toInt();
    m_framesAudioInputBuffered =
        m_settings->value("Audio/InputBufferFrames", JS8_RX_SAMPLE_RATE / 10)
            .toInt();
    m_audioThreadPriority = static_cast<QThread::Priority>(
        m_settings->value("Audio/ThreadPriority", QThread::TimeCriticalPriority)
            .toInt() %
        8);
    m_notificationAudioThreadPriority = static_cast<QThread::Priority>(
        m_settings
            ->value("Audio/NotificationThreadPriority", QThread::LowPriority)
            .toInt() %
        8);
    m_decoderThreadPriority = static_cast<QThread::Priority>(
        m_settings->value("Audio/DecoderThreadPriority", QThread::HighPriority)
            .toInt() %
        8);
    m_networkThreadPriority = static_cast<QThread::Priority>(
        m_settings->value("Network/NetworkThreadPriority", QThread::LowPriority)
            .toInt() %
        8);
    m_settings->endGroup();

    if (m_config.reset_activity()) {
        // NOOP
    } else {
        m_settings->beginGroup("CallActivity");
        foreach (auto call, m_settings->allKeys()) {

            auto values = m_settings->value(call).toMap();

            auto snr = values.value("snr", -64).toInt();
            auto grid = values.value("grid", "").toString();
            auto dial = values.value("dial", 0).toInt();
            auto freq = values.value("freq", 0).toInt();
            auto tdrift = values.value("tdrift", 0).toFloat();

#if CACHE_CALL_DATETIME_AS_STRINGS
            auto ackTimestampStr = values.value("ackTimestamp", "").toString();
            auto ackTimestamp =
                QDateTime::fromString(ackTimestampStr, "yyyy-MM-dd hh:mm:ss");
            ackTimestamp.setUtcOffset(0);

            auto utcTimestampStr = values.value("utcTimestamp", "").toString();
            auto utcTimestamp =
                QDateTime::fromString(utcTimestampStr, "yyyy-MM-dd hh:mm:ss");
            utcTimestamp.setUtcOffset(0);
#else
            auto ackTimestamp = values.value("ackTimestamp").toDateTime();
            auto utcTimestamp = values.value("utcTimestamp").toDateTime();
#endif
            auto submode =
                values.value("submode", Varicode::JS8CallNormal).toInt();

            CallDetail cd = {};
            cd.call = call;
            cd.snr = snr;
            cd.grid = grid;
            cd.dial = dial;
            cd.offset = freq;
            cd.tdrift = tdrift;
            cd.ackTimestamp = ackTimestamp;
            cd.utcTimestamp = utcTimestamp;
            cd.submode = submode;

            logCallActivity(cd, false);
        }
        m_settings->endGroup();
    }

    QTimer::singleShot(0, this, [this]{
        if (!messageDock_) return;

        // If restoreState made it floating, force Qt to recreate the floating window.
        if (messageDock_->isFloating() && messageDock_->isVisible()) {
            messageDock_->setFloating(false);
            messageDock_->setFloating(true);

            messageDock_->show();
            messageDock_->raise();
        }
    });

    m_settings_read = true;
}

void UI_Constructor::set_application_font(QFont const &font) {
    qApp->setFont(font);
    // set font in the application style sheet as well in case it has
    // been modified in the style sheet which has priority
    qApp->setStyleSheet(qApp->styleSheet() + "* {" + font_as_stylesheet(font) +
                        '}');
    for (auto &widget : qApp->topLevelWidgets()) {
        widget->updateGeometry();
    }
}

void UI_Constructor::showSoundInError(const QString &errorMsg) {
    JS8MessageBox::critical_message(this, tr("Error in Sound Input"), errorMsg);
}

void UI_Constructor::showSoundOutError(const QString &errorMsg) {
    JS8MessageBox::critical_message(this, tr("Error in Sound Output"),
                                    errorMsg);
}

void UI_Constructor::showStatusMessage(const QString &statusMsg) {
    statusBar()->showMessage(statusMsg, 5000);
}

void UI_Constructor::on_menuModeJS8_aboutToShow() {
    bool canChangeMode =
        !m_transmitting && m_txFrameCount == 0 && m_txFrameQueue.isEmpty();
    ui->actionModeJS8Normal->setEnabled(canChangeMode);
    ui->actionModeJS8Fast->setEnabled(canChangeMode);
    ui->actionModeJS8Turbo->setEnabled(canChangeMode);
    ui->actionModeJS8Slow->setEnabled(canChangeMode);
    ui->actionModeJS8Ultra->setEnabled(canChangeMode);

    // dynamically replace the autoreply menu item text
    auto autoreplyText = ui->actionModeAutoreply->text();
    if (m_config.autoreply_confirmation() &&
        !autoreplyText.contains(" with Confirmation")) {
        autoreplyText.replace("Autoreply", "Autoreply with Confirmation");
        autoreplyText.replace("&AUTO", "&AUTO+CONF");
        ui->actionModeAutoreply->setText(autoreplyText);
    } else if (!m_config.autoreply_confirmation() &&
               autoreplyText.contains(" with Confirmation")) {
        autoreplyText.replace(" with Confirmation", "");
        autoreplyText.replace("+CONF", "");
        ui->actionModeAutoreply->setText(autoreplyText);
    }
}

void UI_Constructor::on_menuControl_aboutToShow() {
    auto freqMenu = new QMenu(this->menuBar());
    buildFrequencyMenu(freqMenu);
    ui->actionSetFrequency->setMenu(freqMenu);

    auto heartbeatMenu = new QMenu(this->menuBar());
    buildHeartbeatMenu(heartbeatMenu);
    ui->actionHeartbeat->setMenu(heartbeatMenu);

    auto cqMenu = new QMenu(this->menuBar());
    buildCQMenu(cqMenu);
    ui->actionCQ->setMenu(cqMenu);

    ui->actionEnable_Monitor_RX->setChecked(ui->monitorButton->isChecked());
    ui->actionEnable_Transmitter_TX->setChecked(
        ui->monitorTxButton->isChecked());
    ui->actionEnable_Reporting_SPOT->setChecked(ui->spotButton->isChecked());
    ui->actionEnable_Tuning_Tone_TUNE->setChecked(ui->tuneButton->isChecked());
}

void UI_Constructor::on_actionCheck_for_Updates_triggered() {
    checkVersion(true);
}

void UI_Constructor::on_actionUser_Guide_triggered() {
    QDesktopServices::openUrl(
        QUrl("https://js8call-improved.github.io/JS8Call-improved/d6/d14/md_docs_2user__guide_2JS8Call__User__Guide.html"));
}

void UI_Constructor::on_actionEnable_Monitor_RX_toggled(bool checked) {
    ui->monitorButton->setChecked(checked);
}

void UI_Constructor::on_actionEnable_Transmitter_TX_toggled(bool checked) {
    ui->monitorTxButton->setChecked(checked);
}

void UI_Constructor::on_actionEnable_Reporting_SPOT_toggled(bool checked) {
    ui->spotButton->setChecked(checked);
}

void UI_Constructor::on_actionEnable_Tuning_Tone_TUNE_toggled(bool checked) {
    ui->tuneButton->setChecked(checked);
    on_tuneButton_clicked(checked);
}

void UI_Constructor::on_menuWindow_aboutToShow() {
    ui->actionShow_Fullscreen->setChecked(
        (windowState() & Qt::WindowFullScreen) == Qt::WindowFullScreen);

    ui->actionShow_Statusbar->setChecked(ui->statusBar &&
                                         ui->statusBar->isVisible());

    auto hsizes = ui->textHorizontalSplitter->sizes();
    ui->actionShow_Band_Activity->setChecked(hsizes.at(0) > 0);
    ui->actionShow_Call_Activity->setChecked(hsizes.at(2) > 0);

    auto vsizes = ui->mainSplitter->sizes();
    ui->actionShow_Frequency_Clock->setChecked(vsizes.first() > 0);
    ui->actionShow_Waterfall->setChecked(vsizes.last() > 0);
    ui->actionShow_Waterfall_Controls->setChecked(
        ui->actionShow_Waterfall->isChecked() &&
        m_wideGraph->controlsVisible());
    ui->actionShow_Waterfall_Time_Drift_Controls->setChecked(
        ui->actionShow_Waterfall->isChecked() &&
        m_wideGraph->timeControlsVisible());

    QMenu *sortBandMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildBandActivitySortByMenu(sortBandMenu);
    ui->actionSort_Band_Activity->setMenu(sortBandMenu);
    ui->actionSort_Band_Activity->setEnabled(
        ui->actionShow_Band_Activity->isChecked());

    QMenu *sortCallMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildCallActivitySortByMenu(sortCallMenu);
    ui->actionSort_Call_Activity->setMenu(sortCallMenu);
    ui->actionSort_Call_Activity->setEnabled(
        ui->actionShow_Call_Activity->isChecked());

    QMenu *showBandMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildShowColumnsMenu(showBandMenu, "band");
    ui->actionShow_Band_Activity_Columns->setMenu(showBandMenu);
    ui->actionShow_Band_Activity_Columns->setEnabled(
        ui->actionShow_Band_Activity->isChecked());

    QMenu *showCallMenu = new QMenu(this->menuBar()); // ui->menuWindow);
    buildShowColumnsMenu(showCallMenu, "call");
    ui->actionShow_Call_Activity_Columns->setMenu(showCallMenu);
    ui->actionShow_Call_Activity_Columns->setEnabled(
        ui->actionShow_Call_Activity->isChecked());

    ui->actionShow_Band_Heartbeats_and_ACKs->setEnabled(
        ui->actionShow_Band_Activity->isChecked());
}

void UI_Constructor::on_actionFocus_Message_Receive_Area_triggered() {
    ui->textEditRX->setFocus();
}

void UI_Constructor::on_actionFocus_Message_Reply_Area_triggered() {
    ui->extFreeTextMsgEdit->setFocus();
}

void UI_Constructor::on_actionFocus_Band_Activity_Table_triggered() {
    ui->tableWidgetRXAll->setFocus();
}

void UI_Constructor::on_actionFocus_Call_Activity_Table_triggered() {
    ui->tableWidgetCalls->setFocus();
}

void UI_Constructor::on_actionClear_All_Activity_triggered() {
    clearActivity();
}

void UI_Constructor::on_actionClear_Band_Activity_triggered() {
    clearBandActivity();
}

void UI_Constructor::on_actionClear_RX_Activity_triggered() {
    clearRXActivity();
}

void UI_Constructor::on_actionClear_Call_Activity_triggered() {
    clearCallActivity();
}

void UI_Constructor::on_actionSetOffset_triggered() {
    bool ok = false;
    auto const currentFreq = freq();
    QString newFreq =
        QInputDialog::getText(this, tr("Set Frequency Offset"),
                              tr("Offset in Hz:"), QLineEdit::Normal,
                              QString("%1").arg(currentFreq), &ok)
            .toUpper()
            .trimmed();
    int offset = newFreq.toInt(&ok);
    if (!ok) {
        return;
    }

    changeFreq(offset);
}

void UI_Constructor::on_actionShow_Fullscreen_triggered(bool checked) {
    auto state = windowState();
    if (checked) {
        state |= Qt::WindowFullScreen;
    } else {
        state &= ~Qt::WindowFullScreen;
    }
    setWindowState(state);
}

void UI_Constructor::on_actionShow_Statusbar_triggered(bool checked) {
    if (!ui->statusBar) {
        return;
    }

    ui->statusBar->setVisible(checked);
}

void UI_Constructor::on_actionShow_Frequency_Clock_triggered(bool checked) {
    auto vsizes = ui->mainSplitter->sizes();
    vsizes[0] = checked ? ui->logHorizontalWidget->minimumHeight() : 0;
    ui->logHorizontalWidget->setVisible(checked);
    ui->mainSplitter->setSizes(vsizes);
}

void UI_Constructor::on_actionShow_Band_Activity_triggered(bool checked) {
    auto hsizes = ui->textHorizontalSplitter->sizes();

    if (m_bandActivityWidth == 0) {
        m_bandActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_callActivityWidth == 0) {
        m_callActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_textActivityWidth == 0) {
        m_textActivityWidth = ui->textHorizontalSplitter->width() / 2;
    }

    if (checked) {
        hsizes[0] = m_bandActivityWidth;
        hsizes[1] = m_textActivityWidth;
        if (hsizes[2])
            hsizes[2] = m_callActivityWidth;

    } else {
        if (hsizes[0])
            m_bandActivityWidth = hsizes[0];
        if (hsizes[1])
            m_textActivityWidth = hsizes[1];
        if (hsizes[2])
            m_callActivityWidth = hsizes[2];
        hsizes[0] = 0;
    }

    ui->textHorizontalSplitter->setSizes(hsizes);
    ui->tableWidgetRXAll->setVisible(checked);
    m_bandActivityWasVisible = checked;
}

void UI_Constructor::on_actionShow_Band_Heartbeats_and_ACKs_triggered(bool) {
    displayBandActivity();
}

void UI_Constructor::on_actionShow_Call_Activity_triggered(bool checked) {
    auto hsizes = ui->textHorizontalSplitter->sizes();

    if (m_bandActivityWidth == 0) {
        m_bandActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_callActivityWidth == 0) {
        m_callActivityWidth = ui->textHorizontalSplitter->width() / 4;
    }

    if (m_textActivityWidth == 0) {
        m_textActivityWidth = ui->textHorizontalSplitter->width() / 2;
    }

    if (checked) {
        if (hsizes[0])
            hsizes[0] = m_bandActivityWidth;
        hsizes[1] = m_textActivityWidth;
        hsizes[2] = m_callActivityWidth;

    } else {
        if (hsizes[0])
            m_bandActivityWidth = hsizes[0];
        if (hsizes[1])
            m_textActivityWidth = hsizes[1];
        if (hsizes[2])
            m_callActivityWidth = hsizes[2];
        hsizes[2] = 0;
    }

    ui->textHorizontalSplitter->setSizes(hsizes);
    ui->tableWidgetCalls->setVisible(checked);
}

void UI_Constructor::on_actionShow_Waterfall_triggered(bool checked) {
    auto vsizes = ui->mainSplitter->sizes();

    if (m_waterfallHeight == 0) {
        m_waterfallHeight = ui->mainSplitter->height() / 4;
    }

    if (checked) {
        vsizes[vsizes.length() - 1] = m_waterfallHeight;

    } else {
        m_waterfallHeight = vsizes[vsizes.length() - 1];
        vsizes[1] += m_waterfallHeight;
        vsizes[vsizes.length() - 1] = 0;
    }

    ui->mainSplitter->setSizes(vsizes);
    ui->bandHorizontalWidget->setVisible(checked);
}

void UI_Constructor::on_actionShow_Waterfall_Controls_triggered(bool checked) {
    m_wideGraph->setControlsVisible(checked);
    if (checked && !ui->bandHorizontalWidget->isVisible()) {
        on_actionShow_Waterfall_triggered(checked);
    }
}

void UI_Constructor::on_actionShow_Waterfall_Time_Drift_Controls_triggered(
    bool checked) {
    m_wideGraph->setTimeControlsVisible(checked);
    if (checked && !ui->bandHorizontalWidget->isVisible()) {
        on_actionShow_Waterfall_triggered(checked);
    }
}

void UI_Constructor::on_actionReset_Window_Sizes_triggered() {
    // auto size = this->centralWidget()->size();

    ui->mainSplitter->setSizes({ui->logHorizontalWidget->minimumHeight(),
                                ui->mainSplitter->height() / 2,
                                ui->macroHorizonalWidget->minimumHeight(),
                                ui->mainSplitter->height() / 4});

    ui->textHorizontalSplitter->setSizes(
        {ui->textHorizontalSplitter->width() / 4,
         ui->textHorizontalSplitter->width() / 2,
         ui->textHorizontalSplitter->width() / 4});

    ui->textVerticalSplitter->setSizes(
        {ui->textVerticalSplitter->height() / 2,
         ui->textVerticalSplitter->height() / 2});
}

void UI_Constructor::on_actionSettings_triggered() { openSettings(); }

void UI_Constructor::openSettings(int tab) {
    m_config.select_tab(tab);
    
    // Save scroll position before opening settings
    QScrollBar *bar = ui->textEditRX->verticalScrollBar();
    const bool wasAtBottom = (bar->value() == bar->maximum());
    const int savedPos = bar->value();

    // things that might change that we need know about
    auto callsign = m_config.my_callsign();
    auto my_grid = m_config.my_grid();
    auto spot_on = m_config.spot_to_reporting_networks();
    if (QDialog::Accepted == m_config.exec()) {
        if (m_config.my_callsign() != callsign) {
            m_baseCall = Radio::base_callsign(m_config.my_callsign());
        }
        if (m_config.my_callsign() != callsign ||
            m_config.my_grid() != my_grid) {
            statusUpdate();
        }

        enable_DXCC_entity(m_config.DXCC()); // sets text window proportions and
                                             // (re)inits the logbook

        prepareApi();
        prepareSpotting();

        // this will close the connection to PSKReporter if it has been
        // disabled
        if (spot_on && !m_config.spot_to_reporting_networks()) {
            Q_EMIT pskReporterSendReport(true);
        }

        if (m_config.restart_audio_input() &&
            !m_config.audio_input_device().isNull()) {
            Q_EMIT startAudioInputStream(m_config.audio_input_device(),
                                         m_framesAudioInputBuffered, m_detector,
                                         m_config.audio_input_channel());
        }

        if (m_config.restart_audio_output() &&
            !m_config.audio_output_device().isNull()) {
            Q_EMIT initializeAudioOutputStream(
                m_config.audio_output_device(),
                AudioDevice::Mono == m_config.audio_output_channel() ? 1 : 2,
                m_msAudioOutputBuffered);
        }

        if (m_config.restart_notification_audio_output() &&
            !m_config.notification_audio_output_device().isNull()) {
            Q_EMIT initializeNotificationAudioOutputStream(
                m_config.notification_audio_output_device(),
                m_msAudioOutputBuffered);
        }

        displayDialFrequency();
        displayActivity(true);

        setup_status_bar();
        setupJS8();

        m_config.transceiver_online();

        setXIT(freq());

        m_opCall = m_config.opCall();
    }
    
    // Restore scroll position after settings dialog
    QTimer::singleShot(0, this, [this, wasAtBottom, savedPos](){
        QScrollBar *bar = ui->textEditRX->verticalScrollBar();
        if (wasAtBottom) {
            bar->setValue(bar->maximum());
        } else {
            bar->setValue(savedPos);
        }
    });
}

void UI_Constructor::prepareApi() {
    // the udp api is prepared by default (always listening)

    // so, we just need to prepare the tcp api
    bool enabled = m_config.tcpEnabled();
    if (enabled) {
        emit apiSetMaxConnections(m_config.tcp_max_connections());
        emit apiSetServer(m_config.tcp_server_name(),
                          m_config.tcp_server_port());
        emit apiStartServer();
    } else {
        emit apiStopServer();
    }
}

void UI_Constructor::prepareSpotting() {
    bool reportingEnabled = m_config.spot_to_reporting_networks();
    bool aprsEnabled = reportingEnabled &&
                       (m_config.spot_to_aprs() || m_config.spot_to_aprs_relay());

    if (reportingEnabled) {
        spotSetLocal();
        pskSetLocal();
        if (aprsEnabled) {
            aprsSetLocal();
            emit aprsClientSetSkipPercent(0.25);
            emit aprsClientSetServer(m_config.aprs_server_name(),
                                     m_config.aprs_server_port());
        }
        emit aprsClientSetIncomingRelayEnabled(
            aprsEnabled && m_config.spot_to_aprs_relay());
        emit aprsClientSetPaused(!aprsEnabled);
        ui->spotButton->setChecked(true);
    } else {
        emit aprsClientSetPaused(true);
        emit aprsClientSetIncomingRelayEnabled(false);
        ui->spotButton->setChecked(false);
    }
}

void UI_Constructor::on_spotButton_clicked(bool checked) {
    // 1. save setting
    m_config.set_spot_to_reporting_networks(checked);

    // 2. prepare
    prepareApi();
    prepareSpotting();
}

void UI_Constructor::on_monitorButton_clicked(bool checked) {
    if (!m_transmitting) {
        auto prior = m_monitoring;
        monitor(checked);
        if (checked && !prior) {
            if (m_config.monitor_last_used()) {
                // put rig back where it was when last in control
                setRig(m_lastMonitoredFrequency);
                setXIT(freq());
            }
            setFreq(freq()); // ensure FreqCal triggers
        }
        // Get Configuration in/out of strict split and mode checking
        Q_EMIT m_config.sync_transceiver(true, checked);
    } else {
        ui->monitorButton->setChecked(false); // disallow
    }
}

void UI_Constructor::monitor(bool state) {
    ui->monitorButton->setChecked(state);

    // make sure widegraph is running if we are monitoring, otherwise pause it.
    m_wideGraph->setPaused(!state);

    if (state) {
        if (!m_monitoring)
            Q_EMIT resumeAudioInputStream();
    } else {
        Q_EMIT suspendAudioInputStream();
    }
    m_monitoring = state;

    // Notify WSJT-X protocol clients of receive state change (Monitor on/off)
    statusUpdate();
}

void UI_Constructor::on_actionAbout_triggered() // Display "About"
{
    CAboutDlg{this}.exec();
}

void UI_Constructor::on_monitorButton_toggled(bool) {
    resetPushButtonToggleText(ui->monitorButton);
}

void UI_Constructor::on_monitorTxButton_toggled(bool checked) {
    resetPushButtonToggleText(ui->monitorTxButton);

    if (!checked) {
        qCDebug(mainwindow_js8)
            << "on_monitorTxButton_toggled(" << checked << ") to stop TX.";
        on_stopTxButton_clicked();
    }
}

void UI_Constructor::on_tuneButton_toggled(bool) {
    resetPushButtonToggleText(ui->tuneButton);
}

void UI_Constructor::on_spotButton_toggled(bool) {
    resetPushButtonToggleText(ui->spotButton);
}

void UI_Constructor::auto_tx_mode(bool state) {
    qCDebug(mainwindow_js8) << "auto_tx_mode(" << state << ")";
    m_auto = state;
    statusUpdate();
    if (state) {
        // Let us not wait until the next polling slot, but prepare transmission
        // now, even though that may waste a few CPU cycles through double work
        // that will be done soon anyway:
        prepareSending(DriftingDateTime::currentMSecsSinceEpoch());
    } else {
        // This function is called recursively from on_stopTxButton_clicked()!
        bool previous_stopTxButtonisLongterm = m_stopTxButtonIsLongterm;
        m_stopTxButtonIsLongterm = false;
        on_stopTxButton_clicked();
        m_stopTxButtonIsLongterm = previous_stopTxButtonisLongterm;
    }
    qCDebug(mainwindow_js8) << "auto_tx_mode(" << state << ") completed.";
}

void UI_Constructor::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Escape:
        on_stopTxButton_clicked();
        stopTx();
        return;
    case Qt::Key_F5:
        on_logQSOButton_clicked();
        return;
    }

    QMainWindow::keyPressEvent(e);
}

void UI_Constructor::f11f12(int const n) {
    if (n == 11)
        setFreq(freq() - 1);
    if (n == 12)
        setFreq(freq() + 1);
}

Radio::Frequency UI_Constructor::dialFrequency() {
    return Frequency{m_rigState.ptt() && m_rigState.split()
                         ? m_rigState.tx_frequency()
                         : m_rigState.frequency()};
}

void UI_Constructor::setSubmode(int submode) {
    m_nSubMode = submode;
    ui->actionModeJS8Normal->setChecked(submode == Varicode::JS8CallNormal);
    ui->actionModeJS8Fast->setChecked(submode == Varicode::JS8CallFast);
    ui->actionModeJS8Turbo->setChecked(submode == Varicode::JS8CallTurbo);
    ui->actionModeJS8Slow->setChecked(submode == Varicode::JS8CallSlow);
    ui->actionModeJS8Ultra->setChecked(submode == Varicode::JS8CallUltra);
    setupJS8();
    Q_EMIT submodeChanged(Varicode::intToSubmode(submode));
}

void UI_Constructor::updateCurrentBand() {
    QVariant state = ui->readFreq->property("state");
    if (!state.isValid()) {
        return;
    }

    auto dial_frequency = dialFrequency();
    auto const &band_name = m_config.bands()->find(dial_frequency);

    if (m_lastBand == band_name) {
        return;
    }

    cacheActivity(m_lastBand);

    // clear activity on startup if asked or on when the previous band is not
    // empty
    if (m_config.reset_activity() || !m_lastBand.isEmpty()) {
        clearActivity();
    }

    m_wideGraph->setBand(band_name);

    qCDebug(mainwindow_js8) << "setting band" << band_name;

    /**
     * @brief Send WSJT-X Status message on band change
     *
     * When the band changes, send a status update to WSJT-X protocol clients
     * and native JSON API clients (if not conflicting).
     */
    // Send WSJT-X Status message if protocol is enabled (band change triggers
    // status update)
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
            m_decoderBusy || m_monitoring, // decoding
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
            {{"_ID", QVariant(-1)},
             {"BAND", QVariant(band_name)},
             {"FREQ", QVariant((quint64)dialFrequency() + freq())},
             {"DIAL", QVariant((quint64)dialFrequency())},
             {"OFFSET", QVariant((quint64)freq())}});
    }
    m_lastBand = band_name;

    band_changed();
    restoreActivity(m_lastBand);
}

void UI_Constructor::displayDialFrequency() {
#if 0
    qCDebug(mainwindow_js8) << "rx nominal" << m_freqNominal;
    qCDebug(mainwindow_js8) << "tx nominal" << m_freqTxNominal;
    qCDebug(mainwindow_js8) << "offset set to" << freq() << freq();
#endif

    auto dial_frequency = dialFrequency();
    auto audio_frequency = freq();

    // lookup band
    auto const &band_name = m_config.bands()->find(dial_frequency);

    auto sFreq = Radio::pretty_frequency_MHz_string(dial_frequency);
    ui->currentFreq->setText(sFreq);

    if (m_splitMode && m_transmitting) {
        audio_frequency += m_XIT;
    }

    freqOffsetWidget->setValue(audio_frequency);

    auto const onAir = dial_frequency + audio_frequency;
        frequency_label.setText(QString("Freq: %1").arg(Radio::pretty_frequency_MHz_string(onAir)));
}

void UI_Constructor::statusChanged() { statusUpdate(); }

bool UI_Constructor::eventFilter(QObject *object, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
        // fall through
    case QEvent::MouseButtonPress:
        // reset the Tx watchdog
        resetIdleTimer();
        tx_watchdog(false);
        break;

    case QEvent::ChildAdded:
        // ensure our child widgets get added to our event filter
        add_child_to_event_filter(static_cast<QChildEvent *>(event)->child());
        break;

    case QEvent::ChildRemoved:
        // ensure our child widgets get d=removed from our event filter
        remove_child_from_event_filter(
            static_cast<QChildEvent *>(event)->child());
        break;

    case QEvent::ToolTip:
        if (!ui->actionShow_Tooltips->isChecked()) {
            return true;
        }

        break;

    default:
        break;
    }
    return QObject::eventFilter(object, event);
}

void UI_Constructor::createStatusBar() // createStatusBar
{
    tx_status_label.setAlignment(Qt::AlignCenter);
    tx_status_label.setMinimumSize(QSize{150, 18});
    tx_status_label.setStyleSheet(txStatusLabelStyle(TxStatusAppearance::Receiving));
    statusBar()->addWidget(&tx_status_label);

    last_tx_label.setAlignment(Qt::AlignCenter);
    last_tx_label.setMinimumSize(QSize{150, 18});
    last_tx_label.setStyleSheet(statusLabelStyle());
    statusBar()->addWidget(&last_tx_label);

    config_label.setAlignment(Qt::AlignCenter);
    config_label.setMinimumSize(QSize{80, 18});
    config_label.setStyleSheet(statusLabelStyle());
    statusBar()->addWidget(&config_label);
    config_label.hide(); // only shown for non-default configuration

    mode_label.setAlignment(Qt::AlignCenter);
    mode_label.setMinimumSize(QSize{80, 18});
    mode_label.setStyleSheet(statusLabelStyle());

    {
        QString modeLabelText;
        switch (m_nSubMode) {
        case Varicode::JS8CallSlow:
            modeLabelText = "JS8 Slow";
            break;
        case Varicode::JS8CallNormal:
            modeLabelText = "JS8 Normal";
            break;
        case Varicode::JS8CallFast:
            modeLabelText = "JS8 Fast";
            break;
        case Varicode::JS8CallTurbo:
            modeLabelText = "JS8 40";
            break;
        case Varicode::JS8CallUltra:
            modeLabelText = "JS8 60";
            break;
        default:
            modeLabelText = "JS8";
            break;
        }
        mode_label.setText(modeLabelText);
    }
    statusBar()->addWidget(&mode_label);

    frequency_label.setAlignment(Qt::AlignCenter);
    frequency_label.setMinimumSize(QSize{110, 18});
    frequency_label.setStyleSheet(statusLabelStyle());
    frequency_label.setText(QString("Freq: %1").arg(Radio::pretty_frequency_MHz_string(dialFrequency() + freq())));
    statusBar()->addWidget(&frequency_label);
    
    auto_reply_label.setAlignment(Qt::AlignCenter);
    auto_reply_label.setMinimumSize(QSize{110, 18});
    auto_reply_label.setStyleSheet(statusLabelStyle());
    QString autoReplyState = ui->actionModeAutoreply->isChecked() ? "On" : "Off";
    auto_reply_label.setText(QString("Auto Reply: %1").arg(autoReplyState));
    statusBar()->addWidget(&auto_reply_label);

    statusBar()->addPermanentWidget(&progressBar);
    progressBar.setMinimumSize(QSize{100, 18});
    const bool small = true;
    progressBar.setStyleSheet(progress_bar_stylesheet(small));
    progressBar.setFormat("%v/%m");

    statusBar()->addPermanentWidget(&wpm_label);
    wpm_label.setMinimumSize(QSize{120, 18});
    wpm_label.setStyleSheet(statusLabelStyle());
    wpm_label.setAlignment(Qt::AlignCenter);
}

void UI_Constructor::setup_status_bar() { last_tx_label.clear(); }

void UI_Constructor::closeEvent(QCloseEvent *e) {
        if (canSendNetworkMessage()) {
        sendNetworkMessage("STATION.CLOSING", "",
            {{"_ID", QVariant(-1)},
             {"REASON", QVariant("User closed application")}});
    }
    m_valid = false; // suppresses subprocess errors
    m_config.transceiver_offline();
    writeSettings();
    m_guiTimer.stop();
    m_prefixes.reset();
    m_shortcuts.reset();
    m_mouseCmnds.reset();
    Q_EMIT finished();

    QMainWindow::closeEvent(e);
}

void UI_Constructor::on_dialFreqUpButton_clicked() {
    setRig(m_freqNominal + 250);
}

void UI_Constructor::on_dialFreqDownButton_clicked() {
    setRig(m_freqNominal - 250);
}

void UI_Constructor::on_actionAdd_Log_Entry_triggered() {
    on_logQSOButton_clicked();
}

void UI_Constructor::on_actionCopyright_Notice_triggered() {
    auto const &message = tr(
        "If you make fair use of any part of this program under terms of the "
        "GNU "
        "General Public License, you must display the following copyright "
        "notice prominently in your derivative work:\n\n"
        "\"The algorithms, source code, look-and-feel of WSJT-X and related "
        "programs, and protocol specifications for the modes FSK441, FT8, JT4, "
        "JT6M, JT9, JT65, JTMS, QRA64, ISCAT, MSK144 are Copyright (C) "
        "2001-2018 by one or more of the following authors: Joseph Taylor, "
        "K1JT; Bill Somerville, G4WJS; Steven Franke, K9AN; Nico Palermo, "
        "IV3NWV; Greg Beam, KI7MT; Michael Black, W9MDB; Edson Pereira, "
        "PY2SDR; "
        "Philip Karn, KA9Q; and other members of the WSJT Development "
        "Group.\n\n"
        "Further, the source code of JS8Call contains material Copyright (C) "
        "2018-2019 by Jordan Sherer, KN4CRD.\"");
    JS8MessageBox::warning_message(this, message);
}

/**
 * @brief UI_Constructor::isDecodeReady
 *        determine if decoding is ready for a given submode
 * @param submode - submode to test
 * @param k - current frame count
 * @param k0 - previous frame count
 * @param pCurrentDecodeStart - input pointer to a static integer with the
 * current decode start position
 * @param pNextDecodeStart - input pointer to a static integer with the next
 * decode start position
 * @param pStart - output pointer to the next start position when decode is
 * ready
 * @param pSz - output pointer to the next size when decode is ready
 * @param pCycle - output pointer to the next cycle when decode is ready
 * @return true if decode is ready for this submode, false otherwise
 */
bool UI_Constructor::isDecodeReady(int const submode, qint32 const k,
                                   qint32 const k0, qint32 *pCurrentDecodeStart,
                                   qint32 *pNextDecodeStart, qint32 *pStart,
                                   qint32 *pSz, qint32 *pCycle) {
    if (pCurrentDecodeStart == nullptr || pNextDecodeStart == nullptr) {
        return false;
    }

    qint32 const cycleFrames = JS8::Submode::samplesPerPeriod(submode);
    qint32 const framesNeeded = JS8::Submode::samplesNeeded(submode);
    qint32 const currentCycle = JS8::Submode::computeCycleForDecode(submode, k);
    qint32 const delta = qAbs(k - k0);

    if (delta > cycleFrames) {
        qCDebug(decoder_js8) << "-->" << JS8::Submode::name(submode)
                             << "buffer advance delta" << delta;
    }

    // say, current decode start is 360000 and the next is 540000 (right before
    // we loop) frames needed are 150000 and then we turn off rx until k is
    // 110000 and the cycle frames are 180000 and k0 is a proper 100000 we need
    // to still reset... so, if k is less than the last decode start - cycle
    // frames (in this case 360000-180000, or 180000), then we should reset.
    // but, what if k is now 182000??

    // k=182000
    // k < current (182000 < 360000) true
    // k < max(0, current-cycleframes+framesNeeded) (k < 180000+150000) true

    // k=6000
    // k < current (6000<360000) true
    // k < max(0, 360000-180000+150000) true

    // k=350000
    // k < current (350000<360000) true
    // k < max(0, 360000-350000+150000) false

    // are we in the space between the end of the last decode and the start of
    // the next decode?
    bool const deadAir =
        (k < *pCurrentDecodeStart &&
         k < qMax(0, *pCurrentDecodeStart - cycleFrames + framesNeeded));

    // on buffer loop or init, prepare proper next decode start
    if ((deadAir) || (k < k0) || (delta > cycleFrames) ||
        (*pCurrentDecodeStart == -1) || (*pNextDecodeStart == -1)) {
        *pCurrentDecodeStart = currentCycle * cycleFrames;
        *pNextDecodeStart = *pCurrentDecodeStart + cycleFrames;
    }

    bool const ready = *pCurrentDecodeStart + framesNeeded <= k;

    if (ready) {
        qCDebug(decoder_js8)
            << "-->" << JS8::Submode::name(submode) << "from"
            << *pCurrentDecodeStart << "to"
            << *pCurrentDecodeStart + framesNeeded << "k" << k << "k0" << k0;

        if (pCycle)
            *pCycle = currentCycle;
        if (pStart)
            *pStart = *pCurrentDecodeStart;
        if (pSz)
            *pSz = qMax(framesNeeded, k - (*pCurrentDecodeStart));

        *pCurrentDecodeStart = *pNextDecodeStart;
        *pNextDecodeStart = *pCurrentDecodeStart + cycleFrames;
    }

    return ready;
}

/**
 * @brief UI_Constructor::decode
 *        try decoding
 * @return true if the decoder was activated, false otherwise
 */
bool UI_Constructor::decode(qint32 k) {
    static int k0 = 9999999;
    int kZero = k0;
    k0 = k;
    qCDebug(decoder_js8)
        << "decoder checking if ready..."
        << "k" << k << "k0" << kZero << "busy?" << m_decoderBusy
        << "lock exists?"
        << (QFile{m_config.temp_dir().absoluteFilePath(".lock")}.exists());

    if (k == kZero) {
        qCDebug(decoder_js8) << "--> decoder stream has not advanced";
        return false;
    }

    if (!m_monitoring) {
        qCDebug(decoder_js8) << "--> decoder stream is not active";
        return false;
    }

    bool ready = false;

#if JS8_USE_EXPERIMENTAL_DECODE_TIMING
    ready = decodeEnqueueReady(k, kZero);
    if (ready || !m_decoderQueue.isEmpty()) {
        qCDebug(decoder_js8) << "--> decoder is ready to be run with"
                             << m_decoderQueue.count() << "decode periods";
    }
#else
    ready = decodeEnqueueReadyExperiment(k, kZero);
    if (ready || !m_decoderQueue.isEmpty()) {
        qCDebug(decoder_js8) << "--> decoder is ready to be run with"
                             << m_decoderQueue.count() << "decode periods";
    }
#endif

    //
    // TODO: what follows can likely be pulled out to an async process
    //

    // pause decoder if we are currently transmitting
    if (m_transmitting) {
        // We used to use isMessageQueuedForTransmit, and some form of checking
        // for queued messages but, that just caused problems with missing
        // decodes, so we only pause if we are actually actively transmitting.
        qCDebug(decoder_js8) << "--> decoder paused during transmit";
        return false;
    }

    if (m_decoderBusyStartTime.isValid() &&
        m_decoderBusyStartTime.msecsTo(QDateTime::currentDateTimeUtc()) <
            1000) {
        qCDebug(decoder_js8)
            << "--> decoder paused for 1000 ms after last decode start";
        return false;
    }

    int threshold =
        m_nSubMode == Varicode::JS8CallSlow ? 4000 : 2000; // two seconds
    if (isInDecodeDelayThreshold(threshold)) {
        qCDebug(decoder_js8) << "--> decoder paused for" << threshold
                             << "ms after transmit stop";
        return false;
    }

    // critical section (modifying dec_data)

    qint32 submode = -1;
    if (!decodeProcessQueue(&submode)) {
        return false;
    }

    decodeStart();

    return true;
}

/**
 * @brief UI_Constructor::decodeEnqueueReady
 *        compute the available decoder ranges that can be processed and
 *        place them in the decode queue
 * @param k - the current frame count
 * @param k0 - the previous frame count
 * @return true if decoder ranges were queued, false otherwise
 */
bool UI_Constructor::decodeEnqueueReady(qint32 k, qint32 k0) {
    // compute the next decode for each submode
    // enqueue those decodes that are "ready"
    // on an interval, issue a decode
    int decodes = 0;

    bool couldDecodeA = false;
    qint32 startA = -1;
    qint32 szA = -1;
    qint32 cycleA = -1;

    bool couldDecodeB = false;
    qint32 startB = -1;
    qint32 szB = -1;
    qint32 cycleB = -1;

    bool couldDecodeC = false;
    qint32 startC = -1;
    qint32 szC = -1;
    qint32 cycleC = -1;

    bool couldDecodeE = false;
    qint32 startE = -1;
    qint32 szE = -1;
    qint32 cycleE = -1;

#if JS8_ENABLE_JS8I
    bool couldDecodeI = false;
    qint32 startI = -1;
    qint32 szI = -1;
    qint32 cycleI = -1;
#endif

    static qint32 currentDecodeStartA = -1;
    static qint32 nextDecodeStartA = -1;
    qCDebug(decoder_js8) << "? NORMAL   " << currentDecodeStartA
                         << nextDecodeStartA;
    couldDecodeA =
        isDecodeReady(Varicode::JS8CallNormal, k, k0, &currentDecodeStartA,
                      &nextDecodeStartA, &startA, &szA, &cycleA);

    static qint32 currentDecodeStartB = -1;
    static qint32 nextDecodeStartB = -1;
    qCDebug(decoder_js8) << "? FAST     " << currentDecodeStartB
                         << nextDecodeStartB;
    couldDecodeB =
        isDecodeReady(Varicode::JS8CallFast, k, k0, &currentDecodeStartB,
                      &nextDecodeStartB, &startB, &szB, &cycleB);

    static qint32 currentDecodeStartC = -1;
    static qint32 nextDecodeStartC = -1;
    qCDebug(decoder_js8) << "? TURBO    " << currentDecodeStartC
                         << nextDecodeStartC;
    couldDecodeC =
        isDecodeReady(Varicode::JS8CallTurbo, k, k0, &currentDecodeStartC,
                      &nextDecodeStartC, &startC, &szC, &cycleC);

    static qint32 currentDecodeStartE = -1;
    static qint32 nextDecodeStartE = -1;
    qCDebug(decoder_js8) << "? SLOW     " << currentDecodeStartE
                         << nextDecodeStartE;
    couldDecodeE =
        isDecodeReady(Varicode::JS8CallSlow, k, k0, &currentDecodeStartE,
                      &nextDecodeStartE, &startE, &szE, &cycleE);

#if JS8_ENABLE_JS8I
    static qint32 currentDecodeStartI = -1;
    static qint32 nextDecodeStartI = -1;
    qCDebug(decoder_js8) << "? JS8 60    " << currentDecodeStartI
                         << nextDecodeStartI;
    couldDecodeI =
        isDecodeReady(Varicode::JS8CallUltra, k, k0, &currentDecodeStartI,
                      &nextDecodeStartI, &startI, &szI, &cycleI);
#endif

    if (couldDecodeA) {
        DecodeParams d;
        d.submode = Varicode::JS8CallNormal;
        d.start = startA;
        d.sz = szA;
        m_decoderQueue.append(d);
        decodes++;
    }

    if (couldDecodeB) {
        DecodeParams d;
        d.submode = Varicode::JS8CallFast;
        d.start = startB;
        d.sz = szB;
        m_decoderQueue.append(d);
        decodes++;
    }

    if (couldDecodeC) {
        DecodeParams d;
        d.submode = Varicode::JS8CallTurbo;
        d.start = startC;
        d.sz = szC;
        m_decoderQueue.append(d);
        decodes++;
    }

    if (couldDecodeE) {
        DecodeParams d;
        d.submode = Varicode::JS8CallSlow;
        d.start = startE;
        d.sz = szE;
        m_decoderQueue.append(d);
        decodes++;
    }

#if JS8_ENABLE_JS8I
    if (couldDecodeI) {
        DecodeParams d;
        d.submode = Varicode::JS8CallUltra;
        d.start = startI;
        d.sz = szI;
        m_decoderQueue.append(d);
        decodes++;
    }
#endif

    return decodes > 0;
}

/**
 * @brief UI_Constructor::decodeEnqueueReadyExperiment
 *        compute the available decoder ranges that can be processed and
 *        place them in the decode queue
 *
 *        experiment with decoding on a much shorter interval than usual
 *
 * @param k - the current frame count
 * @param k0 - the previous frame count
 * @return true if decoder ranges were queued, false otherwise
 */
bool UI_Constructor::decodeEnqueueReadyExperiment(qint32 k, qint32 /*k0*/) {
    // TODO: make this non-static field of UI_Constructor?
    // map of last decode positions for each submode
    // static QMap<qint32, qint32> m_lastDecodeStartMap;

    // TODO: make this non-static field of UI_Constructor?
    // map of submodes to decode + optional alternate decode positions
    static QMap<qint32, QList<qint32>> submodes = {
        {Varicode::JS8CallSlow, {0}},
        {Varicode::JS8CallNormal, {0}},
        {Varicode::JS8CallFast, {0}},  // NORMAL: 0, 10, 20    --- ALT: 15, 25
        {Varicode::JS8CallTurbo, {0}}, // NORMAL: 0, 6, 12, 18 --- ALT: 15, 21,
                                       // 27
#if JS8_ENABLE_JS8I
        {Varicode::JS8CallUltra, {0}},
#endif
    };

    static qint32 maxSamples = JS8_RX_SAMPLE_SIZE;
    static qint32 oneSecondSamples = JS8_RX_SAMPLE_RATE;

    int decodes = 0;

    // do we have a better way to check this?
    bool multi = ui->actionModeMultiDecoder->isChecked();

    // do we need to process alternate positions?
    bool skipAlt = true;

    foreach (auto submode, submodes.keys()) {
        // do we have a better way to check this?
        bool everySecond = m_wideGraph->shouldAutoSyncSubmode(submode);

        // skip if multi is disabled and this mode is not the current submode
        // and we're not autosyncing this mode
        if (!everySecond && !multi && submode != m_nSubMode) {
            continue;
        }

        // check all alternate decode positions
        foreach (auto alt, submodes.value(submode)) {
            // skip alt decode positions if needed
            if (skipAlt && alt != 0) {
                continue;
            }

            // skip alts if we are decoding every second
            if (everySecond && alt != 0) {
                continue;
            }

            qint32 const cycle = JS8::Submode::computeAltCycleForDecode(
                submode, k, alt * oneSecondSamples);
            qint32 const cycleFrames = JS8::Submode::samplesPerPeriod(submode);
            qint32 const cycleFramesNeeded =
                (submode == Varicode::JS8CallTurbo ||
                 submode == Varicode::JS8CallUltra)
                    ? JS8::Submode::samplesNeeded(submode)
                    : JS8::Submode::samplesForSymbols(submode);
            bool const turboOrUltra =
                (submode == Varicode::JS8CallTurbo ||
                 submode == Varicode::JS8CallUltra);
            qint32 cycleFramesReady = k - (cycle * cycleFrames);
            if (cycleFramesReady < 0) {
                cycleFramesReady = k + (maxSamples - (cycle * cycleFrames));
            }

            if (!m_lastDecodeStartMap.contains(submode)) {
                m_lastDecodeStartMap[submode] = cycle * cycleFrames;
            }

            qint32 lastDecodeStart = m_lastDecodeStartMap[submode];
            qint32 incrementedBy = k - lastDecodeStart;
            if (k < lastDecodeStart) {
                incrementedBy = maxSamples - lastDecodeStart + k;
            }
            qCDebug(decoder_js8)
                << JS8::Submode::name(submode) << "alt" << alt << "cycle"
                << cycle << "cycle frames" << cycleFrames << "cycle start"
                << cycle * cycleFrames << "cycle end"
                << (cycle + 1) * cycleFrames << "k" << k << "frames ready"
                << cycleFramesReady << "incremeted by" << incrementedBy;

            if (everySecond && incrementedBy >= oneSecondSamples) {
                DecodeParams d;
                d.submode = submode;
                d.sz = cycleFrames;
                d.start = k - d.sz;
                if (d.start < 0) {
                    d.start += maxSamples;
                }
                m_decoderQueue.append(d);
                decodes++;

                // keep track of last decode position
                m_lastDecodeStartMap[submode] = k;
            } else if (turboOrUltra &&
                       cycleFramesReady >= cycleFramesNeeded) {
                qint32 const cycleStart = cycle * cycleFrames;
                if (m_lastDecodeCycleMap.value(submode, -1) != cycleStart) {
                    DecodeParams d;
                    d.submode = submode;
                    d.start = cycleStart;
                    d.sz = cycleFramesNeeded;
                    m_decoderQueue.append(d);
                    decodes++;

                    // keep track of last decode position and cycle
                    m_lastDecodeStartMap[submode] = k;
                    m_lastDecodeCycleMap[submode] = cycleStart;
                }
            } else if ((incrementedBy >= 1.5 * oneSecondSamples &&
                        cycleFramesReady >=
                            cycleFramesNeeded) || // within every 3/2 seconds
                                                  // for normal positions
                       (incrementedBy >= oneSecondSamples &&
                        cycleFramesReady >=
                            cycleFramesNeeded -
                                1.5 * oneSecondSamples) || // within the last
                                                           // 3/2 seconds of a
                                                           // new cycle
                       (incrementedBy >= oneSecondSamples &&
                        cycleFramesReady <
                            1.5 * oneSecondSamples) // within the first 3/2
                                                    // seconds of a new cycle
            ) {
                DecodeParams d;
                d.submode = submode;
                d.start = cycle * cycleFrames;
                d.sz = cycleFramesReady;
                m_decoderQueue.append(d);
                decodes++;

                // keep track of last decode position
                m_lastDecodeStartMap[submode] = k;
            }
        }
    }

    return decodes > 0;
}

/**
 * @brief UI_Constructor::decodeProcessQueue
 *        process the decode queue by merging available decode ranges
 *        into the dec_data shared structure for the decoder to process
 * @param pSubmode - the lowest speed submode in this iteration
 * @return true if the decoder is ready to be run, false otherwise
 */
bool UI_Constructor::decodeProcessQueue(qint32 *pSubmode) {
    // critical section
    QMutexLocker mutex(m_detector->getMutex());

    if (m_decoderBusy) {
        int seconds =
            m_decoderBusyStartTime.secsTo(QDateTime::currentDateTimeUtc());
        if (seconds > 60) {
            qCDebug(decoder_js8) << "--> decoder should be killed!"
                                 << QString("(%1 seconds)").arg(seconds);
        } else if (seconds > 30) {
            qCDebug(decoder_js8) << "--> decoder is hanging!"
                                 << QString("(%1 seconds)").arg(seconds);
        } else {
            qCDebug(decoder_js8) << "--> decoder is busy!";
        }

        return false;
    }

    if (m_decoderQueue.isEmpty()) {
        qCDebug(decoder_js8) << "--> decoder has nothing to process!";
        return false;
    }

    int submode = -1;
    int maxDecodes = 1;

    bool multi = ui->actionModeMultiDecoder->isChecked();
    if (multi) {
        maxDecodes = JS8_ENABLE_JS8I ? 5 : 4;
    }

    int count = m_decoderQueue.count();
    if (count > maxDecodes) {
        qCDebug(decoder_js8) << "--> decoder skipping at least 1 decode cycle"
                             << "count" << count << "max" << maxDecodes;
    }

    // default to no submodes being decoded, then bitwise OR the modes together
    // to decode them all at once
    dec_data.params.nsubmodes = 0;

    while (!m_decoderQueue.isEmpty()) {
        auto params = m_decoderQueue.front();
        m_decoderQueue.removeFirst();

        // skip if we are not in multi mode and the submode doesn't equal the
        // global submode
        if (!multi && params.submode != m_nSubMode) {
            continue;
        }

        if (submode == -1 || params.submode < submode) {
            submode = params.submode;
        }

        switch (params.submode) {
        case Varicode::JS8CallNormal:
            dec_data.params.kposA = params.start;
            dec_data.params.kszA = params.sz;
            dec_data.params.nsubmodes |= (params.submode + 1);
            break;
        case Varicode::JS8CallFast:
            dec_data.params.kposB = params.start;
            dec_data.params.kszB = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
        case Varicode::JS8CallTurbo:
            dec_data.params.kposC = params.start;
            dec_data.params.kszC = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
        case Varicode::JS8CallSlow:
            dec_data.params.kposE = params.start;
            dec_data.params.kszE = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
#if JS8_ENABLE_JS8I
        case Varicode::JS8CallUltra:
            dec_data.params.kposI = params.start;
            dec_data.params.kszI = params.sz;
            dec_data.params.nsubmodes |= (params.submode << 1);
            break;
#endif
        }
    }

    if (submode == -1) {
        qCDebug(decoder_js8) << "--> decoder has no segments to decode!";
        return false;
    }

    dec_data.params.syncStats = (m_wideGraph->shouldDisplayDecodeAttempts() ||
                                 m_wideGraph->isAutoSyncEnabled());
    dec_data.params.newdat = 1;

    auto const period_unsigned = JS8::Submode::period(submode);
    // Need to use a signed integer here,
    auto const period_signed = (int)period_unsigned;
    // as (2 - period_unsigned) results in an enourmeous number close to 2**32.
    auto const t =
        DriftingDateTime::currentDateTimeUtc().addSecs(2 - period_signed);
    auto const ihr = t.toString("hh").toInt();
    auto const imin = t.toString("mm").toInt();
    auto const isec = t.toString("ss").toInt();

    dec_data.params.nutc = code_time(ihr, imin, isec - isec % period_unsigned);
    dec_data.params.nfqso = freq();
    dec_data.params.nfa =
        m_wideGraph->filterEnabled() ? m_wideGraph->filterMinimum() : 0;
    dec_data.params.nfb =
        m_wideGraph->filterEnabled() ? m_wideGraph->filterMaximum() : 5000;

    if (dec_data.params.nutc < m_nutc0)
        m_RxLog = 1; // Date and Time to ALL.TXT
    if (dec_data.params.newdat == 1)
        m_nutc0 = dec_data.params.nutc;

    // keep track of the minimum submode
    if (pSubmode)
        *pSubmode = submode;

    return true;
}

/**
 * @brief UI_Constructor::decodeStart
 *        copy the dec_data structure to shared memory and
 *        remove the lock file to start the decoding process
 */
void UI_Constructor::decodeStart() {
    // critical section
    QMutexLocker mutex(m_detector->getMutex());

    if (m_decoderBusy) {
        qCDebug(decoder_js8) << "--> decoder cannot start...busy (busy flag)";
        return;
    }

    // Mark the decoder busy; decodeDone is responsible for marking
    // the decode _not_ busy

    decodeBusy(true);
    qCDebug(decoder_js8) << "--> decoder starting"
                         << " --> kin:" << dec_data.params.kin
                         << " --> newdat:" << dec_data.params.newdat
                         << " --> nsubmodes:" << dec_data.params.nsubmodes
                         << " --> A:" << dec_data.params.kposA
                         << dec_data.params.kposA + dec_data.params.kszA
                         << QString("(%1)").arg(dec_data.params.kszA)
                         << " --> B:" << dec_data.params.kposB
                         << dec_data.params.kposB + dec_data.params.kszB
                         << QString("(%1)").arg(dec_data.params.kszB)
                         << " --> C:" << dec_data.params.kposC
                         << dec_data.params.kposC + dec_data.params.kszC
                         << QString("(%1)").arg(dec_data.params.kszC)
                         << " --> E:" << dec_data.params.kposE
                         << dec_data.params.kposE + dec_data.params.kszE
                         << QString("(%1)").arg(dec_data.params.kszE)
                         << " --> I:" << dec_data.params.kposI
                         << dec_data.params.kposI + dec_data.params.kszI
                         << QString("(%1)").arg(dec_data.params.kszI);

    m_decoder.decode();
}

/**
 * @brief UI_Constructor::decodeBusy
 *        mark the decoder as currently busy (to prevent overlapping decodes)
 * @param b - true if busy, false otherwise
 */
void UI_Constructor::decodeBusy(bool b) // decodeBusy()
{
    m_decoderBusy = b;

    if (m_decoderBusy) {
        tx_status_label.setText("Decoding");

        m_decoderBusyStartTime = QDateTime::
            currentDateTimeUtc(); // DriftingDateTime::currentDateTimeUtc();
        m_decoderBusyFreq = dialFrequency();
        m_decoderBusyBand = m_config.bands()->find(m_decoderBusyFreq);
    }

    // Notify WSJT-X protocol clients of decode state change (start/end of decode round)
    statusUpdate();
}

/**
 * @brief UI_Constructor::decodeDone
 *        clean up after a decode is finished
 */
void UI_Constructor::decodeDone() {
    // critical section
    QMutexLocker mutex(m_detector->getMutex());

    dec_data.params.newdat = false;
    m_RxLog = 0;

    // cleanup old cached messages (messages > submode period old)

    std::erase_if(m_messageDupeCache, [](auto const &it) {
        return it.second.secsTo(QDateTime::currentDateTimeUtc()) >
               JS8::Submode::period(it.first.submode);
    });

    decodeBusy(false);
}

QDateTime UI_Constructor::nextTransmitCycle() {
    auto timestamp = DriftingDateTime::currentDateTimeUtc();

    // remove milliseconds
    auto t = timestamp.time();
    t.setHMS(t.hour(), t.minute(), t.second());
    timestamp.setTime(t);

    // round to 15 second increment
    int secondsSinceEpoch = (timestamp.toMSecsSinceEpoch() / 1000);
    int delta = roundUp(secondsSinceEpoch, m_TRperiod) + 1 - secondsSinceEpoch;
    timestamp = timestamp.addSecs(delta);

    return timestamp;
}

bool UI_Constructor::hasExistingMessageBufferToMe(int *const pOffset) {
    for (auto const [offset, buffer] : m_messageBuffer.asKeyValueRange()) {
        // if this is a valid buffer and it's to me...
        if (buffer.cmd.utcTimestamp.isValid() &&
            (buffer.cmd.to == m_config.my_callsign() ||
             buffer.cmd.to == Radio::base_callsign(m_config.my_callsign()))) {
            if (pOffset)
                *pOffset = offset;
            return true;
        }
    }

    return false;
}

bool UI_Constructor::hasExistingMessageBuffer(int submode, int offset,
                                              bool drift, int *pPrevOffset) {
    if (m_messageBuffer.contains(offset)) {
        if (pPrevOffset)
            *pPrevOffset = offset;
        return true;
    }

    int const range = JS8::Submode::rxThreshold(submode);

    QList<int> offsets = generateOffsets(offset - range, offset + range);

    foreach (int prevOffset, offsets) {
        if (!m_messageBuffer.contains(prevOffset)) {
            continue;
        }

        if (drift) {
            m_messageBuffer[offset] = m_messageBuffer[prevOffset];
            m_messageBuffer.remove(prevOffset);
        }

        if (pPrevOffset)
            *pPrevOffset = prevOffset;
        return true;
    }

    return false;
}

bool UI_Constructor::hasClosedExistingMessageBuffer(int offset) {
#if 0
    int range = 10;
    if(m_nSubMode == Varicode::JS8CallFast){ range = 16; }
    if(m_nSubMode == Varicode::JS8CallTurbo){ range = 32; }

    return offset - range <= m_lastClosedMessageBufferOffset && m_lastClosedMessageBufferOffset <= offset + range;
#elif 0
    int range = 10;
    if (m_nSubMode == Varicode::JS8CallFast) {
        range = 16;
    }
    if (m_nSubMode == Varicode::JS8CallTurbo) {
        range = 32;
    }

    return m_lastClosedMessageBufferOffset - range <= offset &&
           offset <= m_lastClosedMessageBufferOffset + range;
#else
    Q_UNUSED(offset);
#endif
    return false;
}

void UI_Constructor::logCallActivity(CallDetail d, bool spot) {
    // don't log empty calls
    if (d.call.trimmed().isEmpty()) {
        return;
    }

    // don't log relay calls
    if (d.call.contains(">")) {
        return;
    }

    if (m_callActivity.contains(d.call)) {
        // update (keep grid)
        CallDetail old = m_callActivity[d.call];
        if (d.grid.isEmpty() && !old.grid.isEmpty()) {
            d.grid = old.grid;
        }
        if (!d.ackTimestamp.isValid() && old.ackTimestamp.isValid()) {
            d.ackTimestamp = old.ackTimestamp;
        }
        if (!d.cqTimestamp.isValid() && old.cqTimestamp.isValid()) {
            d.cqTimestamp = old.cqTimestamp;
        }
        m_callActivity[d.call] = d;
    } else {
        // create
        m_callActivity[d.call] = d;

        // notification of old and new callsigns
        if (m_logBook.hasWorkedBefore(d.call, "")) {
            tryNotify("call_old");
        } else {
            tryNotify("call_new");
        }
    }

    // enqueue for spotting to psk reporter
    if (spot) {
        m_rxCallQueue.append(d);
    }
}

void UI_Constructor::logHeardGraph(QString from, QString to) {
    auto my_callsign = m_config.my_callsign();

    // hearing
    if (m_heardGraphOutgoing.contains(my_callsign)) {
        m_heardGraphOutgoing[my_callsign].insert(from);
    } else {
        m_heardGraphOutgoing[my_callsign].insert(from);
    }

    // heard by
    if (m_heardGraphIncoming.contains(from)) {
        m_heardGraphIncoming[from].insert(my_callsign);
    } else {
        m_heardGraphIncoming[from] = {my_callsign};
    }

    if (to == "@ALLCALL") {
        return;
    }

    // hearing
    if (m_heardGraphOutgoing.contains(from)) {
        m_heardGraphOutgoing[from].insert(to);
    } else {
        m_heardGraphOutgoing[from] = {to};
    }

    // heard by
    if (m_heardGraphIncoming.contains(to)) {
        m_heardGraphIncoming[to].insert(from);
    } else {
        m_heardGraphIncoming[to] = {from};
    }
}

QString UI_Constructor::lookupCallInCompoundCache(QString const &call) {
    QString myBaseCall = Radio::base_callsign(m_config.my_callsign());
    if (call == myBaseCall) {
        return m_config.my_callsign();
    }
    return m_compoundCallCache.value(call, call);
}

void UI_Constructor::spotReport(int const submode, int const dial,
                                int const offset, int const snr,
                                QString const &callsign, QString const &grid) {
    if (!m_config.spot_to_reporting_networks() ||
        (m_config.spot_blacklist().contains(callsign) ||
         m_config.spot_blacklist().contains(Radio::base_callsign(callsign))))
        return;

    Q_EMIT spotClientEnqueueSpot(callsign, grid, submode, dial, offset, snr);
}

void UI_Constructor::spotCmd(CommandDetail const &cmd) {
    if (!m_config.spot_to_reporting_networks() ||
        (m_config.spot_blacklist().contains(cmd.from) ||
         m_config.spot_blacklist().contains(Radio::base_callsign(cmd.from))))
        return;

    QString cmdStr = cmd.cmd;

    if (!cmdStr.trimmed().isEmpty()) {
        cmdStr = Varicode::lstrip(cmd.cmd);
    }

    Q_EMIT spotClientEnqueueCmd(cmdStr, cmd.from, cmd.to, cmd.relayPath,
                                cmd.text, cmd.grid, cmd.extra, cmd.submode,
                                cmd.dial, cmd.offset, cmd.snr);
}

// KN4CRD: @APRSIS CMD :EMAIL-2  :email@domain.com booya{1
void UI_Constructor::spotAprsCmd(CommandDetail const &cmd) {
    if (!m_config.spot_to_reporting_networks())
        return;
    if (!m_config.spot_to_aprs())
        return;
    if (m_config.spot_blacklist().contains(cmd.from) ||
        m_config.spot_blacklist().contains(Radio::base_callsign(cmd.from)))
        return;

    if (cmd.cmd != " CMD")
        return;

    qCDebug(mainwindow_js8)
        << "APRSISClient Enqueueing Third Party Text" << cmd.from << cmd.text;

    auto by_call = APRSISClient::replaceCallsignSuffixWithSSID(
        m_config.my_callsign(), Radio::base_callsign(m_config.my_callsign()));
    auto from_call = APRSISClient::replaceCallsignSuffixWithSSID(
        cmd.from, Radio::base_callsign(cmd.from));

    // we use a queued signal here so we can process these spots in a network
    // thread to prevent blocking the gui/decoder while waiting on TCP
    emit aprsClientEnqueueThirdParty(by_call, from_call, cmd.text);
}

void UI_Constructor::spotAprsGrid(int dial, int offset, int snr,
                                  QString callsign, QString grid) {
    if (!m_config.spot_to_reporting_networks())
        return;
    if (!m_config.spot_to_aprs())
        return;
    if (m_config.spot_blacklist().contains(callsign) ||
        m_config.spot_blacklist().contains(Radio::base_callsign(callsign)))
        return;
    if (grid.length() < 4)
        return;

    Frequency frequency = dial + offset;

    auto comment = QString("%1MHz %2dB")
                       .arg(Radio::frequency_MHz_string(frequency))
                       .arg(Varicode::formatSNR(snr));
    if (callsign.contains("/")) {
        comment = QString("%1 %2").arg(callsign).arg(comment);
    }

    auto by_call = APRSISClient::replaceCallsignSuffixWithSSID(
        m_config.my_callsign(), Radio::base_callsign(m_config.my_callsign()));
    auto from_call = APRSISClient::replaceCallsignSuffixWithSSID(
        callsign, Radio::base_callsign(callsign));

    // we use a queued signal here so we can process these spots in a network
    // thread to prevent blocking the gui/decoder while waiting on TCP
    emit aprsClientEnqueueSpot(by_call, from_call, grid, comment);
}

void UI_Constructor::pskLogReport(QString const &mode, int const dial,
                                  int const offset, int const snr,
                                  QString const &callsign, QString const &grid,
                                  QDateTime const &utcTimestamp) {
    if (!m_config.spot_to_reporting_networks() ||
        (m_config.spot_blacklist().contains(callsign) ||
         m_config.spot_blacklist().contains(Radio::base_callsign(callsign))))
        return;

    Q_EMIT pskReporterAddRemoteStation(callsign, grid, dial + offset, mode, snr,
                                       utcTimestamp);
}

void UI_Constructor::refuseToSendIn30mWSPRBand() {
    if (m_transmitting or m_auto or m_tune) {
        m_dateTimeLastTX = DriftingDateTime::currentDateTimeLocal();

        // Don't transmit another mode in the 30 m WSPR sub-band
        Frequency onAirFreq = m_freqNominal + freq();

        // qCDebug(mainwindow_js8) << "transmitting on" << onAirFreq;

        if (10139900 <= onAirFreq && onAirFreq <= 10140320) {
            qCWarning(mainwindow_js8)
                << "QRG" << onAirFreq
                << "found to be in WSPR guard band 10139.9 - 10140.32 kHz "
                   "where this programm will not transmit, so  canceling all "
                   "transmissions.";
            m_isTimeToSend = false;
            if (m_auto)
                auto_tx_mode(false);
            if (m_hb_loop->isActive()) {
                m_hb_loop->onLoopCancel();
            }
            if (m_cq_loop->isActive()) {
                m_cq_loop->onLoopCancel();
            }
            if (onAirFreq != m_onAirFreq0) {
                m_onAirFreq0 = onAirFreq;
                QTimer::singleShot(0, [this] {
                    JS8MessageBox::warning_message(
                        this, tr("WSPR Guard Band"),
                        tr("Please choose another Tx frequency."
                           " The app will not knowingly transmit another"
                           " mode in the WSPR sub-band on 30m."));
                });
            }
        }
    }
}

void UI_Constructor::prepareSending(qint64 nowMS) {
    // TX Duration in seconds.
    const double tx_duration = JS8::Submode::txDuration(m_nSubMode);
    const unsigned period = JS8::Submode::period(m_nSubMode);

    const double seconds_into_the_period = (nowMS % (period * 1000)) / 1000.0;
    const double tx_delay = m_TxDelay;

    const bool time_is_in_tx_delay =
        (period - tx_delay) <= seconds_into_the_period;

    // Are we during the time we might be sending?
    const bool m_timeToSend = ((0 <= seconds_into_the_period) and
                               (seconds_into_the_period < tx_duration)) or
                              time_is_in_tx_delay or m_tune;

    auto const msgLength = QStringView(m_nextFreeTextMsg).trimmed().length();

    // TODO: stop
    if (msgLength == 0 && !m_tune) {
        m_stopTxButtonIsLongterm = false;
        this->on_stopTxButton_clicked();
        m_stopTxButtonIsLongterm = true;
    }

    double const fraction_of_tx_slot = seconds_into_the_period / period;

    // 15.0 - 12.6
    double const ratio = JS8::Submode::computeRatio(m_nSubMode, m_TRperiod);

    // the late threshold is the dead air time minus the tx delay time
    float lateThreshold = ratio - (m_config.txDelay() / m_TRperiod);
    if (m_nSubMode == Varicode::JS8CallFast) {
        // for the faster mode, only allow 3/4 late threshold
        lateThreshold *= 0.75;
    } else if (m_nSubMode == Varicode::JS8CallTurbo ||
               m_nSubMode == Varicode::JS8CallUltra) {
        // for the JS8 40 (formerly "Turbo") and JS8 60
        // modes, only allow 1/2 late threshold
        lateThreshold *= 0.5;
    };

    // qCDebug(mainwindow_js8) << "nowMS" << nowMS << "period" << period <<
    // "tx_delay" << tx_delay
    //                         << "seconds_into_the_period" <<
    //                         seconds_into_the_period
    //                         << "time_is_in_tx_delay" << time_is_in_tx_delay
    //                         << "fraction_of_tx_slot" << fraction_of_tx_slot
    //                         << "m_iptt" << m_iptt << "m_timeToSend" <<
    //                         m_timeToSend << "msgLength" << msgLength <<
    //                         "m_tune" << m_tune;

    if (m_iptt == 0 &&
        ((m_timeToSend &&
          (fraction_of_tx_slot < lateThreshold or time_is_in_tx_delay) &&
          0 < msgLength) ||
         m_tune)) {
        // This signals the transmitter to switch to sending.
        // When that has happened, we get a callback from
        // handle_transceiver_update, which will start the audio.
        m_iptt = 1;
        m_generateAudioWhenPttConfirmedByTX = true;
        setRig();
        setXIT(freq());
        emitPTT(true);
    }

    // TODO: stop
    if (!m_timeToSend and !m_tune)
        m_btxok = false; // Time to stop transmitting

    // Calculate Tx tones when needed
    if ((m_iptt == 1 && m_iptt0 == 0) || m_restart) {
        //----------------------------------------------------------------------

        copyMessage(m_nextFreeTextMsg, message);

        if (m_lastMessageSent != m_currentMessage ||
            m_lastMessageType != m_currentMessageType) {
            m_lastMessageSent = m_currentMessage;
            m_lastMessageType = m_currentMessageType;
        }

        m_currentMessageType = 0;

        if (m_tune) {
            itone[0] = 0;
        } else {
            JS8::encode(m_i3bit,
                        JS8::Costas::array(JS8::Submode::costas(m_nSubMode)),
                        message,
                        const_cast<int *>(reinterpret_cast<volatile int *>(
                            itone))); // XXX ick...

            std::fill_n(std::begin(msgsent), 22, ' ');
            std::copy_n(std::begin(message), 12, std::begin(msgsent));

            if (mainwindow_js8().isDebugEnabled()) {
                qCDebug(mainwindow_js8) << "-> msg:" << message;
                qCDebug(mainwindow_js8) << "-> bit:" << m_i3bit;
                for (int i = 0; i < 7; ++i)
                    qCDebug(mainwindow_js8)
                        << "-> tone" << i << "=" << itone[i];
                for (int i = JS8_NUM_SYMBOLS - 7; i < JS8_NUM_SYMBOLS; ++i)
                    qCDebug(mainwindow_js8)
                        << "-> tone" << i << "=" << itone[i];
            }

            msgibits = m_i3bit;
            msgsent[22] = 0;
            m_currentMessage = QString::fromLatin1(msgsent).trimmed();
            m_currentMessageBits = msgibits;

            emitTones();
        }

        if (m_tune) {
            m_currentMessage = "TUNE";
            m_currentMessageType = -1;
        }
        if (m_restart) {
            write_transmit_entry("ALL.TXT");
        }

        auto msg_parts = m_currentMessage.split(' ', Qt::SkipEmptyParts);
        if (msg_parts.size() > 2) {
            // clean up short code forms
            msg_parts[0].remove(QChar{'<'});
            msg_parts[1].remove(QChar{'>'});
        }

        if ((m_currentMessageType < 6 || 7 == m_currentMessageType) &&
            msg_parts.length() >= 3 &&
            (msg_parts[1] == m_config.my_callsign() ||
             msg_parts[1] == m_baseCall)) {
            int i1;
            bool ok;
            i1 = msg_parts[2].toInt(&ok);
            if (ok and i1 >= -50 and i1 < 50) {
                m_rptSent = msg_parts[2];
            } else {
                if (msg_parts[2].mid(0, 1) == "R") {
                    i1 = msg_parts[2].mid(1).toInt(&ok);
                    if (ok and i1 >= -50 and i1 < 50) {
                        m_rptSent = msg_parts[2].mid(1);
                    }
                }
            }
        }
        m_restart = false;
        //----------------------------------------------------------------------
    }

    if (m_iptt == 1 && m_iptt0 == 0) {
        auto const &current_message = QString::fromLatin1(msgsent);
        if (m_config.watchdog() && current_message != m_msgSent0) {
            // new messages don't reset the idle timer :|
            // tx_watchdog (false);  // in case we are auto sequencing
            m_msgSent0 = current_message;
        }

        if (!m_tune) {
            write_transmit_entry("ALL.TXT");
        }

        // TODO: jsherer - perhaps an on_transmitting signal?
        m_lastTxStartTime = DriftingDateTime::currentDateTimeUtc();

        m_transmitting = true;
        transmitDisplay(true);
        statusUpdate();
    }

    // TODO: stop
    if (!m_btxok && m_btxok0 && m_iptt == 1)
        stopTx();
}

void UI_Constructor::updateClockUI(const QDateTime &now) {
    qint64 drift = DriftingDateTime::drift();
    QStringList parts;
    parts << now.date().toString("yyyy MMM dd");
    parts
        << (now.time().toString() +
            (!drift
                 ? " "
                 : QString(" (%1%2ms)").arg(drift > 0 ? "+" : "").arg(drift)));
    ui->labUTC->setText(parts.join(" "));
}

//------------------------------------------------------------- //guiUpdate()
void UI_Constructor::guiUpdate() {

    unsigned period = JS8::Submode::period(m_nSubMode);

    m_TRperiod = period; // Investigate: Does anyone need this?

    // Propagate any tx delay change to m_hb_loop and m_cq_loop.
    double tx_delay_now = m_config.txDelay();
    if (tx_delay_now != m_TxDelay) {
        m_TxDelay = tx_delay_now;
        qint64 tx_delay_ms = std::lround(tx_delay_now * 1000);
        m_hb_loop->onTxDelayChange(tx_delay_ms);
        m_cq_loop->onTxDelayChange(tx_delay_ms);
    }

    const QDateTime now = DriftingDateTime::currentDateTimeUtc();
    const qint64 seconds_since_epoch = now.toSecsSinceEpoch();

    if (m_transmitting or m_auto or m_tune) {
        refuseToSendIn30mWSPRBand();
        prepareSending(now.toMSecsSinceEpoch());
    }

    // Once per second:
    if (seconds_since_epoch != m_sec0) {
        m_sec0 = seconds_since_epoch;

        updateClockUI(now);

        if (m_monitoring or m_transmitting) {
            // We are lucky that TX delay starts well into the second
            // and lasts less than a second. So as long as we
            // do this near the begining of a second, we will never hit
            // the confusing "progress" of tx delay.
            progressBar.setMaximum(period);
            int progress = seconds_since_epoch % period;
            progressBar.setValue(progress);
        } else {
            progressBar.setValue(0);
        }

        if (m_transmitting) {
            tx_status_label.setStyleSheet(txStatusLabelStyle(TxStatusAppearance::Transmitting));

            if (m_tune) {
                tx_status_label.setText("Tx: TUNE");
            } else {
                auto message =
                    DecodedText(msgsent, msgibits, m_nSubMode).message();
                tx_status_label.setText(
                    QString("Tx: %1").arg(message).left(40).trimmed());
            }
            transmitDisplay(true);
        } else if (m_monitoring) {
            if (m_tx_watchdog) {
                tx_status_label.setStyleSheet(txStatusLabelStyle(TxStatusAppearance::IdleTimeout));
                tx_status_label.setText("Idle timeout");
            } else {
                tx_status_label.setStyleSheet(txStatusLabelStyle(TxStatusAppearance::Decoding));
                tx_status_label.setText(m_decoderBusy ? "Decoding"
                                                      : "Receiving");
            }
            transmitDisplay(false);
        } else if (!m_tx_watchdog) {
            tx_status_label.setStyleSheet("");
            tx_status_label.setText("");
        }

        auto callLabel = m_config.my_callsign();
        if (m_config.use_dynamic_grid() && !m_config.my_grid().isEmpty()) {
            callLabel =
                QString("%1 - %2").arg(callLabel).arg(m_config.my_grid());
        }
        ui->labCallsign->setText(callLabel);

        if (!m_monitoring) {
            ui->signal_meter_widget->setValue(0, 0);
        }

        // once per period
        if (seconds_since_epoch % period == 0) {
            tryBandHop();
        }

        // Need to do processing at the end of the period
        // or when there is something in m_rxActivityQueue.
        bool forceDirty = (seconds_since_epoch % period == 0) ||
                          ((seconds_since_epoch - 1) % period == 0) ||
                          !m_rxActivityQueue.isEmpty();

        // update the dial frequency once per second..
        displayDialFrequency();
        updateHBButtonDisplay();
        updateCQButtonDisplay();

        // once per second...but not when we're transmitting, unless it's in the
        // first second...
        if (!m_transmitting || (seconds_since_epoch % period == 0)) {
            // process all received activity...
            processActivity(forceDirty);

            // process outgoing tx queue...
            processTxQueue();

            // once processed, lets update the display...
            displayActivity(forceDirty);
            updateButtonDisplay();
            updateTextDisplay();
        }
    } // end of stuff we do once per second.

    displayTransmit();

    m_iptt0 = m_iptt;
    m_btxok0 = m_btxok;

    // Set the time to hit the start of the next UI_POLL_INTERVAL_MS slot.
    // This automatically hits close to the start of each second
    // and hence close to the start of each transmit period.
    qint64 now_at_end_ms = DriftingDateTime::currentMSecsSinceEpoch();
    qint64 time_into_poll_slot = now_at_end_ms % UI_POLL_INTERVAL_MS;
    qint64 until_start_of_next_poll_slot =
        UI_POLL_INTERVAL_MS - time_into_poll_slot;
    m_guiTimer.start(until_start_of_next_poll_slot);
} // End of guiUpdate

void UI_Constructor::startTx() {
#if IDLE_BLOCKS_TX
    if (m_tx_watchdog) {
        return;
    }
#endif

    auto text = ui->extFreeTextMsgEdit->toPlainText();
    if (!ensureCreateMessageReady(text)) {
        return;
    }

    if (!prepareNextMessageFrame()) {
        return;
    }

    m_dateTimeQSOOn = QDateTime{};
    if (m_transmitting)
        m_restart = true;

    if (!m_auto)
        auto_tx_mode(true);

    // disallow editing of the text while transmitting
    // ui->extFreeTextMsgEdit->setReadOnly(true);
    update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", true);

    // update the tx button display
    updateTxButtonDisplay();
}

void UI_Constructor::transmit() {
    if (m_modulator->isIdle()) {
        qDebug(mainwindow_js8) << "Asking the modulator to emit audio.";
        Q_EMIT sendMessage(freq() + m_XIT, m_nSubMode, m_TxDelay, m_soundOutput,
                           m_config.audio_output_channel());
        ui->signal_meter_widget->setValue(0, 0);
    } else {
        qDebug(mainwindow_js8) << "Not asking the modulator to emit audio as "
                                  "modulator isn't idle.";
    }
}

void UI_Constructor::stopTx() {
    Q_EMIT endTransmitMessage();

    auto dt = DecodedText(m_currentMessage.trimmed(), m_currentMessageBits,
                          m_nSubMode);
    last_tx_label.setText("Last Tx: " +
                          dt.message()); // m_currentMessage.trimmed());

    // TODO: uncomment if we want to mark after the frame is sent.
    //// // start message marker
    //// // - keep track of the total message sent so far, and mark it having
    /// been sent / m_totalTxMessage.append(dt.message()); /
    /// ui->extFreeTextMsgEdit->setCharsSent(m_totalTxMessage.length()); /
    /// qCDebug(mainwindow_js8) << "total sent:\n" << m_totalTxMessage; / // end
    /// message marker

    m_btxok = false;
    m_transmitting = false;
    m_iptt = 0;
    m_lastTxStopTime = DriftingDateTime::currentDateTimeUtc();
    if (!m_tx_watchdog) {
        tx_status_label.setStyleSheet("");
        tx_status_label.setText("");
    }

#if IDLE_BLOCKS_TX
    bool shouldContinue = !m_tx_watchdog && prepareNextMessageFrame();
#else
    bool shouldContinue = prepareNextMessageFrame();
#endif
    if (!shouldContinue) {
        // TODO: jsherer - split this up...
        ui->extFreeTextMsgEdit->clear();
        ui->extFreeTextMsgEdit->setReadOnly(false);
        update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", false);
        bool previous_stopTxButtonIsLongterm = m_stopTxButtonIsLongterm;
        m_stopTxButtonIsLongterm = false;
        on_stopTxButton_clicked();
        m_stopTxButtonIsLongterm = previous_stopTxButtonIsLongterm;
        tryRestoreFreqOffset();
    }

    pttReleaseTimer.start(
        TX_SWITCHOFF_DELAY); // end-of-transmission sequencer delay stopTx2
    monitor(true);
    statusUpdate();
}

/**
 *  stopTx2 is called from stopTx to open the PTT
 */
void UI_Constructor::stopTx2() {
    // GM8JCF: m_txFrameCount is set to the number of frames to be transmitted
    // when the send button is pressed and remains at that count until the last
    // frame is transmitted. So, we keep the PTT ON so long as m_txFrameCount is
    // non-zero

    qCDebug(mainwindow_js8) << "stopTx2 frames left" << m_txFrameCount;

    // If we're holding the PTT and there are more frames to transmit, do not
    // emit the PTT signal
    if (!m_tune && m_config.hold_ptt() && m_txFrameCount > 0) {
        return;
    }

    // Otherwise, emit the PTT signal
    emitPTT(false);
}

void UI_Constructor::TxAgain() { auto_tx_mode(true); }

void UI_Constructor::cacheActivity(QString key) {
    m_callActivityBandCache[key] = m_callActivity;
    m_bandActivityBandCache[key] = m_bandActivity;
    m_rxTextBandCache[key] = ui->textEditRX->toHtml();
    m_heardGraphIncomingBandCache[key] = m_heardGraphIncoming;
    m_heardGraphOutgoingBandCache[key] = m_heardGraphOutgoing;
}

void UI_Constructor::restoreActivity(QString key) {
    if (m_callActivityBandCache.contains(key)) {
        m_callActivity = m_callActivityBandCache[key];
    }

    if (m_bandActivityBandCache.contains(key)) {
        m_bandActivity = m_bandActivityBandCache[key];
    }

    if (m_rxTextBandCache.contains(key)) {
        ui->textEditRX->setHtml(m_rxTextBandCache[key]);
    }

    if (m_heardGraphIncomingBandCache.contains(key)) {
        m_heardGraphIncoming = m_heardGraphIncomingBandCache[key];
    }

    if (m_heardGraphOutgoingBandCache.contains(key)) {
        m_heardGraphOutgoing = m_heardGraphOutgoingBandCache[key];
    }

    displayActivity(true);
}

void UI_Constructor::clearActivity() {
    qCDebug(mainwindow_js8) << "clear activity";

    m_callSeenHeartbeat.clear();
    m_compoundCallCache.clear();
    m_rxCallCache.clear();
    m_rxCallQueue.clear();
    m_rxRecentCache.clear();
    m_rxDirectedCache.clear();
    m_rxCommandQueue.clear();
    m_lastTxMessage.clear();

    refreshInboxCounts();
    resetTimeDeltaAverage();

    clearBandActivity();
    clearRXActivity();
    clearCallActivity();

    displayActivity(true);
}

void UI_Constructor::clearBandActivity() {
    qCDebug(mainwindow_js8) << "clear band activity";
    m_bandActivity.clear();
    ui->tableWidgetRXAll->setRowCount(0);

    resetTimeDeltaAverage();
    displayBandActivity();
}

void UI_Constructor::clearRXActivity() {
    qCDebug(mainwindow_js8) << "clear rx activity";

    m_rxFrameBlockNumbers.clear();
    m_rxActivityQueue.clear();

    ui->textEditRX->clear();

    // make sure to clear the read only and transmitting flags so there's always
    // a "way out"
    ui->extFreeTextMsgEdit->clear();
    ui->extFreeTextMsgEdit->setReadOnly(false);
    update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", false);
}

void UI_Constructor::clearCallActivity() {
    qCDebug(mainwindow_js8) << "clear call activity";

    m_callActivity.clear();

    m_heardGraphIncoming.clear();
    m_heardGraphOutgoing.clear();

    ui->tableWidgetCalls->setRowCount(0);

    bool showIconColumn = false;
    createGroupCallsignTableRows(ui->tableWidgetCalls, "", showIconColumn);

    resetTimeDeltaAverage();
    displayCallActivity();
}

void UI_Constructor::createGroupCallsignTableRows(QTableWidget *table,
                                                  QString const &selectedCall,
                                                  bool &showIconColumn) {
    int count = 0;
    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();

    int startCol = 1;

    foreach (auto cd, m_callActivity.values()) {
        if (cd.call.trimmed().isEmpty()) {
            continue;
        }
        if (callsignAging &&
            cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
            continue;
        }
        count++;
    }

    table->horizontalHeaderItem(startCol)->setText(
        count == 0 ? columnLabel("Callsigns")
                   : QString(columnLabel("Callsigns (%1)")).arg(count));

    if (!m_config.avoid_allcall()) {
        table->insertRow(table->rowCount());

        auto emptyItem = new QTableWidgetItem("");
        emptyItem->setData(Qt::UserRole, QVariant("@ALLCALL"));
        table->setItem(table->rowCount() - 1, 0, emptyItem);

        auto item = new QTableWidgetItem(QString("@ALLCALL"));
        item->setData(Qt::UserRole, QVariant("@ALLCALL"));

        table->setItem(table->rowCount() - 1, startCol, item);
        table->setSpan(table->rowCount() - 1, startCol, 1,
                       table->columnCount());
        if (selectedCall == "@ALLCALL") {
            table->item(table->rowCount() - 1, 0)->setSelected(true);
            table->item(table->rowCount() - 1, startCol)->setSelected(true);
        }
    }

    auto groups = m_config.my_groups().values();
    std::sort(groups.begin(), groups.end());
    foreach (auto group, groups) {
        int col = 0;
        table->insertRow(table->rowCount());

        bool hasMessage = m_rxInboxCountCache.value(group, 0) > 0;

        auto iconItem = new QTableWidgetItem(hasMessage ? "\u2691" : "");
        iconItem->setData(Qt::UserRole, QVariant(group));
        iconItem->setToolTip(hasMessage ? "Message Available" : "");
        iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        table->setItem(table->rowCount() - 1, col++, iconItem);
        if (hasMessage) {
            showIconColumn = true;
        }

        auto item = new QTableWidgetItem(group);
        item->setData(Qt::UserRole, QVariant(group));
        item->setToolTip(generateCallDetail(group));
        table->setItem(table->rowCount() - 1, col, item);
        table->setSpan(table->rowCount() - 1, col, 1, table->columnCount());

        if (selectedCall == group) {
            table->item(table->rowCount() - 1, 0)->setSelected(true);
            table->item(table->rowCount() - 1, col)->setSelected(true);
        }
    }
}

void UI_Constructor::displayTextForFreq(QString text, int freq, QDateTime date,
                                        bool isTx, bool isNewLine,
                                        bool isLast) {
    // Don't display RX messages from blocked callsigns.
    // TX frames (our own transmissions) are never suppressed.
    if (!isTx) {
        auto const &blocklist = m_config.rx_callsign_blocklist();
        if (!blocklist.isEmpty()) {
            auto const sender = text.section(':', 0, 0).trimmed().toUpper();
            auto const senderBase = Radio::base_callsign(sender);
            if (!sender.isEmpty() &&
                (blocklist.contains(sender) ||
                 blocklist.contains(senderBase))) {
                m_rxBlockedOffsets.insert(freq);
                return;
            }
        }
        // Suppress continuation frames from a blocked offset
        if (m_rxBlockedOffsets.contains(freq)) {
            if (isLast || isNewLine) {
                m_rxBlockedOffsets.remove(freq);
                if (!isNewLine) {
                    return;
                }
            } else {
                return;
            }
        }
    }

    int lowFreq = freq / 10 * 10;
    int highFreq = lowFreq + 10;

    int block = -1;

    if (m_rxFrameBlockNumbers.contains(freq)) {
        block = m_rxFrameBlockNumbers[freq];
    } else if (m_rxFrameBlockNumbers.contains(lowFreq)) {
        block = m_rxFrameBlockNumbers[lowFreq];
        freq = lowFreq;
    } else if (m_rxFrameBlockNumbers.contains(highFreq)) {
        block = m_rxFrameBlockNumbers[highFreq];
        freq = highFreq;
    }

    qCDebug(mainwindow_js8) << "existing block?" << block << freq;

    if (isNewLine) {
        m_rxFrameBlockNumbers.remove(freq);
        m_rxFrameBlockNumbers.remove(lowFreq);
        m_rxFrameBlockNumbers.remove(highFreq);
        block = -1;
    }

    block = writeMessageTextToUI(date, text, freq, isTx, block);

    // never cache tx or last lines
    if (/*isTx || */ isLast) {
        // reset the cache so we're always progressing forward
        m_rxFrameBlockNumbers.clear();
    } else {
        m_rxFrameBlockNumbers.insert(freq, block);
        m_rxFrameBlockNumbers.insert(lowFreq, block);
        m_rxFrameBlockNumbers.insert(highFreq, block);
    }
}

void UI_Constructor::writeNoticeTextToUI(QDateTime date, QString text) {
    auto c = ui->textEditRX->textCursor();
    c.movePosition(QTextCursor::End);
    if (c.block().length() > 1) {
        c.insertBlock();
    }

    text = text.toHtmlEscaped();
    c.insertBlock();
    c.insertHtml(QString("<strong>%1 - %2</strong>")
                     .arg(date.time().toString())
                     .arg(text));

    c.movePosition(QTextCursor::End);

    ui->textEditRX->ensureCursorVisible();
    ui->textEditRX->verticalScrollBar()->setValue(
        ui->textEditRX->verticalScrollBar()->maximum());
}

int UI_Constructor::writeMessageTextToUI(QDateTime date, QString text, int freq,
                                         bool isTx, int block) {
    auto c = ui->textEditRX->textCursor();

    // find an existing block (that does not contain an EOT marker)
    bool found = false;
    if (block != -1) {
        QTextBlock b = c.document()->findBlockByNumber(block);
        c.setPosition(b.position());
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

        auto blockText = c.selectedText();
        c.clearSelection();
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        if (!blockText.contains(m_config.eot())) {
            found = true;
        }
    }

    if (!found) {
        c.movePosition(QTextCursor::End);
        if (c.block().length() > 1) {
            c.insertBlock();
        }
    }

    // fixup duplicate acks
    auto tc = c.document()->find(text);
    if (!tc.isNull() && tc.selectedText() == text &&
        (text.contains(" ACK ") || text.contains(" HEARTBEAT SNR "))) {
        tc.select(QTextCursor::BlockUnderCursor);

        if (tc.selectedText().trimmed().startsWith(date.time().toString())) {
            qCDebug(mainwindow_js8)
                << "found" << tc.selectedText() << "so not displaying...";
            return tc.blockNumber();
        }
    }

    if (found) {
        c.clearSelection();
        c.insertText(text);
    } else {
        text = text.toHtmlEscaped();
        text = text.replace("\n", "<br/>");
        text = text.replace("  ", "&nbsp;&nbsp;");
        c.insertBlock();
        c.insertHtml(QString("%1 - (%2) - %3")
                         .arg(date.time().toString())
                         .arg(freq)
                         .arg(text));
    }

    if (isTx) {
        c.block().setUserState(State::TX);
        highlightBlock(c.block(), m_config.tx_text_font(),
                       m_config.color_tx_foreground(), QColor(Qt::transparent));
    } else {
        c.block().setUserState(State::RX);
        highlightBlock(c.block(), m_config.rx_text_font(),
                       m_config.color_rx_foreground(), QColor(Qt::transparent));
    }

    ui->textEditRX->ensureCursorVisible();
    ui->textEditRX->verticalScrollBar()->setValue(
        ui->textEditRX->verticalScrollBar()->maximum());

    return c.blockNumber();
}

bool UI_Constructor::isMessageQueuedForTransmit() {
    return m_transmitting || m_txFrameCount > 0;
}

bool UI_Constructor::isInDecodeDelayThreshold(int ms) {
    if (!m_lastTxStopTime.isValid() || m_lastTxStopTime.isNull()) {
        return false;
    }

    return m_lastTxStopTime.msecsTo(DriftingDateTime::currentDateTimeUtc()) <
           ms;
}

void UI_Constructor::prependMessageText(QString text) {
    // don't add message text if we already have a transmission queued...
    if (isMessageQueuedForTransmit()) {
        return;
    }

    auto c = QTextCursor(ui->extFreeTextMsgEdit->textCursor());
    c.movePosition(QTextCursor::Start);
    c.insertText(text);
}

void UI_Constructor::addMessageText(QString text, bool clear,
                                    bool selectFirstPlaceholder) {
    // don't add message text if we already have a transmission queued...
    if (isMessageQueuedForTransmit()) {
        return;
    }

    if (clear) {
        ui->extFreeTextMsgEdit->clear();
    }

    QTextCursor c = ui->extFreeTextMsgEdit->textCursor();
    if (c.hasSelection()) {
        c.removeSelectedText();
    }

    int pos = c.position();
    c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);

    bool isSpace =
        c.selectedText().isEmpty() || c.selectedText().at(0).isSpace();
    c.clearSelection();

    c.setPosition(pos);

    if (!isSpace) {
        c.insertText(" ");
    }

    c.insertText(text);

    if (selectFirstPlaceholder) {
        auto match = QRegularExpression("(\\[[^\\]]+\\])")
                         .match(ui->extFreeTextMsgEdit->toPlainText());
        if (match.hasMatch()) {
            c.setPosition(match.capturedStart());
            c.setPosition(match.capturedEnd(), QTextCursor::KeepAnchor);
            ui->extFreeTextMsgEdit->setTextCursor(c);
        }
    }

    ui->extFreeTextMsgEdit->setFocus();
}

void UI_Constructor::enqueueMessage(int priority, QString message, int offset,
                                    Callback c) {
    m_txMessageQueue.enqueue(PrioritizedMessage{
        DriftingDateTime::currentDateTimeUtc(), priority, message, offset, c});
}

void UI_Constructor::resetMessage() {
    resetMessageUI();
    resetMessageTransmitQueue();
}

void UI_Constructor::resetMessageUI() {
    m_nextFreeTextMsg.clear();
    ui->extFreeTextMsgEdit->clear();
    ui->extFreeTextMsgEdit->setReadOnly(false);

    update_dynamic_property(ui->extFreeTextMsgEdit, "transmitting", false);

    if (ui->startTxButton->isChecked()) {
        ui->startTxButton->setChecked(false);
    }
}

bool UI_Constructor::ensureCallsignSet(bool alert) {
    if (m_config.my_callsign().trimmed().isEmpty()) {
        if (alert)
            JS8MessageBox::warning_message(
                this, tr("Please enter your callsign in the settings."));
        openSettings();
        return false;
    }

    if (m_config.my_grid().trimmed().isEmpty()) {
        if (alert)
            JS8MessageBox::warning_message(
                this, tr("Please enter your grid locator in the settings."));
        openSettings();
        return false;
    }

    return true;
}

bool UI_Constructor::ensureKeyNotStuck(QString const &text) {
    // be annoying and drop messages with all the same character to reduce
    // spam...
    if (text.length() > 5 &&
        QString(text).replace(text.at(0), "").trimmed().isEmpty()) {
        return false;
    }

    return true;
}

bool UI_Constructor::ensureNotIdle() {
    if (!m_config.watchdog()) {
        return true;
    }

    if (m_idleMinutes < m_config.watchdog()) {
        return true;
    }

    tx_watchdog(true); // disable transmit and auto replies
    return false;
}

bool UI_Constructor::ensureCanTransmit() {
    return ui->monitorTxButton->isChecked();
}

bool UI_Constructor::ensureCreateMessageReady(const QString &text) {
    if (text.isEmpty()) {
        return false;
    }

    if (!ensureCanTransmit()) {
        on_stopTxButton_clicked();
        return false;
    }

    if (!ensureCallsignSet()) {
        on_stopTxButton_clicked();
        return false;
    }

    if (!ensureNotIdle()) {
        on_stopTxButton_clicked();
        return false;
    }

    if (!ensureKeyNotStuck(text)) {
        on_stopTxButton_clicked();
        JS8MessageBox::warning_message(
            this,
            tr("Message Not Sent"),
            tr("It appears you may have a stuck key (more than 5 repeated characters). "
               "Please check your keyboard and try again."));
        return false;
    }

    return true;
}

QString UI_Constructor::createMessage(QString const &text,
                                      bool *pDisableTypeahead) {
    return createMessageTransmitQueue(
        replaceMacros(text, buildMacroValues(), false), true, false,
        pDisableTypeahead);
}

QString UI_Constructor::appendMessage(QString const &text, bool isData,
                                      bool *pDisableTypeahead) {
    return createMessageTransmitQueue(
        replaceMacros(text, buildMacroValues(), false), false, isData,
        pDisableTypeahead);
}

QString UI_Constructor::createMessageTransmitQueue(QString const &text,
                                                   bool reset, bool isData,
                                                   bool *pDisableTypeahead) {
    if (reset) {
        resetMessageTransmitQueue();
    }

    auto frames = buildMessageFrames(text, isData, pDisableTypeahead);

    QStringList lines;
    foreach (auto frame, frames) {
        auto dt = DecodedText(frame.first, frame.second, m_nSubMode);
        lines.append(dt.message());
    }

    m_txFrameQueue.append(frames);
    m_txFrameCount += frames.length();

    // TODO: jsherer - move this outside of create message transmit queue
    // if we're transmitting a message to be displayed, we should bump the
    // repeat buttons... "Bump the repeat buttons" from 2018 probably translates
    // to "stop automatic transmission loops" in 2025: qCDebug(mainwindow_js8)
    // << "Cancel HB and CQ transmit loops in createMessageTransmitQueue";
    // m_cq_loop->onLoopCancel();
    // m_hb_loop->onLoopCancel();
    // But the loops cause this code to be executed as part of their
    // normal operation, when the first transmission is sent.
    // So the cancelation makes it impossible to iterate through the loop a
    // second time.

    // return the text
    return lines.join("");
}

// automatic HB ACK suppression, creates and queries a sqlite3 database that
// that keeps track of last heard HB's.
QString UI_Constructor::hbBlockingPath() const {
    return QDir::toNativeSeparators(
        m_config.writeable_data_dir().absoluteFilePath("hb_blocking.db3"));
}

void UI_Constructor::restoreMessage() {
    if (m_lastTxMessage.isEmpty()) {
        return;
    }
    addMessageText(Varicode::rstrip(m_lastTxMessage), true);
}

/**
 * @brief Resets the frame-level transmission state after a message completes.
 *
 * This function clears the frame queue and resets frame counters, preparing
 * the system for the next message transmission. Importantly, it does NOT
 * clear m_txMessageQueue, which holds pending high-level messages (e.g.,
 * queued APRS relay messages) that should be transmitted after the current
 * transmission completes.
 *
 * @note Called via resetMessage() -> on_stopTxButton_clicked() when
 *       transmission ends.
 */
void UI_Constructor::resetMessageTransmitQueue() {
    m_txFrameCount = 0;
    m_txFrameCountSent = 0;
    m_txFrameQueue.clear();
    // Note: m_txMessageQueue is intentionally NOT cleared here.
    // It holds pending messages (e.g., APRS relay messages) that should
    // be transmitted after the current transmission completes.

    // reset the total message sent
    m_totalTxMessage.clear();
}

QPair<QString, int> UI_Constructor::popMessageFrame() {
    if (m_txFrameQueue.isEmpty()) {
        return QPair<QString, int>{};
    }
    return m_txFrameQueue.dequeue();
}

void UI_Constructor::currentTextChanged() {
    auto const text = ui->extFreeTextMsgEdit->toPlainText();

    // keep track of dirty flags
    m_txTextDirty = text != m_txTextDirtyLastText;
    m_txTextDirtyLastText = text;

    // immediately update the display
    updateButtonDisplay();
    updateTextDisplay();
}

void UI_Constructor::tableSelectionChanged(QItemSelection const &,
                                           QItemSelection const &) {
    currentTextChanged();

    auto const selectedCall = callsignSelected();

    if (selectedCall != m_prevSelectedCallsign) {
        callsignSelectedChanged(m_prevSelectedCallsign, selectedCall);
    }
}

QList<QPair<QString, int>>
UI_Constructor::buildMessageFrames(const QString &text, bool isData,
                                   bool *pDisableTypeahead) {
    // prepare selected callsign for directed message
    QString selectedCall = callsignSelected();

    // prepare compound
    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);

    bool forceIdentify = !m_config.avoid_forced_identify();

    // TODO: might want to be more explicit?
    bool forceData = m_txFrameCountSent > 0 && isData;

    Varicode::MessageInfo info;
    auto frames = Varicode::buildMessageFrames(mycall, mygrid, selectedCall,
                                               text, forceIdentify, forceData,
                                               m_nSubMode, &info);

    if (pDisableTypeahead) {
        // checksummed commands should not allow typeahead
        *pDisableTypeahead = (!info.dirCmd.isEmpty() &&
                              Varicode::isCommandChecksumed(info.dirCmd));
    }

#if 0
    qCDebug(mainwindow_js8) << "frames:";
    foreach(auto frame, frames){
        auto dt = DecodedText(frame.frame, frame.bits);
        qCDebug(mainwindow_js8) << "->" << frame << dt.message() << Varicode::frameTypeString(dt.frameType());
    }
#endif

    return frames;
}

bool UI_Constructor::prepareNextMessageFrame() {
    // check to see if the last i3bit was a last bit
    bool i3bitLast = (m_i3bit & Varicode::JS8CallLast) == Varicode::JS8CallLast;

    // TODO: should this be user configurable?
    bool shouldForceDataForTypeahead = !i3bitLast;

    // reset i3
    m_i3bit = Varicode::JS8Call;

    // typeahead
    bool shouldDisableTypeahead = false;
    if (ui->extFreeTextMsgEdit->isDirty() &&
        !ui->extFreeTextMsgEdit->isEmpty()) {
        // block edit events while computing next frame
        QString newText;
        ui->extFreeTextMsgEdit->setReadOnly(true);
        {
            auto sent = ui->extFreeTextMsgEdit->sentText();
            auto unsent = ui->extFreeTextMsgEdit->unsentText();
            qCDebug(mainwindow_js8) << "text dirty for typeahead\n"
                                    << sent << "\n"
                                    << unsent;
            m_txFrameQueue.clear();
            m_txFrameCount = 0;

            newText = appendMessage(unsent, shouldForceDataForTypeahead,
                                    &shouldDisableTypeahead);

            // if this was the last frame, append a newline
            if (i3bitLast) {
                m_totalTxMessage.append("\n");
                newText.prepend("\n");
            }

            qCDebug(mainwindow_js8) << "unsent replaced to" << "\n" << newText;
        }
        ui->extFreeTextMsgEdit->setReadOnly(shouldDisableTypeahead);
        ui->extFreeTextMsgEdit->replaceUnsentText(newText, true);
        ui->extFreeTextMsgEdit->setClean();
    }

    QPair<QString, int> f = popMessageFrame();
    auto frame = f.first;
    auto bits = f.second;

    // if not the first frame, ensure first bit is not set
    if (m_txFrameCountSent > 0) {
        bits &= ~Varicode::JS8CallFirst;
    }

    // if last frame, ensure the last bit is set
    if (m_txFrameQueue.isEmpty()) {
        bits |= Varicode::JS8CallLast;
    }

    if (frame.isEmpty()) {
        m_nextFreeTextMsg.clear();
        updateTxButtonDisplay();
        return false;
    }

    // append this frame to the total message sent so far
    auto dt = DecodedText(frame, bits, m_nSubMode);
    m_totalTxMessage.append(dt.message());
    ui->extFreeTextMsgEdit->setCharsSent(m_totalTxMessage.length());
    m_txFrameCountSent += 1;
    m_lastTxMessage = m_totalTxMessage;
    qCDebug(mainwindow_js8) << "total sent:" << m_txFrameCountSent << "\n"
                            << m_totalTxMessage;

    // display the frame...
    if (m_txFrameQueue.isEmpty()) {
        displayTextForFreq(
            QString("%1 %2 ").arg(dt.message()).arg(m_config.eot()), freq(),
            DriftingDateTime::currentDateTimeUtc(), true, false, true);
    } else {
        displayTextForFreq(dt.message(), freq(),
                           DriftingDateTime::currentDateTimeUtc(), true,
                           m_txFrameCountSent == 1, false);
    }

    m_nextFreeTextMsg = frame;
    m_i3bit = bits;

    updateTxButtonDisplay();

    return true;
}

bool UI_Constructor::isFreqOffsetFree(int const f, int const bw) {
    // if this frequency is our current frequency, or it's in our
    // directed cache, it's free.

    if ((freq() == f) || isDirectedOffset(f, nullptr))
        return true;

    // Run through the band activity; if there's no activity for a given
    // offset, or we last received on it more than 30 seconds ago, then
    // it's free. If it's an occupied slot within the bandwidth of where
    // we'd like to transmit, then it's not free.

    auto const now = DriftingDateTime::currentDateTimeUtc();

    for (auto [offset, activity] : m_bandActivity.asKeyValueRange()) {
        if (activity.isEmpty() ||
            activity.last().utcTimestamp.secsTo(now) >= 30)
            continue;

        if (qAbs(offset - f) < bw)
            return false;
    }

    return true;
}

int UI_Constructor::findFreeFreqOffset(int fmin, int fmax, int bw) {
    int nslots = (fmax - fmin) / bw;

    int f = fmin;
    for (int i = 0; i < nslots; i++) {
        f = fmin + bw * (QRandomGenerator::global()->generate() % nslots);
        if (isFreqOffsetFree(f, bw)) {
            return f;
        }
    }

    for (int i = 0; i < nslots; i++) {
        f = fmin + (QRandomGenerator::global()->generate() % (fmax - fmin));
        if (isFreqOffsetFree(f, bw)) {
            return f;
        }
    }

    // return fmin if there's no free offset
    return fmin;
}

#if 0
// schedulePing
void UI_Constructor::scheduleHeartbeat(bool first){
    auto timestamp = DriftingDateTime::currentDateTimeUtc();

    // if we have the heartbeat interval disabled, return early, unless this is a "heartbeat now"
    if(!m_config.heartbeat() && !first){
        heartbeatTimer.stop();
        return;
    }

    // remove milliseconds
    auto t = timestamp.time();
    t.setHMS(t.hour(), t.minute(), t.second());
    timestamp.setTime(t);

    // round to 15 second increment
    int secondsSinceEpoch = (timestamp.toMSecsSinceEpoch()/1000);
    int delta = roundUp(secondsSinceEpoch, 15) + 1 + (first ? 0 : qMax(1, m_config.heartbeat()) * 60) - secondsSinceEpoch;
    timestamp = timestamp.addSecs(delta);

    // 25% of the time, switch intervals
    float prob = (float) QRandomGenerator::global()->generate() / (RAND_MAX);
    if(prob < 0.25){
        timestamp = timestamp.addSecs(15);
    }

    m_nextHeartbeat = timestamp;
    m_nextHeartbeatQueued = false;
    m_nextHeartPaused = false;

    if(!heartbeatTimer.isActive()){
        heartbeatTimer.setInterval(1000);
        heartbeatTimer.start();
    }
}

// pausePing
void UI_Constructor::pauseHeartbeat(){
    m_nextHeartPaused = true;

    if(heartbeatTimer.isActive()){
        heartbeatTimer.stop();
    }
}

// unpausePing
void UI_Constructor::unpauseHeartbeat(){
    scheduleHeartbeat(false);
}

// checkPing
void UI_Constructor::checkHeartbeat(){
    if(m_config.heartbeat() <= 0){
        return;
    }
    auto secondsUntilHeartbeat = DriftingDateTime::currentDateTimeUtc().secsTo(m_nextHeartbeat);
    if(secondsUntilHeartbeat > 5 && m_txHeartbeatQueue.isEmpty()){
        return;
    }
    if(m_nextHeartbeatQueued){
        return;
    }
    if(m_tx_watchdog){
        return;
    }

    // idle heartbeat watchdog!
    if (m_config.watchdog() && m_idleMinutes >= m_config.watchdog ()){
      tx_watchdog (true);       // disable transmit
      return;
    }

    prepareHeartbeat();
}

// preparePing
void UI_Constructor::prepareHeartbeat(){
    QStringList lines;

    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);

    // JS8Call Style
    if(m_txHeartbeatQueue.isEmpty()){
        lines.append(QString("%1: HEARTBEAT %2").arg(mycall).arg(mygrid));
    } else {
        while(!m_txHeartbeatQueue.isEmpty() && lines.length() < 1){
            lines.append(m_txHeartbeatQueue.dequeue());
        }
    }

    // Choose a ping frequency
    auto f = m_config.heartbeat_anywhere() ? -1 : findFreeFreqOffset(500, 1000, 50);

    auto text = lines.join(QChar('\n'));
    if(text.isEmpty()){
        return;
    }

    // Queue the ping
    enqueueMessage(PriorityLow, text, f, [this](){
        m_nextHeartbeatQueued = false;
    });

    m_nextHeartbeatQueued = true;
}
#endif

void UI_Constructor::on_startTxButton_toggled(bool checked) {
    if (checked) {
        startTx();
    } else {
        resetMessage();
        on_stopTxButton_clicked();
        stopTx();
    }
}

void UI_Constructor::toggleTx(bool start) {
    if (start && ui->startTxButton->isChecked()) {
        return;
    }
    if (!start && !ui->startTxButton->isChecked()) {
        return;
    }
    qCDebug(mainwindow_js8)
        << "toggleTx(" << start << ") setting the TX button.";
    ui->startTxButton->setChecked(start);
}

void UI_Constructor::on_logQSOButton_clicked() // Log QSO button
{
    QString call = callsignSelected();
    if (m_callSelectedTime.contains(call)) {
        m_dateTimeQSOOn = m_callSelectedTime[call];
    }
    if (!m_dateTimeQSOOn.isValid()) {
        m_dateTimeQSOOn = DriftingDateTime::currentDateTimeUtc();
    }
    auto dateTimeQSOOff = DriftingDateTime::currentDateTimeUtc();
    if (dateTimeQSOOff < m_dateTimeQSOOn)
        dateTimeQSOOff = m_dateTimeQSOOn;

    if (call.startsWith("@")) {
        call = "";
    }
    QString grid = "";
    if (m_callActivity.contains(call)) {
        grid = m_callActivity[call].grid;
    }
    QString opCall = m_opCall;
    if (opCall.isEmpty()) {
        opCall = m_config.my_callsign();
    }

    QString comments = ui->textEditRX->textCursor().selectedText();

    // don't reset the log window if the call hasn't changed.
    if (!m_logDlg->currentCall().isEmpty() &&
        call.trimmed() == m_logDlg->currentCall()) {
        m_logDlg->show();
        return;
    }

    // kj4ctd - hackish but I don't see anywhere else that we set rptSent
    if (m_callActivity.contains(call)) {
        auto cd = m_callActivity[call];
        if (cd.snr > -50) {
            m_rptSent = Varicode::formatSNR(cd.snr);
        }
    }

    m_logDlg->initLogQSO(call.trimmed(), grid.trimmed(), "JS8", m_rptSent,
                         m_rptRcvd, m_dateTimeQSOOn, dateTimeQSOOff,
                         m_freqNominal + freq(), m_config.my_callsign(),
                         m_config.my_grid(), opCall, comments);
}

void UI_Constructor::acceptQSO(
    QDateTime const &QSO_date_off, QString const &call, QString const &grid,
    Frequency dial_freq, QString const &mode, QString const &submode,
    QString const &rpt_sent, QString const &rpt_received,
    QString const &comments, QString const &name, QDateTime const &QSO_date_on,
    QString const &operator_call, QString const &my_call,
    QString const &my_grid, QByteArray const &ADIF,
    QVariantMap const &additionalFields) {
    QString date = QSO_date_on.toString("yyyyMMdd");
    m_logBook.addAsWorked(m_hisCall, m_config.bands()->find(m_freqNominal),
                          mode, submode, grid, date, name, comments);

    qCDebug(mainwindow_js8) << "acceptQSO rptSent (" << m_rptSent << ")";
    qCDebug(mainwindow_js8) << "acceptQSO rptRcvd (" << m_rptRcvd << ")";

    // Log to JS8Call API
    if (canSendNetworkMessage()) {
        sendNetworkMessage(
            "LOG.QSO", QString(ADIF),
            {{"_ID", QVariant(-1)},
             {"UTC.ON", QVariant(QSO_date_on.toMSecsSinceEpoch())},
             {"UTC.OFF", QVariant(QSO_date_off.toMSecsSinceEpoch())},
             {"CALL", QVariant(call)},
             {"GRID", QVariant(grid)},
             {"FREQ", QVariant(dial_freq)},
             {"MODE", QVariant(mode)},
             {"SUBMODE", QVariant(submode)},
             {"RPT.SENT", QVariant(rpt_sent)},
             {"RPT.RECV", QVariant(rpt_received)},
             {"NAME", QVariant(name)},
             {"COMMENTS", QVariant(comments)},
             {"STATION.OP", QVariant(operator_call)},
             {"STATION.CALL", QVariant(my_call)},
             {"STATION.GRID", QVariant(my_grid)},
             {"EXTRA", additionalFields}});
    }

    // Log to N1MM Logger
    if (m_config.broadcast_to_n1mm() && m_config.valid_n1mm_info()) {
        const QHostAddress n1mmhost = QHostAddress(m_config.n1mm_server_name());
        QUdpSocket _sock;
        auto rzult = _sock.writeDatagram(ADIF + " <eor>", n1mmhost,
                                         quint16(m_config.n1mm_server_port()));
        if (rzult == -1) {
            bool hidden = m_logDlg->isHidden();
            m_logDlg->setHidden(true);
            JS8MessageBox::warning_message(
                this, tr("Error sending log to N1MM"),
                tr("Write returned \"%1\"").arg(rzult));
            m_logDlg->setHidden(hidden);
        }
    }

    // Log to N3FJP Logger
    if (m_config.broadcast_to_n3fjp() && m_config.valid_n3fjp_info()) {
        QString data = QString("<CMD>"
                               "<ADDDIRECT>"
                               "<EXCLUDEDUPES>TRUE</EXCLUDEDUPES>"
                               "<STAYOPEN>FALSE</STAYOPEN>"
                               "<fldDateStr>%1</fldDateStr>"
                               "<fldTimeOnStr>%2</fldTimeOnStr>"
                               "<fldCall>%3</fldCall>"
                               "<fldGridR>%4</fldGridR>"
                               "<fldBand>%5</fldBand>"
                               "<fldFrequency>%6</fldFrequency>"
                               "<fldMode>JS8</fldMode>"
                               "<fldOperator>%7</fldOperator>"
                               "<fldNameR>%8</fldNameR>"
                               "<fldComments>%9</fldComments>"
                               "<fldRstS>%10</fldRstS>"
                               "<fldRstR>%11</fldRstR>"
                               "%12"
                               "</CMD>");

        data = data.arg(QSO_date_on.toString("yyyy/MM/dd"));
        data = data.arg(QSO_date_on.toString("H:mm"));
        data = data.arg(call);
        data = data.arg(grid);
        data = data.arg(m_config.bands()->find(dial_freq).replace("m", ""));
        data = data.arg(Radio::frequency_MHz_string(dial_freq));
        data = data.arg(operator_call);
        data = data.arg(name);
        data = data.arg(comments);
        data = data.arg(rpt_sent);
        data = data.arg(rpt_received);

        int other = 0;
        QStringList additional;
        if (!additionalFields.isEmpty()) {
            foreach (auto key, additionalFields.keys()) {
                QString n3key;
                if (N3FJP_ADIF_MAP.contains(key)) {
                    n3key = N3FJP_ADIF_MAP.value(key);
                } else {
                    other++;
                    n3key = N3FJP_ADIF_MAP.value(QString("*%1").arg(other));
                }

                if (n3key.isEmpty()) {
                    break;
                }
                auto value = additionalFields[key].toString();
                additional.append(QString("<%1>%2</%1>").arg(n3key).arg(value));
            }
        }
        data = data.arg(additional.join(""));

        auto host = m_config.n3fjp_server_name();
        auto port = m_config.n3fjp_server_port();

        if (m_n3fjpClient->sendNetworkMessage(host, port, data.toLocal8Bit(),
                                              true, 500)) {
            QTimer::singleShot(300, this, [this, host, port]() {
                m_n3fjpClient->sendNetworkMessage(
                    host, port, "<CMD><CHECKLOG></CMD>", true, 100);
                m_n3fjpClient->sendNetworkMessage(host, port, "\r\n", true,
                                                  100);
            });
        } else {
            bool hidden = m_logDlg->isHidden();
            m_logDlg->setHidden(true);
            JS8MessageBox::warning_message(
                this, tr("Error sending log to N3FJP"),
                tr("Write failed for \"%1:%2\"").arg(host).arg(port));
            m_logDlg->setHidden(hidden);
        }
    }

    /**
     * @brief Log QSO to WSJT-X Protocol
     *
     * Sends QSO logged information to WSJT-X protocol clients. Sends both
     * the QSOLogged message and the LoggedADIF message (type 12) which
     * contains the ADIF formatted record. The ADIF message is what most
     * WSJT-X clients actually use for logging.
     */
    if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
        m_wsjtxMessageMapper->sendQSOLogged(QSO_date_off, call, grid, dial_freq,
                                            mode, rpt_sent, rpt_received,
                                            my_call, my_grid);
        // Also send ADIF formatted message (this is what clients actually use)
        if (m_wsjtxMessageClient) {
            m_wsjtxMessageClient->logged_ADIF(ADIF);
        }
    }

    // reload the logbook data
    m_logBook.init();

    clearCallsignSelected();

    displayCallActivity();

    m_dateTimeQSOOn = QDateTime{};
}

void UI_Constructor::on_actionModeJS8HB_toggled(bool) {
    // prep hb mode

    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());
    displayActivity(true);

    setupJS8();
}

void UI_Constructor::on_actionHeartbeatAcknowledgements_toggled(bool) {
    // prep hb ack mode

    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());
    displayActivity(true);

    setupJS8();
}

void UI_Constructor::on_actionModeMultiDecoder_toggled(bool checked) {
    Q_UNUSED(checked);

    displayActivity(true);

    setupJS8();
}

void UI_Constructor::on_actionModeJS8Normal_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Fast_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Turbo_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Slow_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeJS8Ultra_triggered() { setupJS8(); }

void UI_Constructor::on_actionModeAutoreply_toggled(bool) {
    // update the HB ack option (needs autoreply on)
    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());

    // Update the status label to reflect the auto reply state
    const QString autoReplyState = ui->actionModeAutoreply->isChecked() ? "On" : "Off";
    auto_reply_label.setText(QString("Auto Reply: %1").arg(autoReplyState));

    // then update the js8 mode
    setupJS8();
}

bool UI_Constructor::canCurrentModeSendHeartbeat() const {
    return (m_nSubMode == Varicode::JS8CallFast ||
            m_nSubMode == Varicode::JS8CallNormal ||
            m_nSubMode == Varicode::JS8CallSlow);
}

void UI_Constructor::prepareMonitorControls() {
    // on_monitorButton_toggled(!m_config.monitor_off_at_startup());
    ui->monitorTxButton->setChecked(!m_config.transmit_off_at_startup());
}

void UI_Constructor::prepareHeartbeatMode(bool enabled) {
    // Not all submodes supports HBs.
    ui->hbMacroButton->setVisible(enabled);
    if (!enabled) {
        m_hb_loop->onLoopCancel();
        ui->hbMacroButton->setChecked(false);
    }
    ui->actionHeartbeat->setEnabled(enabled);
    ui->actionModeJS8HB->setEnabled(canCurrentModeSendHeartbeat());
    ui->actionHeartbeatAcknowledgements->setEnabled(
        enabled && ui->actionModeAutoreply->isChecked());

#if 0
    if(enabled){
        m_config.addGroup("@HB");
    } else {
        m_config.removeGroup("@HB");
    }
#endif

#if 0
    //ui->actionCQ->setEnabled(!enabled);
    //ui->actionFocus_Message_Reply_Area->setEnabled(!enabled);

    // default to not displaying the other buttons
    // ui->cqMacroButton->setVisible(!enabled);
    // ui->replyMacroButton->setVisible(!enabled);
    // ui->snrMacroButton->setVisible(!enabled);
    // ui->infoMacroButton->setVisible(!enabled);
    // ui->macrosMacroButton->setVisible(!enabled);
    // ui->queryButton->setVisible(!enabled);
    // ui->extFreeTextMsgEdit->setVisible(!enabled);
    // if(enabled){
    //     ui->extFreeTextMsgEdit->clear();
    // }

    // show heartbeat and acks in hb mode only
    // ui->actionShow_Band_Heartbeats_and_ACKs->setChecked(enabled);
    // ui->actionShow_Band_Heartbeats_and_ACKs->setVisible(true);
    // ui->actionShow_Band_Heartbeats_and_ACKs->setEnabled(false);
#endif

    updateHBButtonDisplay();
    updateButtonDisplay();
}

void UI_Constructor::setupJS8() {
    m_nSubMode = Varicode::JS8CallNormal;

    if (ui->actionModeJS8Normal->isChecked())
        m_nSubMode = Varicode::JS8CallNormal;
    else if (ui->actionModeJS8Fast->isChecked())
        m_nSubMode = Varicode::JS8CallFast;
    else if (ui->actionModeJS8Turbo->isChecked())
        m_nSubMode = Varicode::JS8CallTurbo;
    else if (ui->actionModeJS8Slow->isChecked())
        m_nSubMode = Varicode::JS8CallSlow;
    else if (ui->actionModeJS8Ultra->isChecked())
        m_nSubMode = Varicode::JS8CallUltra;

    // Only enable heartbeat for modes that support it
    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());

    updateModeButtonText();

    m_wideGraph->setSubMode(m_nSubMode);
    m_wideGraph->setFilterMinimumBandwidth(
        JS8::Submode::bandwidth(m_nSubMode) +
        JS8::Submode::rxThreshold(m_nSubMode) * 2);

    enable_DXCC_entity(m_config.DXCC());
    m_config.frequencies()->filter(m_config.region(), Mode::JS8);
    m_FFTSize = JS8_NSPS / 2;
    Q_EMIT FFTSize(m_FFTSize);
    setup_status_bar();
    m_TRperiod = JS8::Submode::period(m_nSubMode);
    m_wideGraph->show();

    Q_ASSERT(JS8_NTMAX == 60);
    m_wideGraph->setPeriod(m_TRperiod);
    m_detector->setTRPeriod(JS8_NTMAX); // TODO - not thread safe

    updateTextDisplay();
    refreshTextDisplay();
    statusChanged();
}

void UI_Constructor::setFreq(int const n) {
    m_freq = n;
    m_wideGraph->setFreq(n);
    Q_EMIT transmitFrequency(n + m_XIT);
    statusUpdate();
}

void UI_Constructor::on_actionErase_ALL_TXT_triggered() // Erase ALL.TXT
{
    int ret = JS8MessageBox::query_message(
        this, tr("Confirm Erase"),
        tr("Are you sure you want to erase file ALL.TXT?"));
    if (ret == JS8MessageBox::Yes) {
        QFile f{m_config.writeable_data_dir().absoluteFilePath("ALL.TXT")};
        f.remove();
        m_RxLog = 1;
    }
}

void UI_Constructor::on_actionErase_js8call_log_adi_triggered() {
    int ret = JS8MessageBox::query_message(
        this, tr("Confirm Erase"),
        tr("Are you sure you want to erase file js8call_log.adi?"));
    if (ret == JS8MessageBox::Yes) {
        QFile f{
            m_config.writeable_data_dir().absoluteFilePath("js8call_log.adi")};
        f.remove();

        m_logBook.init();
    }
}

void UI_Constructor::on_actionOpen_log_directory_triggered() {
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(m_config.writeable_data_dir().absolutePath()));
}

void UI_Constructor::band_changed() {
    if (m_config.pwrBandTxMemory() && !m_tune) {
        if (m_pwrBandTxMemory.contains(m_lastBand)) {
            ui->outAttenuation->setValue(m_pwrBandTxMemory[m_lastBand].toInt());
        } else {
            m_pwrBandTxMemory[m_lastBand] = ui->outAttenuation->value();
        }
    }
}

void UI_Constructor::enable_DXCC_entity(bool /*on*/) {
    m_logBook.init(); // re-read the log and cty.dat files
    updateGeometry();
}

void UI_Constructor::buildFrequencyMenu(QMenu *menu) {
    auto custom = menu->addAction("Set a Custom Frequency...");

    connect(custom, &QAction::triggered, this, [this]() {
        bool ok = false;
        auto currentFreq = Radio::frequency_MHz_string(dialFrequency());
        QString newFreq =
            QInputDialog::getText(this, tr("Set a Custom Frequency"),
                                  tr("Frequency in MHz:"), QLineEdit::Normal,
                                  currentFreq, &ok)
                .toUpper()
                .trimmed();
        if (!ok) {
            return;
        }

        setRig(Radio::frequency(newFreq, 6));
    });

    menu->addSeparator();

    auto frequencies = m_config.frequencies()->frequency_list();
    std::sort(frequencies.begin(), frequencies.end(),
              [](FrequencyList_v3::Item &a, FrequencyList_v3::Item &b) {
                  return a.frequency_ < b.frequency_;
              });

    foreach (auto f, frequencies) {
        auto freq = Radio::pretty_frequency_MHz_string(f.frequency_);
        auto const &band = m_config.bands()->find(f.frequency_);

        QString description = (f.description_.isEmpty()) ? ""
            : QString(" - %1").arg(f.description_);

        auto a =
            menu->addAction(QString("%1:%2%2%3 MHz%4")
                                .arg(band)
                                .arg(QString(" ").repeated(5 - band.length()))
                                .arg(freq)
                                .arg(description));
        connect(a, &QAction::triggered, this,
                [this, f]() { setRig(f.frequency_); });
    }
}

void UI_Constructor::buildHeartbeatMenu(QMenu *menu) {
    if (m_hbInterval > 0) {
        auto startStop = menu->addAction(ui->hbMacroButton->isChecked()
                                             ? "Stop Heartbeat Timer"
                                             : "Start Heartbeat Timer");
        connect(startStop, &QAction::triggered, this,
                [this]() { ui->hbMacroButton->toggle(); });
        menu->addSeparator();
    }

    buildRepeatMenu(menu, ui->hbMacroButton, false, &m_hbInterval);

    menu->addSeparator();
    auto now = menu->addAction("Send Heartbeat Now");
    connect(now, &QAction::triggered, this, &UI_Constructor::sendHB);
}

void UI_Constructor::buildCQMenu(QMenu *menu) {
    if (m_cqInterval > 0) {
        auto startStop =
            menu->addAction(ui->cqMacroButton->isChecked() ? "Stop CQ Timer"
                                                           : "Start CQ Timer");
        connect(startStop, &QAction::triggered, this,
                [this]() { ui->cqMacroButton->toggle(); });
        menu->addSeparator();
    }

    buildRepeatMenu(menu, ui->cqMacroButton, true, &m_cqInterval);

    menu->addSeparator();
    auto now = menu->addAction("Send CQ Now");
    connect(now, &QAction::triggered, this, [this]() { sendCQ(false); });
}

void UI_Constructor::buildRepeatMenu(QMenu *menu, QPushButton *button,
                                     bool isLowInterval, int *interval) {
    QList<QPair<QString, int>> items = {
        {"On demand / do not repeat", 0},
        {"Repeat every 1 minute", 1},
        {"Repeat every 5 minutes", 5},
        {"Repeat every 10 minutes", 10},
        {"Repeat every 15 minutes", 15},
        {"Repeat every 30 minutes", 30},
        {"Repeat every 60 minutes", 60},
        {"Repeat every N minutes (Custom Interval)",
         -1}, // this needs to be last because of isSet bool
    };

    if (isLowInterval) {
        items.removeAt(6); // remove the sixty minute interval
        items.removeAt(5); // remove the thirty minute interval
    } else {
        items.removeAt(2); // remove the five minute interval
        items.removeAt(1); // remove the one minute interval
    }

    auto customFormat = QString("Repeat every %1 minutes (Custom Interval)");

    QActionGroup *group = new QActionGroup(menu);

    bool isSet = false;
    foreach (auto pair, items) {
        int minutes = pair.second;
        bool isMatch = *interval == minutes;
        bool isCustom = (minutes == -1 && isSet == false);
        if (isMatch) {
            isSet = true;
        }

        auto text = pair.first;
        if (isCustom) {
            text = QString(customFormat).arg(*interval);
        }

        QAction *action = menu->addAction(text);
        action->setData(minutes);
        action->setCheckable(true);
        action->setChecked(isMatch || isCustom);
        group->addAction(action);

        connect(
            action, &QAction::toggled, this,
            [this, action, customFormat, minutes, interval,
             button](bool checked) {
                int min = minutes;

                if (checked) {

                    if (minutes == -1) {
                        bool ok = false;
                        min =
                            QInputDialog::getInt(this, "Repeat every N minutes",
                                                 "Minutes", 0, 1, 1440, 1, &ok);
                        if (!ok) {
                            return;
                        }
                        action->setText(QString(customFormat).arg(*interval));
                    }

                    *interval = min;

                    if (min > 0) {
                        // force a re-toggle
                        button->setChecked(false);
                    }
                    button->setChecked(min > 0);
                }
            });
    }
}

void UI_Constructor::sendHB() {

    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);

    QStringList parts;
    parts.append(QString("%1:").arg(mycall));

#if JS8_CUSTOMIZE_HB
    auto hb = m_config.hb_message();
#else
    auto hb = QString{};
#endif
    if (hb.isEmpty()) {
        parts.append("HEARTBEAT");
        parts.append(mygrid);
    } else {
        parts.append(hb);
    }

    QString message = parts.join(" ").trimmed();

    auto f = findFreeFreqOffset(500, 1000, 50);

    if (freq() <= 1000) {
        f = freq();
    } else if (m_config.heartbeat_anywhere()) {
        f = -1;
    }

    enqueueMessage(PriorityLow + 1, message, f, nullptr);
    processTxQueue();
}

void UI_Constructor::sendHeartbeatAck(QString to, int snr, QString extra) {
#if JS8_HB_ACK_SNR_CONFIGURABLE
    auto message = m_config.heartbeat_ack_snr()
                       ? QString("%1 SNR %2 %3")
                             .arg(to)
                             .arg(Varicode::formatSNR(snr))
                             .arg(extra)
                             .trimmed()
                       : QString("%1 ACK %2").arg(to).arg(extra).trimmed();
#else
    auto message = QString("%1 HEARTBEAT SNR %2 %3")
                       .arg(to)
                       .arg(Varicode::formatSNR(snr))
                       .arg(extra)
                       .trimmed();
#endif

    auto f =
        m_config.heartbeat_anywhere() ? -1 : findFreeFreqOffset(500, 1000, 50);

    if (m_config.autoreply_confirmation()) {
        confirmThenEnqueueMessage(90, PriorityLow + 1, message, f,
                                  [this]() { processTxQueue(); });
    } else {
        enqueueMessage(PriorityLow + 1, message, f, nullptr);
        processTxQueue();
    }
}

void UI_Constructor::on_hbMacroButton_toggled(bool checked) {
    qCDebug(mainwindow_js8) << "on_hbMacroButton_toggled(" << checked << ")";
    if (checked) {
        // only clear callsign if we do not allow hbs while in qso
        if (m_config.heartbeat_qso_pause()) {
            clearCallsignSelected();
        }

        if (m_hbInterval) {
            if (!m_hb_loop->isActive()) {
                qCDebug(mainwindow_js8)
                    << "Starting HB loop from on_hbMacroButton_toggled()";
                m_hb_loop->onTxLoopPeriodChangeStart(m_hbInterval *
                                                     (qint64)60000);
            }
        } else {
            qCDebug(mainwindow_js8)
                << "Sending single HB from on_hbMacroButton_toggled()";
            m_hb_loop->onLoopCancel();
            // Heartbeat, but not in a loop.
            sendHB();

            // make this button emulate a single press button
            ui->hbMacroButton->setChecked(false);
        }
    } else {
        if (m_hb_loop->isActive() && m_hbButtonIsLongterm) {
            qCDebug(mainwindow_js8)
                << "Stopping HB loop from on_hbMacroButton_toggled()";
            m_hb_loop->onLoopCancel();
        }
    }
    qCDebug(mainwindow_js8)
        << "updateHBButtonDisplay called via on_hbMacroButton_toggled";
    updateHBButtonDisplay();
}

void UI_Constructor::on_hbMacroButton_clicked() {}

void UI_Constructor::sendCQ(bool repeat) {

    if (!repeat && m_cq_loop->isActive()) {
        qCDebug(mainwindow_js8) << "Cancel CQ loop on single-shot CQ";
        m_cq_loop->onLoopCancel();
    }
    if (!repeat && m_hb_loop->isActive()) {
        qCDebug(mainwindow_js8) << "Cancel HB loop on single-shot CQ";
        m_hb_loop->onLoopCancel();
    }

    QString message = m_config.cq_message();
    if (message.isEmpty()) {
        QString mygrid = m_config.my_grid().left(4);
        message = QString("CQ CQ CQ %1").arg(mygrid).trimmed();
    }

    clearCallsignSelected();

    addMessageText(replaceMacros(message, buildMacroValues(), true));

    if (repeat || m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_cqMacroButton_toggled(bool checked) {
    qCDebug(mainwindow_js8) << "on_cqMacroButton_toggled(" << checked << ")";
    if (checked) {
        clearCallsignSelected();

        if (m_cqInterval) {
            qCDebug(mainwindow_js8)
                << "Starting CQ loop from on_cqMacroButton_toggled()";
            m_cq_loop->onTxLoopPeriodChangeStart(m_cqInterval * (qint64)60000);
        } else {
            qCDebug(mainwindow_js8)
                << "Sending single CQ from on_cqMacroButton_toggled()";
            m_cq_loop->onLoopCancel();
            sendCQ(false);

            // make this button emulate a single press button
            ui->cqMacroButton->setChecked(false);
        }
    } else {
        if (m_cq_loop->isActive() && m_cqButtonIsLongterm) {
            qCDebug(mainwindow_js8)
                << "Stopping CQ loop from on_cqMacroButton_toggled()";
            m_cq_loop->onLoopCancel();
        }
    }
    qCDebug(mainwindow_js8)
        << "updateCQButtonDisplay called via on_cqMacroButton_toggled";
    updateCQButtonDisplay();
}

void UI_Constructor::on_cqMacroButton_clicked() {}

void UI_Constructor::on_replyMacroButton_clicked() {
    QString call = callsignSelected();
    if (call.isEmpty()) {
        return;
    }

    auto message = m_config.reply_message();
    message = replaceMacros(message, buildMacroValues(), true);
    addMessageText(QString("%1 %2").arg(call).arg(message));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_snrMacroButton_clicked() {
    QString call = callsignSelected();
    if (call.isEmpty()) {
        return;
    }

    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();
    if (!m_callActivity.contains(call)) {
        return;
    }

    auto cd = m_callActivity[call];
    if (callsignAging && cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
        return;
    }

    auto snr = Varicode::formatSNR(cd.snr);

    addMessageText(QString("%1 SNR %2").arg(call).arg(snr));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_infoMacroButton_clicked() {
    QString info = m_config.my_info();
    if (info.isEmpty()) {
        return;
    }

    addMessageText(
        QString("INFO %1").arg(replaceMacros(info, buildMacroValues(), true)));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::on_statusMacroButton_clicked() {
    QString status = m_config.my_status();
    if (status.isEmpty()) {
        return;
    }

    addMessageText(QString("STATUS %1")
                       .arg(replaceMacros(status, buildMacroValues(), true)));

    if (m_config.transmit_directed())
        toggleTx(true);
}

void UI_Constructor::setShowColumn(QString tableKey, QString columnKey,
                                   bool value) {
    m_showColumnsCache[tableKey + columnKey] = QVariant(value);
    displayBandActivity();
    displayCallActivity();
}

bool UI_Constructor::showColumn(QString tableKey, QString columnKey,
                                bool default_) {
    return m_showColumnsCache.value(tableKey + columnKey, QVariant(default_))
        .toBool();
}

QString UI_Constructor::columnLabel(QString defaultLabel) {
    bool minimalLabels = showColumn("all", "minimal_labels", false);

    // If we are not rendering minimal labels, return the default
    if (!minimalLabels) {
        return defaultLabel;
    }

    // If there is an entry, send it, if not, return default
    return m_columnLabelMap.value(defaultLabel, defaultLabel);
}

void UI_Constructor::buildShowColumnsMenu(QMenu *menu, QString tableKey) {
    QList<QPair<QString, QString>> columnKeys = {
        {"Frequency Offset", "offset"},
        {"Last heard timestamp", "timestamp"},
        {"SNR", "snr"},
        {"Time Delta", "tdrift"},
        {"Mode Speed", "submode"},
    };

    QMap<QString, bool> defaultOverride = {
        {"submode", false},  {"tdrift", false},  {"grid", false},
        {"distance", false}, {"azimuth", false}, {"minimal_labels", false}};

    if (tableKey == "call") {
        columnKeys.prepend({"Callsign", "callsign"});
        columnKeys.append({
            {"Grid Locator", "grid"},
            {"Distance", "distance"},
            {"Azimuth", "azimuth"},
            {"Worked Before", "log"},
            {"Logged Name", "logName"},
            {"Logged Comment", "logComment"},
        });
    }

    columnKeys.prepend({"Minimal Column Labels", "minimal_labels"});
    columnKeys.prepend({"Show Column Labels", "labels"});

    int columnIndex = 0;
    QString origTableKey = tableKey;
    foreach (auto p, columnKeys) {
        auto columnLabel = p.first;
        auto columnKey = p.second;

        auto a = menu->addAction(columnLabel);
        a->setCheckable(true);

        // Add separator after second item
        // If this is the second item, it is the minimal labels item, so set the
        // table key to all
        if (++columnIndex == 2) {
            tableKey = "all";
            menu->addSeparator();
        }

        bool showByDefault = true;
        if (defaultOverride.contains(columnKey)) {
            showByDefault = defaultOverride[columnKey];
        }
        a->setChecked(showColumn(tableKey, columnKey, showByDefault));

        connect(a, &QAction::triggered, this, [this, a, tableKey, columnKey]() {
            setShowColumn(tableKey, columnKey, a->isChecked());
        });

        // If we have switched to a custom table key in this iteration, reset to
        // the original key
        if (tableKey != origTableKey) {
            tableKey = origTableKey;
        }
    }
}

void UI_Constructor::setSortBy(QString key, QString value) {
    m_sortCache[key] = QVariant(value);
    displayBandActivity();
    displayCallActivity();
}

QString UI_Constructor::getSortBy(QString const &key,
                                  QString const &defaultValue) const {
    return m_sortCache.value(key, QVariant(defaultValue)).toString();
}

UI_Constructor::SortByReverse
UI_Constructor::getSortByReverse(QString const &key,
                                 QString const &defaultValue) const {
    auto const sortBy = getSortBy(key, defaultValue);
    auto const reverse = sortBy.startsWith("-");

    return {reverse ? sortBy.sliced(1) : sortBy, reverse};
}

void UI_Constructor::buildSortByMenu(QMenu *menu, QString key,
                                     QString defaultValue,
                                     QList<QPair<QString, QString>> values) {
    auto currentSortBy = getSortBy(key, defaultValue);

    QActionGroup *g = new QActionGroup(menu);
    g->setExclusive(true);

    foreach (auto p, values) {
        auto k = p.first;
        auto v = p.second;
        auto a = menu->addAction(k);
        a->setCheckable(true);
        a->setChecked(v == currentSortBy);
        a->setActionGroup(g);

        connect(a, &QAction::triggered, this, [this, a, key, v]() {
            if (a->isChecked()) {
                setSortBy(key, v);
            }
        });
    }
}

void UI_Constructor::buildBandActivitySortByMenu(QMenu *menu) {
    buildSortByMenu(menu, "bandActivity", "offset",
                    {{"Frequency offset", "offset"},
                     {"Last heard timestamp (oldest first)", "timestamp"},
                     {"Last heard timestamp (recent first)", "-timestamp"},
                     {"SNR (weakest first)", "snr"},
                     {"SNR (strongest first)", "-snr"},
                     {"Mode Speed (slowest first)", "submode"},
                     {"Mode Speed (fastest first)", "-submode"}});
}

void UI_Constructor::buildCallActivitySortByMenu(QMenu *menu) {
    buildSortByMenu(menu, "callActivity", "callsign",
                    {{"Callsign", "callsign"},
                     {"Callsigns Replied (recent first)", "ackTimestamp"},
                     {"Frequency offset", "offset"},
                     {"Distance (closest first)", "distance"},
                     {"Distance (farthest first)", "-distance"},
                     {"Azimuth", "azimuth"},
                     {"Last heard timestamp (oldest first)", "timestamp"},
                     {"Last heard timestamp (recent first)", "-timestamp"},
                     {"SNR (weakest first)", "snr"},
                     {"SNR (strongest first)", "-snr"},
                     {"Mode Speed (slowest first)", "submode"},
                     {"Mode Speed (fastest first)", "-submode"}});
}

void UI_Constructor::buildRelayMenu(QMenu *menu) {
    auto now = DriftingDateTime::currentDateTimeUtc();
    int callsignAging = m_config.callsign_aging();
    foreach (auto cd, m_callActivity.values()) {
        if (callsignAging &&
            cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
            continue;
        }

        menu->addAction(buildRelayAction(cd.call));
    }
}

QAction *UI_Constructor::buildRelayAction(QString call) {
    QAction *a = new QAction(call, nullptr);
    connect(a, &QAction::triggered, this,
            [this, call]() { prependMessageText(QString("%1>").arg(call)); });
    return a;
}

void UI_Constructor::buildEditMenu(QMenu *menu, QTextEdit *edit) {
    bool hasSelection = !edit->textCursor().selectedText().isEmpty();

    auto cut = menu->addAction("Cu&t");
    cut->setEnabled(hasSelection && !edit->isReadOnly());
    connect(edit, &QTextEdit::copyAvailable, this,
            [edit, cut](bool copyAvailable) {
                cut->setEnabled(copyAvailable && !edit->isReadOnly());
            });
    connect(cut, &QAction::triggered, this, [edit]() {
        edit->copy();
        edit->textCursor().removeSelectedText();
    });

    auto copy = menu->addAction("&Copy");
    copy->setEnabled(hasSelection);
    connect(edit, &QTextEdit::copyAvailable, this,
            [copy](bool copyAvailable) { copy->setEnabled(copyAvailable); });
    connect(copy, &QAction::triggered, edit, &QTextEdit::copy);

    auto paste = menu->addAction("&Paste");
    paste->setEnabled(edit->canPaste());
    connect(paste, &QAction::triggered, edit, &QTextEdit::paste);
}

QMap<QString, QString> UI_Constructor::buildMacroValues() {
    auto lastActive =
        DriftingDateTime::currentDateTimeUtc().addSecs(-m_idleMinutes * 60);
    QString myIdle = since(lastActive).toUpper().replace("NOW", "0M");
    QString myVersion = version();

    QMap<QString, QString> values = {
        {"<MYCALL>", m_config.my_callsign()},
        {"<MYGRID4>", m_config.my_grid().left(4)},
        {"<MYGRID12>", m_config.my_grid().left(12)},
        {"<MYINFO>", m_config.my_info()},
        {"<MYHB>", m_config.hb_message()},
        {"<MYCQ>", m_config.cq_message()},
        {"<MYREPLY>", m_config.reply_message()},
        {"<MYSTATUS>", m_config.my_status()},

        {"<MYVERSION>", myVersion},
        {"<MYIDLE>", myIdle},
    };

    auto selectedCall = callsignSelected();
    if (m_callActivity.contains(selectedCall)) {
        auto cd = m_callActivity[selectedCall];

        values["<CALL>"] = selectedCall;
        values["<TDELTA>"] = QString("%1 ms").arg((int)(1000 * cd.tdrift));

        if (cd.snr > -31) {
            values["<SNR>"] = Varicode::formatSNR(cd.snr);
        }
    }

    // these macros can have recursive macros
    values["<MYINFO>"] = replaceMacros(values["<MYINFO>"], values, false);
    values["<MYSTATUS>"] = replaceMacros(values["<MYSTATUS>"], values, false);
    values["<MYCQ>"] = replaceMacros(values["<MYCQ>"], values, false);
    values["<MYHB>"] = replaceMacros(values["<MYHB>"], values, false);
    values["<MYREPLY>"] = replaceMacros(values["<MYREPLY>"], values, false);

    return values;
}

void UI_Constructor::buildColumnLabelMap() {
    // This is the map of full-length strings to shortened versions
    // Add new minimal labels here as needed
    m_columnLabelMap = {{"Callsigns", "Call"}, {"Callsigns (%1)", "Call(%1)"},
                        {"Offset", "Off"},     {"SNR", "SN"},
                        {"Time Delta", "TD"},  {"Speed", "Sp"},
                        {"Distance", "Dist"},  {"Azimuth", "Az"},
                        {"%1 ms", "%1"},       {"%1 dB", "%1"},
                        {"%1 Hz", "%1"}};

    // Populate original header maps
    int cols = ui->tableWidgetRXAll->columnCount();
    for (int c = 0; c < cols; ++c) {
        QString label = ui->tableWidgetRXAll->horizontalHeaderItem(c)->text();

        m_origRxHeaderLabelMap[c] = label;
    }

    cols = ui->tableWidgetCalls->columnCount();
    for (int c = 0; c < cols; ++c) {
        QString label = ui->tableWidgetCalls->horizontalHeaderItem(c)->text();

        m_origCallActivityHeaderLabelMap[c] = label;
    }
}

void UI_Constructor::buildSuggestionsMenu(QMenu *menu, QTextEdit *edit,
                                          const QPoint &point) {
    if (!m_config.spellcheck()) {
        return;
    }

    bool found = false;

    auto c = edit->cursorForPosition(point);
    if (c.charFormat().underlineStyle() != QTextCharFormat::WaveUnderline) {
        return;
    }

    c.movePosition(QTextCursor::StartOfWord);
    c.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);

    auto word = c.selectedText().toUpper().trimmed();
    if (word.isEmpty()) {
        return;
    }

    QStringList suggestions = JSCChecker::suggestions(word, 5, &found);
    if (suggestions.isEmpty() && !found) {
        return;
    }

    if (suggestions.isEmpty()) {
        auto a = menu->addAction("No Suggestions");
        a->setDisabled(true);
    } else {
        foreach (auto suggestion, suggestions) {
            auto a = menu->addAction(suggestion);

            connect(a, &QAction::triggered, this, [edit, point, suggestion]() {
                auto c = edit->cursorForPosition(point);
                c.select(QTextCursor::WordUnderCursor);
                c.insertText(suggestion);
            });
        }
    }

    menu->addSeparator();
}

void UI_Constructor::buildSavedMessagesMenu(QMenu *menu) {
    auto values = buildMacroValues();

    foreach (QString macro, m_config.macros()->stringList()) {
        QAction *action = menu->addAction(replaceMacros(macro, values, false));
        connect(action, &QAction::triggered, this, [this, macro]() {
            auto values = buildMacroValues();
            addMessageText(replaceMacros(macro, values, true));

            if (m_config.transmit_directed())
                toggleTx(true);
        });
    }

    menu->addSeparator();

    auto editAction = new QAction(QString("&Edit Saved Messages"), menu);
    menu->addAction(editAction);
    connect(editAction, &QAction::triggered, this,
            [this]() { openSettings(5); });

    auto saveAction = new QAction(QString("&Save Current Message"), menu);
    saveAction->setDisabled(ui->extFreeTextMsgEdit->toPlainText().isEmpty());
    menu->addAction(saveAction);
    connect(saveAction, &QAction::triggered, this, [this]() {
        auto macros = m_config.macros();
        if (macros->insertRow(macros->rowCount())) {
            auto index = macros->index(macros->rowCount() - 1);
            macros->setData(index, ui->extFreeTextMsgEdit->toPlainText());
            writeSettings();
        }
    });
}

void UI_Constructor::on_queryButton_pressed() {
    QMenu *menu = ui->queryButton->menu();
    if (!menu) {
        menu = new QMenu(ui->queryButton);
    }
    menu->clear();

    buildQueryMenu(menu, callsignSelected());

    ui->queryButton->setMenu(menu);
    ui->queryButton->showMenu();
}

void UI_Constructor::on_macrosMacroButton_pressed() {
    QMenu *menu = ui->macrosMacroButton->menu();
    if (!menu) {
        menu = new QMenu(ui->macrosMacroButton);
    }
    menu->clear();

    buildSavedMessagesMenu(menu);

    ui->macrosMacroButton->setMenu(menu);
    ui->macrosMacroButton->showMenu();
}

void UI_Constructor::on_deselectButton_pressed() { clearCallsignSelected(); }

void UI_Constructor::on_tableWidgetRXAll_cellClicked(int /*row*/, int /*col*/) {
    ui->tableWidgetCalls->selectionModel()->select(
        ui->tableWidgetCalls->selectionModel()->selection(),
        QItemSelectionModel::Deselect);

    displayCallActivity();
}

void UI_Constructor::on_tableWidgetRXAll_cellDoubleClicked(int row, int col) {
    on_tableWidgetRXAll_cellClicked(row, col);

    // TODO: jsherer - could also parse the messages for the last callsign?
    auto item = ui->tableWidgetRXAll->item(row, 0);
    int offset = item->text().replace(" Hz", "").toInt();

    // switch to the offset of this row
    changeFreq(offset);

    // TODO: prompt mode switch?

    // print the history in the main window...
    int activityAging = m_config.activity_aging();
    QDateTime now = DriftingDateTime::currentDateTimeUtc();
    QDateTime firstActivity = now;
    QString activityText;
    bool isLast = false;
    foreach (auto d, m_bandActivity[offset]) {
        if (activityAging && d.utcTimestamp.secsTo(now) / 60 >= activityAging) {
            continue;
        }
        if (activityText.isEmpty()) {
            firstActivity = d.utcTimestamp;
        }
        activityText.append(d.text);

        isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;
        if (isLast) {
            activityText = QString("%1 %2 ")
                               .arg(Varicode::rstrip(activityText))
                               .arg(m_config.eot());
        }
    }
    if (!activityText.isEmpty()) {
        displayTextForFreq(activityText, offset, firstActivity, false, true,
                           isLast);
    }
}

QString UI_Constructor::generateCallDetail(QString selectedCall) {
    if (selectedCall.isEmpty()) {
        return "";
    }

    // heard detail
    QString hearing =
        m_heardGraphOutgoing.value(selectedCall).values().join(", ");
    QString heardby =
        m_heardGraphIncoming.value(selectedCall).values().join(", ");
    QStringList detail = {
        QString("<h1>%1</h1>").arg(selectedCall.toHtmlEscaped()),
        hearing.isEmpty() ? ""
                          : QString("<p><strong>HEARING</strong>: %1</p>")
                                .arg(hearing.toHtmlEscaped()),
        heardby.isEmpty() ? ""
                          : QString("<p><strong>HEARD BY</strong>: %1</p>")
                                .arg(heardby.toHtmlEscaped()),
    };

    return detail.join("\n");
}

void UI_Constructor::on_tableWidgetCalls_cellClicked(int /*row*/, int /*col*/) {
    ui->tableWidgetRXAll->selectionModel()->select(
        ui->tableWidgetRXAll->selectionModel()->selection(),
        QItemSelectionModel::Deselect);

    displayBandActivity();
}

void UI_Constructor::on_tableWidgetCalls_cellDoubleClicked(int row, int col) {
    on_tableWidgetCalls_cellClicked(row, col);

    auto call = callsignSelected();
    addMessageText(call);

#if SHOW_MESSAGE_HISTORY_ON_DOUBLECLICK
    if (m_rxInboxCountCache.value(call, 0) > 0) {

        // TODO:
        // CommandDetail d = m_rxCallsignInboxCountCache[call].first();
        // m_rxCallsignInboxCountCache[call].removeFirst();
        //
        // processAlertReplyForCommand(d, d.relayPath, d.cmd);

        Inbox i(inboxPath());
        if (i.open()) {
            QList<Message> msgs;
            foreach (auto pair,
                     i.values("UNREAD", "$.params.FROM", call, 0, 1000)) {
                msgs.append(pair.second);
            }

            auto mp = new MessagePanel(this);
            mp->populateMessages(msgs);
            mp->show();

            ensureMessageDock();

            messageDock_->show();
            messageDock_->raise();

            auto pair = i.firstUnreadFrom(call);
            auto id = pair.first;
            auto msg = pair.second;
            auto params = msg.params();

            CommandDetail d;
            d.cmd = params.value("CMD").toString();
            d.extra = params.value("EXTRA").toString();
            d.freq = params.value("OFFSET").toInt();
            d.from = params.value("FROM").toString();
            d.grid = params.value("GRID").toString();
            d.relayPath = params.value("PATH").toString();
            d.snr = params.value("SNR").toInt();
            d.tdrift = params.value("TDRIFT").toFloat();
            d.text = params.value("TEXT").toString();
            d.to = params.value("TO").toString();
            d.utcTimestamp = QDateTime::fromString(
                params.value("UTC").toString(), "yyyy-MM-dd hh:mm:ss");
            d.utcTimestamp.setUtcOffset(0);

            msg.setType("READ");
            i.set(id, msg);

            m_rxInboxCountCache[call] =
                max(0, m_rxInboxCountCache.value(call) - 1);

            processAlertReplyForCommand(d, d.relayPath, d.cmd);
        }

    } else {
        addMessageText(call);
    }
#endif
}

void UI_Constructor::on_tuneButton_clicked(bool checked) {
    static bool lastChecked = false;
    if (lastChecked == checked)
        return;
    lastChecked = checked;
    if (checked && m_tune == false) { // we're starting tuning so remember Tx
                                      // and change pwr to Tune value
        if (m_config.pwrBandTuneMemory()) {
            m_pwrBandTxMemory[m_lastBand] =
                ui->outAttenuation->value(); // remember our Tx pwr
            m_PwrBandSetOK = false;
            if (m_pwrBandTuneMemory.contains(m_lastBand)) {
                ui->outAttenuation->setValue(
                    m_pwrBandTuneMemory[m_lastBand].toInt()); // set to Tune pwr
            }
            m_PwrBandSetOK = true;
        }
    }
    if (m_tune) {
        tuneButtonTimer.start(250);
    } else {
        itone[0] = 0;
        on_monitorButton_clicked(true);
        m_tune = true;
    }
    Q_EMIT tune(checked);
}

void UI_Constructor::end_tuning() {
    tuneATU_Timer.stop(); // stop tune watchdog when stopping Tune manually
    on_stopTxButton_clicked();
    // we're turning off so remember our Tune pwr setting and reset to Tx pwr
    if (m_config.pwrBandTuneMemory() || m_config.pwrBandTxMemory()) {
        m_pwrBandTuneMemory[m_lastBand] =
            ui->outAttenuation->value(); // remember our Tune pwr
        m_PwrBandSetOK = false;
        ui->outAttenuation->setValue(
            m_pwrBandTxMemory[m_lastBand].toInt()); // set to Tx pwr
        m_PwrBandSetOK = true;
    }
}

void UI_Constructor::stop_tuning() {
    tuneATU_Timer.stop(); // stop tune watchdog when stopping Tune manually
    on_tuneButton_clicked(false);
    ui->tuneButton->setChecked(false);
    m_isTimeToSend = false;
    m_tune = false;
}

void UI_Constructor::stopTuneATU() {
    on_tuneButton_clicked(false);
    m_isTimeToSend = false;
}

void UI_Constructor::resetPushButtonToggleText(QPushButton *btn) {
    bool checked = btn->isChecked();
    auto style = btn->styleSheet();
    if (checked) {
        style = style.replace("font-weight:normal;", "font-weight:bold;");
    } else {
        style = style.replace("font-weight:bold;", "font-weight:normal;");
    }
    btn->setStyleSheet(style);

#if PUSH_BUTTON_CHECKMARK
    auto on = "✓ ";
    auto text = btn->text();
    if (checked) {
        btn->setText(on + text.replace(on, ""));
    } else {
        btn->setText(text.replace(on, ""));
    }
#endif

#if PUSH_BUTTON_MIN_WIDTH
    int width = 0;
    QList<QPushButton *> btns;
    foreach (auto child, ui->buttonGrid->children()) {
        if (!child->isWidgetType()) {
            continue;
        }

        if (!child->objectName().contains("Button")) {
            continue;
        }

        auto b = qobject_cast<QPushButton *>(child);
        width = qMax(width, b->geometry().width());
        btns.append(b);
    }

    foreach (auto child, btns) {
        child->setMinimumWidth(width);
    }
#endif
}

void UI_Constructor::on_stopTxButton_clicked() // Stop Tx
{
    if (m_tune)
        stop_tuning();
    if (m_auto and !m_tuneup)
        auto_tx_mode(false);
    m_btxok = false;

    resetMessage();

    if (m_stopTxButtonIsLongterm) {
        m_hb_loop->onLoopCancel();
        m_cq_loop->onLoopCancel();
    }
}

void UI_Constructor::rigOpen() {
    update_dynamic_property(ui->readFreq, "state", "warning");
    ui->readFreq->setText("CAT");
    ui->readFreq->setEnabled(true);
    m_config.transceiver_online();
    Q_EMIT m_config.sync_transceiver(true, true);
}

void UI_Constructor::on_readFreq_clicked() {
    if (m_transmitting)
        return;

    if (m_config.transceiver_online()) {
        Q_EMIT m_config.sync_transceiver(true, true);
    }
}

void UI_Constructor::setXIT(int audio_freq) {
    if (m_transmitting && !m_config.tx_qsy_allowed()) {
        qCWarning(mainwindow_js8) << "Ignoring change of audio freq to"
                                  << audio_freq << "as currently transmitting.";
        return;
    }

    // m_XIT is the frequency diff that will be added to the audio frequency
    // and subtracted from the radio frequency.
    // The new audio frequency is in the 1500 - 2000 Hz range,
    // the audio actually transmitted is 1500 - 2160 Hz (JS8 40 has 160 Hz
    // bandwith). This way, the unwanted triple audio frequency possibly
    // generated by audio distortions is safely beyond the TX audio bandwidth of
    // 3 kHz and will not result in transmission. Also, the 1500 - 2160 Hz range
    // should not be distorted or dampened by the TX's audio filter.
    if (m_config.split_mode()) {
        const int next_lower_multiple_of_500 = audio_freq - audio_freq % 500;
        m_XIT = 1500 - next_lower_multiple_of_500;
    } else {
        m_XIT = 0;
    }

    const int new_audio_frequency = audio_freq + m_XIT;

    if ((m_monitoring || m_transmitting) && m_config.is_transceiver_online() &&
        m_config.split_mode()) {
        // All conditions are met, reset the transceiver Tx dial frequency
        m_freqTxNominal = m_freqNominal - m_XIT;
        qCDebug(mainwindow_js8)
            << "For incoming AF" << audio_freq << "setting tx HF to"
            << m_freqTxNominal << "and new AF to" << new_audio_frequency;
        Q_EMIT m_config.transceiver_tx_frequency(m_freqTxNominal);
    }

    // Now set the audio Tx freq
    Q_EMIT transmitFrequency(new_audio_frequency);
}

void UI_Constructor::qsy(int const hzDelta) {
    setRig(m_freqNominal + hzDelta);
    changeFreq(m_wideGraph->centerFreq());

    // Adjust band activity frequencies.

    BandActivity bandActivity;

    for (auto [key, value] : m_bandActivity.asKeyValueRange()) {
        if (value.isEmpty())
            continue;

        auto const newKey = key - hzDelta;

        bandActivity[newKey] = value;
        bandActivity[newKey].last().offset -= hzDelta;
    }

    m_bandActivity.swap(bandActivity);

    // Adjust call activity frequencies.

    for (auto [key, value] : m_callActivity.asKeyValueRange()) {
        value.offset -= hzDelta;
    }

    displayActivity(true);
}

void UI_Constructor::onDriftChanged(qint64 /*new_drift_ms*/) {
    // here we reset the buffer position without clearing the buffer
    // this makes the detected emit the correct k when drifting time
    qCDebug(mainwindow_js8) << "Processing drift change.";
    m_detector->resetBufferPosition();
}

bool UI_Constructor::tryRestoreFreqOffset() {
    if (m_sliderFreqBeforeHB == 0) return false;
    int restoreFreq = m_sliderFreqBeforeHB;
    m_sliderFreqBeforeHB = 0;
    changeFreq(restoreFreq);
    return true;
}

void UI_Constructor::changeFreq(int const newFreq) {
    // Don't allow QSY if we've already queued a transmission,
    // unless we have that functionality enabled.

    if (isMessageQueuedForTransmit() && !m_config.tx_qsy_allowed())
        return;

    // TODO: jsherer - here's where we'd set minimum frequency again (later?)

    setFreq(std::max(0, newFreq));

    displayDialFrequency();
}

void UI_Constructor::handle_transceiver_update(
    Transceiver::TransceiverState const &new_rig_state) {
    qCDebug(mainwindow_js8)
        << "UI_Constructor::handle_transceiver_update:" << new_rig_state;
    Transceiver::TransceiverState old_state{m_rigState};

    // GM8JCF: in stopTx2 we maintain PTT if there are still untransmitted JS8
    // frames and we are holding the PTT KN4CRD: if we're not holding the PTT we
    // need to check to ensure it's safe to transmit
    if (m_config.hold_ptt() ||
        (new_rig_state.ptt() &&
         !m_rigState
              .ptt())) // safe to start audio (caveat - DX Lab Suite Commander)
    {
        if (m_generateAudioWhenPttConfirmedByTX &&
            m_iptt) // waiting to Tx and still needed
        {
            // The Modulator nicely emits silence during txDelay, so let us just
            // tigger it.
            transmit();
        }
        m_generateAudioWhenPttConfirmedByTX = false;
    }
    m_rigState = new_rig_state;

    auto old_freqNominal = m_freqNominal;
    if (!old_freqNominal) {
        // always take initial rig frequency to avoid start up problems
        // with bogus Tx frequencies
        m_freqNominal = new_rig_state.frequency();
    }

    if (old_state.online() == false && new_rig_state.online() == true) {
        // initializing
        on_monitorButton_clicked(!m_config.monitor_off_at_startup());
        on_monitorTxButton_toggled(!m_config.transmit_off_at_startup());
    }

    if (new_rig_state.frequency() != old_state.frequency() ||
        new_rig_state.split() != m_splitMode) {
        m_splitMode = new_rig_state.split();
        if (!new_rig_state.ptt()) {
            m_freqNominal = new_rig_state.frequency();
            if (old_freqNominal != m_freqNominal) {
                m_freqTxNominal = m_freqNominal;
            }

            if (m_monitoring) {
                m_lastMonitoredFrequency = m_freqNominal;
            }
            if (m_lastDialFreq != m_freqNominal) {

                m_lastDialFreq = m_freqNominal;
                m_secBandChanged =
                    DriftingDateTime::currentMSecsSinceEpoch() / 1000;

                if (m_freqNominal != m_bandHoppedFreq) {
                    m_bandHopped = false;
                }

                if (new_rig_state.frequency() < 30000000u) {
                    write_frequency_entry("ALL.TXT");
                }

                if (m_config.spot_to_reporting_networks()) {
                    spotSetLocal();
                    pskSetLocal();
                    if (m_config.spot_to_aprs() ||
                        m_config.spot_to_aprs_relay()) {
                        aprsSetLocal();
                    }
                }
                statusChanged();
                m_wideGraph->setDialFreq(m_freqNominal / 1.e6f);
            }
        } else {
            m_freqTxNominal = new_rig_state.split()
                                  ? new_rig_state.tx_frequency()
                                  : new_rig_state.frequency();
        }
    }

    // ensure frequency display is correct
    // setRig();
    updateCurrentBand();
    displayDialFrequency();
    update_dynamic_property(ui->readFreq, "state", "ok");
    ui->readFreq->setEnabled(false);
    ui->readFreq->setText(new_rig_state.split() ? "CAT/S" : "CAT");
}

void UI_Constructor::handle_transceiver_failure(QString const &reason) {
    update_dynamic_property(ui->readFreq, "state", "error");
    ui->readFreq->setEnabled(true);
    on_stopTxButton_clicked();
    rigFailure(reason);
}

void UI_Constructor::rigFailure(QString const &reason) {
    if (m_first_error) {
        // one automatic retry
        QTimer::singleShot(0, this, &UI_Constructor::rigOpen);
        m_first_error = false;
    } else {
        m_rigErrorMessageBox.setDetailedText(reason);

        // don't call slot functions directly to avoid recursion
        m_rigErrorMessageBox.exec();
        auto const clicked_button = m_rigErrorMessageBox.clickedButton();
        if (clicked_button == m_configurations_button) {
            ui->menuConfig->exec(QCursor::pos());
        } else {
            switch (m_rigErrorMessageBox.standardButton(clicked_button)) {
            case JS8MessageBox::Ok:
                m_config.select_tab(1);
                QTimer::singleShot(
                    0, this, &UI_Constructor::on_actionSettings_triggered);
                break;

            case JS8MessageBox::Retry:
                QTimer::singleShot(0, this, &UI_Constructor::rigOpen);
                break;

            case JS8MessageBox::Cancel:
                QTimer::singleShot(0, this, &UI_Constructor::close);
                break;

            default:
                break; // squashing compile warnings
            }
        }
        m_first_error = true; // reset
    }
}

void UI_Constructor::on_outAttenuation_valueChanged(int const a) {
    if (m_PwrBandSetOK) {
        if (!m_tune && m_config.pwrBandTxMemory())
            m_pwrBandTxMemory[m_lastBand] = a; // remember our Tx pwr
        if (m_tune && m_config.pwrBandTuneMemory())
            m_pwrBandTuneMemory[m_lastBand] = a; // remember our Tune pwr
    }

    // Slider interpreted as dB / 100.

    Q_EMIT outAttenuationChanged(a / 10.0);
}

void UI_Constructor::spotSetLocal() {
    Q_EMIT spotClientSetLocalStation(
        m_config.my_callsign(), m_config.my_grid(),
        replaceMacros(m_config.my_info(), buildMacroValues(), true));
}

void UI_Constructor::pskSetLocal() {
    Q_EMIT pskReporterSetLocalStation(
        m_config.my_callsign(), m_config.my_grid(),
        replaceMacros(m_config.my_info(), buildMacroValues(), true));
}

void UI_Constructor::aprsSetLocal() {
    Q_EMIT aprsClientSetLocalStation(
        "APJ8CL", QString::number(APRSISClient::hashCallsign("APJ8CL")));
}

void UI_Constructor::transmitDisplay(bool transmitting) {
    if (transmitting == m_transmitting) {
        if (transmitting) {
            ui->signal_meter_widget->setValue(0, 0);
            if (m_monitoring)
                monitor(false);
            m_btxok = true;
        }
    }

    updateTxButtonDisplay();
}

void UI_Constructor::postDecode(bool is_new, QString const &) {
#if 0
  auto const& decode = message.trimmed ();
  auto const& parts = decode.left (22).split (' ', QString::SkipEmptyParts);
  if (parts.size () >= 5)
  {
      auto has_seconds = parts[0].size () > 4;
      m_messageClient->decode (is_new
                               , QTime::fromString (parts[0], has_seconds ? "hhmmss" : "hhmm")
                               , parts[1].toInt ()
                               , parts[2].toFloat (), parts[3].toUInt (), parts[4]
                               , decode.mid (has_seconds ? 24 : 22, 21)
                               , QChar {'?'} == decode.mid (has_seconds ? 24 + 21 : 22 + 21, 1)
                               , m_diskData);
  }
#endif

    if (is_new) {
        m_rxDirty = true;
    }
}

void UI_Constructor::tryNotify(QString const &key) {
    // During configuration dialog, use live UI state
    auto const path = m_config.is_active()
        ? m_config.test_notification_path(key)
        : m_config.notification_path(key);
    if (!path.isEmpty()) {
        emit playNotification(path);
    }
}

void UI_Constructor::displayTransmit() {
    // Transmit Activity
    update_dynamic_property(ui->startTxButton, "transmitting", m_transmitting);
    update_dynamic_property(ui->monitorTxButton, "transmitting",
                            m_transmitting);
}

bool UI_Constructor::presentlyWantHBReplies() {
    return ui->actionModeAutoreply->isChecked() &&
           ui->actionHeartbeatAcknowledgements->isChecked() &&
           m_messageBuffer.isEmpty() &&
           (!m_config.heartbeat_qso_pause() ||
            m_prevSelectedCallsign.isEmpty());
}

void UI_Constructor::updateModeButtonText() {
    auto multi = ui->actionModeMultiDecoder->isChecked();
    auto autoreply = ui->actionModeAutoreply->isChecked();
    auto heartbeat =
        ui->actionModeJS8HB->isEnabled() && ui->actionModeJS8HB->isChecked();

    auto modeText = JS8::Submode::name(m_nSubMode);
    if (multi) {
        modeText += QString("+MULTI");
    }

    if (autoreply) {
        if (m_config.autoreply_confirmation()) {
            modeText += QString("+AUTO+CONF");
        } else {
            modeText += QString("+AUTO");
        }
    }

    if (heartbeat) {
        if (presentlyWantHBReplies()) {
            modeText += QString("+HB+ACK");
        } else {
            modeText += QString("+HB");
        }
    }

    ui->modeButton->setText(modeText);
    {
        QString modeLabelText;
        switch (m_nSubMode) {
        case Varicode::JS8CallSlow:
            modeLabelText = "JS8 Slow";
            break;
        case Varicode::JS8CallNormal:
            modeLabelText = "JS8 Normal";
            break;
        case Varicode::JS8CallFast:
            modeLabelText = "JS8 Fast";
            break;
        case Varicode::JS8CallTurbo:
            modeLabelText = "JS8 40";
            break;
        case Varicode::JS8CallUltra:
            modeLabelText = "JS8 60";
            break;
        default:
            modeLabelText = "JS8";
            break;
        }
        mode_label.setText(modeLabelText);
    }
}

void UI_Constructor::updateButtonDisplay() {
    bool isTransmitting = isMessageQueuedForTransmit();

    auto selectedCallsign = callsignSelected(true);
    bool emptyCallsign = selectedCallsign.isEmpty();
    bool emptyInfo = m_config.my_info().isEmpty();
    bool emptyStatus = m_config.my_status().isEmpty();

    bool previous_hbButtonisLongterm = m_hbButtonIsLongterm;
    m_hbButtonIsLongterm = false;
    ui->hbMacroButton->setDisabled(isTransmitting);
    m_hbButtonIsLongterm = previous_hbButtonisLongterm;

    bool previous_cqButtonisLongterm = m_cqButtonIsLongterm;
    m_cqButtonIsLongterm = false;
    ui->cqMacroButton->setDisabled(isTransmitting);
    m_cqButtonIsLongterm = previous_cqButtonisLongterm;

    ui->replyMacroButton->setDisabled(isTransmitting || emptyCallsign);
    ui->snrMacroButton->setDisabled(isTransmitting || emptyCallsign);
    ui->infoMacroButton->setDisabled(isTransmitting || emptyInfo);
    ui->statusMacroButton->setDisabled(isTransmitting || emptyStatus);
    ui->macrosMacroButton->setDisabled(isTransmitting);
    ui->queryButton->setDisabled(isTransmitting || emptyCallsign);
    ui->deselectButton->setDisabled(isTransmitting || emptyCallsign);
    ui->queryButton->setText(
        emptyCallsign ? "Directed"
                      : QString("Directed to %1").arg(selectedCallsign));

    // update mode button text
    updateModeButtonText();
}

void UI_Constructor::updateHBButtonDisplay() {
    if (m_hb_loop->isActive()) {
        QDateTime now = DriftingDateTime::currentDateTimeUtc();
        QDateTime nextHeartbeat = m_hb_loop->nextActivity();
        long secs = std::lround(now.msecsTo(nextHeartbeat) / 1000.0);

        QString hbBase = presentlyWantHBReplies() ? "HB + ACK" : "HB";

        if (secs > 0) {
            ui->hbMacroButton->setText(
                QString("%1 (%2)").arg(hbBase).arg(secs));
        } else {
            // Dead code?
            ui->hbMacroButton->setText(QString("%1 (now)").arg(hbBase));
        }
    } else {
        if (presentlyWantHBReplies()) {
            ui->hbMacroButton->setText("HB + ACK");
        } else {
            ui->hbMacroButton->setText("HB");
        }
    }
}

void UI_Constructor::updateCQButtonDisplay() {
    if (m_cq_loop->isActive()) {
        QDateTime now = DriftingDateTime::currentDateTimeUtc();
        QDateTime nextCQ = m_cq_loop->nextActivity();
        long secs = std::lround(now.msecsTo(nextCQ) / 1000.0);
        // qCDebug(mainwindow_js8)
        //         << "updateCQButtonDisplay, signal due at" << nextCQ
        //         << "so" << secs << "s to go";
        if (secs > 0) {
            ui->cqMacroButton->setText(QString("CQ (%1)").arg(secs));
        } else {
            // Dead code?
            ui->cqMacroButton->setText("CQ (now)");
        }
    } else {
        ui->cqMacroButton->setText("CQ");
        // qCDebug(mainwindow_js8) << "updateCQButtonDisplay while m_cq_loop is
        // off";
    }
}

void UI_Constructor::updateTextDisplay() {
    bool canTransmit = ensureCanTransmit();
    bool isTransmitting = isMessageQueuedForTransmit();
    bool emptyText = ui->extFreeTextMsgEdit->toPlainText().isEmpty();

    ui->startTxButton->setDisabled(!canTransmit || isTransmitting || emptyText);

    if (m_txTextDirty) {
        // debounce frame and word count
        if (m_txTextDirtyDebounce.isActive()) {
            m_txTextDirtyDebounce.stop();
        }
        m_txTextDirtyDebounce.setSingleShot(true);
        m_txTextDirtyDebounce.start(100);
        m_txTextDirty = false;
    }
}

#if __APPLE__
#define USE_SYNC_FRAME_COUNT 0
#else
#define USE_SYNC_FRAME_COUNT 0
#endif

void UI_Constructor::refreshTextDisplay() {
    qCDebug(mainwindow_js8) << "refreshing text display...";
    auto text = ui->extFreeTextMsgEdit->toPlainText();

#if USE_SYNC_FRAME_COUNT
    auto frames = buildMessageFrames(text);

    QStringList textList;
    qCDebug(mainwindow_js8) << "frames:";
    foreach (auto frame, frames) {
        auto dt = DecodedText(frame.first, frame.second);
        qCDebug(mainwindow_js8) << "->" << frame << dt.message()
                                << Varicode::frameTypeString(dt.frameType());
        textList.append(dt.message());
    }

    auto transmitText = textList.join("");
    auto count = frames.length();

    // ugh...i hate these globals
    m_txTextDirtyLastSelectedCall = callsignSelected(true);
    m_txTextDirtyLastText = text;
    m_txFrameCountEstimate = count;
    m_txTextDirty = false;

    updateTextWordCheckerDisplay();
    updateTextStatsDisplay(transmitText, count);
    updateTxButtonDisplay();

#else
    // prepare selected callsign for directed message
    QString selectedCall = callsignSelected();

    // prepare compound
    QString mycall = m_config.my_callsign();
    QString mygrid = m_config.my_grid().left(4);
    bool forceIdentify = !m_config.avoid_forced_identify();
    bool forceData = false;

    BuildMessageFramesThread *t =
        new BuildMessageFramesThread(mycall, mygrid, selectedCall, text,
                                     forceIdentify, forceData, m_nSubMode);

    connect(t, &BuildMessageFramesThread::finished, t, &QObject::deleteLater);
    connect(t, &BuildMessageFramesThread::resultReady, this,
            [this, text](QString transmitText, int frames) {
                // ugh...i hate these globals
                m_txTextDirtyLastSelectedCall = callsignSelected(true);
                m_txTextDirtyLastText = text;
                m_txFrameCountEstimate = frames;
                m_txTextDirty = false;

                updateTextWordCheckerDisplay();
                updateTextStatsDisplay(transmitText, m_txFrameCountEstimate);
                updateTxButtonDisplay();
            });
    t->start();
#endif
}

void UI_Constructor::updateTextWordCheckerDisplay() {
    if (!m_config.spellcheck()) {
        return;
    }

    JSCChecker::checkRange(ui->extFreeTextMsgEdit, 0, -1);
}

void UI_Constructor::updateTextStatsDisplay(QString text, int count) {
    const double fpm = 60.0 / m_TRperiod;
    if (count > 0) {
        auto words = text.split(" ", Qt::SkipEmptyParts).length();
        auto wpm = QString::number(words / (count / fpm), 'f', 1);
        auto cpm = QString::number(text.length() / (count / fpm), 'f', 1);
        wpm_label.setText(QString("%1wpm / %2cpm").arg(wpm).arg(cpm));
        wpm_label.setVisible(true);
    } else {
        wpm_label.setVisible(false);
        wpm_label.clear();
    }
}

void UI_Constructor::updateTxButtonDisplay() {
    // can we transmit at all?
    bool canTransmit = ensureCanTransmit();

    // if we're tuning or have a message queued
    if (m_tune || isMessageQueuedForTransmit()) {
        int count = m_txFrameCount;
        int left = m_txFrameQueue.count();
        int sent = count - left;
        QString buttonText;
        if (m_tune) {
            buttonText = State::Tuning.toString();
        } else if (m_transmitting) {
            buttonText =
                State::timed(State::Sending, ((left + 1) * m_TRperiod) -
                                                 ((m_sec0 + 1) % m_TRperiod));
        } else {
            buttonText = State::timed(
                State::Ready, sent == 1 ? ((left + 1) * m_TRperiod)
                                        : (((left + 2) * m_TRperiod) -
                                           ((m_sec0 + 1) % m_TRperiod)));
        }
        ui->startTxButton->setText(buttonText);
        ui->startTxButton->setEnabled(false);
        ui->startTxButton->setFlat(true);
    } else {
        QString const buttonText =
            m_txFrameCountEstimate > 0
                ? State::timed(State::Send, m_txFrameCountEstimate * m_TRperiod)
                : State::Send.toString();
        ui->startTxButton->setText(buttonText);
        ui->startTxButton->setEnabled(canTransmit &&
                                      m_txFrameCountEstimate > 0);
        ui->startTxButton->setFlat(false);
    }
}

QString UI_Constructor::callsignSelected(bool) {
    if (!ui->tableWidgetCalls->selectedItems().isEmpty()) {
        auto selectedCalls = ui->tableWidgetCalls->selectedItems();
        if (!selectedCalls.isEmpty()) {
            auto call = selectedCalls.first()->data(Qt::UserRole).toString();
            if (!call.isEmpty()) {
                return call;
            }
        }
    }

    if (!ui->tableWidgetRXAll->selectedItems().isEmpty()) {
        int selectedOffset = -1;
        auto selectedItems = ui->tableWidgetRXAll->selectedItems();
        selectedOffset = selectedItems.first()->data(Qt::UserRole).toInt();

        int threshold = 0;
        auto activity = m_bandActivity.value(selectedOffset);
        if (!activity.isEmpty()) {
            threshold = JS8::Submode::rxThreshold(activity.last().submode);
        }

        auto keys = m_callActivity.keys();

        std::stable_sort(keys.begin(), keys.end(),
                         [this](QString const &lhs, QString const &rhs) {
                             auto const lhsTS =
                                 m_callActivity[lhs].utcTimestamp;
                             auto const rhsTS =
                                 m_callActivity[rhs].utcTimestamp;

                             return lhsTS == rhsTS ? lhs < rhs : rhsTS < lhsTS;
                         });

        // Return the first callsign at a frequency within the
        // threshold limit of the selected offset, if any.

        auto const offsetLo = selectedOffset - threshold;
        auto const offsetHi = selectedOffset + threshold;

        for (auto const &key : keys) {
            if (auto const &d = m_callActivity[key];
                offsetLo <= d.offset && d.offset <= offsetHi) {
                return d.call;
            }
        }
    }

#if ALLOW_USE_INPUT_TEXT_CALLSIGN
    if (useInputText) {
        auto text = ui->extFreeTextMsgEdit->toPlainText().left(
            11); // Maximum callsign is 6 + / + 4 = 11 characters
        auto calls = Varicode::parseCallsigns(text);
        if (!calls.isEmpty() && text.startsWith(calls.first()) &&
            calls.first() != m_config.my_callsign()) {
            return calls.first();
        }
    }
#endif

    return QString();
}

void UI_Constructor::callsignSelectedChanged(QString /*old*/,
                                             QString selectedCall) {
    auto placeholderText =
        QString("Type your outgoing messages here.").toUpper();
    if (selectedCall.isEmpty()) {
        // try to restore hb
        if (m_hbPaused) {
            ui->hbMacroButton->setChecked(true);
            m_hbPaused = false;
        }
    } else {
        placeholderText =
            QString("Type your outgoing directed message to %1 here.")
                .arg(selectedCall)
                .toUpper();

        // when we select a callsign, use it as the qso start time
        if (!m_callSelectedTime.contains(selectedCall)) {
            m_callSelectedTime[selectedCall] =
                DriftingDateTime::currentDateTimeUtc();
        }

        if (m_config.heartbeat_qso_pause()) {
            // TODO: jsherer - HB issue
            // don't hb if we select a callsign... (but we should keep track so
            // if we deselect, we restore our hb)
            if (ui->hbMacroButton->isChecked()) {
                qCDebug(mainwindow_js8)
                    << "Unchecking hbMacroButton after selection"
                    << selectedCall << "but planning to resurrect later";
                ui->hbMacroButton->setChecked(false);
                m_hbPaused = true;
            }

            // don't cq if we select a callsign... (and it will not be restored
            // otherwise)
            if (ui->cqMacroButton->isChecked()) {
                qCDebug(mainwindow_js8)
                    << "Unchecking cqMacroButton after selection"
                    << selectedCall;
                ui->cqMacroButton->setChecked(false);
            }
        }
    }
    ui->extFreeTextMsgEdit->setPlaceholderText(placeholderText);
    ui->extFreeTextMsgEdit->pillRenderer()->setSelectedCallsign(selectedCall);

#if SHOW_CALL_DETAIL_BROWSER
    auto html = generateCallDetail(selectedCall);
    ui->callDetailTextBrowser->setHtml(html);
    ui->callDetailTextBrowser->setVisible(
        !selectedCall.isEmpty() && (!hearing.isEmpty() || !heardby.isEmpty()));
#endif

    m_prevSelectedCallsign = selectedCall;

    // immediately update the display
    updateButtonDisplay();
    updateTextDisplay();
    statusChanged();
}

void UI_Constructor::clearCallsignSelected() {
    // remove the date cache
    m_callSelectedTime.remove(m_prevSelectedCallsign);

    // remove the callsign selection
    ui->tableWidgetCalls->clearSelection();
    ui->tableWidgetRXAll->clearSelection();
}

bool UI_Constructor::isRecentOffset(int submode, int offset) {
    if (abs(offset - freq()) <= JS8::Submode::rxThreshold(submode)) {
        return true;
    }
    return (m_rxRecentCache.contains(offset / 10 * 10) &&
            m_rxRecentCache[offset / 10 * 10]->secsTo(
                DriftingDateTime::currentDateTimeUtc()) < 120);
}

void UI_Constructor::markOffsetRecent(int offset) {
    m_rxRecentCache.insert(
        offset / 10 * 10, new QDateTime(DriftingDateTime::currentDateTimeUtc()),
        10);
    m_rxRecentCache.insert(
        offset / 10 * 10 + 10,
        new QDateTime(DriftingDateTime::currentDateTimeUtc()), 10);
}

bool UI_Constructor::isDirectedOffset(int offset, bool *pIsAllCall) {
    bool isDirected = (m_rxDirectedCache.contains(offset / 10 * 10) &&
                       m_rxDirectedCache[offset / 10 * 10]->date.secsTo(
                           DriftingDateTime::currentDateTimeUtc()) < 120);

    if (isDirected && pIsAllCall) {
        *pIsAllCall = m_rxDirectedCache[offset / 10 * 10]->isAllcall;
    }

    return isDirected;
}

void UI_Constructor::markOffsetDirected(int offset, bool isAllCall) {
    CachedDirectedType *d1 = new CachedDirectedType{
        isAllCall, DriftingDateTime::currentDateTimeUtc()};
    CachedDirectedType *d2 = new CachedDirectedType{
        isAllCall, DriftingDateTime::currentDateTimeUtc()};
    m_rxDirectedCache.insert(offset / 10 * 10, d1, 10);
    m_rxDirectedCache.insert(offset / 10 * 10 + 10, d2, 10);
}

void UI_Constructor::clearOffsetDirected(int offset) {
    m_rxDirectedCache.remove(offset / 10 * 10);
    m_rxDirectedCache.remove(offset / 10 * 10 + 10);
}

bool UI_Constructor::isMyCallIncluded(const QString &text) {
    QString myCall = Radio::base_callsign(m_config.my_callsign());

    if (myCall.isEmpty()) {
        return false;
    }

    if (!text.contains(myCall)) {
        return false;
    }

    auto calls = Varicode::parseCallsigns(text);
    return calls.contains(myCall) || calls.contains(m_config.my_callsign());
}

bool UI_Constructor::isAllCallIncluded(const QString &text) {
    return text.contains("@ALLCALL") || text.contains("@HB");
}

bool UI_Constructor::isGroupCallIncluded(const QString &text) {
    return m_config.my_groups().contains(text);
}

void UI_Constructor::processActivity(bool force) {
    if (!m_rxDirty && !force) {
        return;
    }

    // Recent Rx Activity
    processRxActivity();

    // Process Idle Activity
    processIdleActivity();

    // Grouped Compound Activity
    processCompoundActivity();

    // Buffered Activity
    processBufferedActivity();

    // Command Activity
    processCommandActivity();

    // Process PSKReporter Spots
    processSpots();

    m_rxDirty = false;
}

void UI_Constructor::resetTimeDeltaAverage() {
    m_driftMsMMA = 0;
    m_driftMsMMA_N = 0;
}

void UI_Constructor::setDrift(int n) { DriftingDateTime::setDrift(n); }

void UI_Constructor::processIdleActivity() {
    auto const now = DriftingDateTime::currentDateTimeUtc();

    // if we detect an idle offset, insert an ellipsis into the activity queue
    // and band activity

    for (auto [offset, activity] : m_bandActivity.asKeyValueRange()) {
        if (activity.isEmpty())
            continue;

        auto const last = activity.last();

        if ((last.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast)
            continue;
        if (last.text == m_config.mfi())
            continue;
        if (last.utcTimestamp.secsTo(now) <
            JS8::Submode::period(last.submode) * 1.50)
            continue;

        ActivityDetail d = {};
        d.text = m_config.mfi();
        d.utcTimestamp = last.utcTimestamp;
        d.snr = last.snr;
        d.tdrift = last.tdrift;
        d.dial = last.dial;
        d.offset = last.offset;
        d.submode = last.submode;

        if (hasExistingMessageBuffer(d.submode, offset, false, nullptr)) {
            m_messageBuffer[offset].msgs.append(d);
        }

        m_rxActivityQueue.append(d);
        activity.append(d);
    }
}

void UI_Constructor::processCompoundActivity() {
    if (m_messageBuffer.isEmpty()) {
        return;
    }

    // group compound callsign and directed commands together.
    foreach (auto freq, m_messageBuffer.keys()) {

        auto &buffer = m_messageBuffer[freq];

        qCDebug(mainwindow_js8) << "-> grouping buffer for freq" << freq;

        if (buffer.compound.isEmpty()) {
            qCDebug(mainwindow_js8) << "-> buffer.compound is empty...skip";
            continue;
        }

        // if we don't have an initialized command, skip...
        int bits = buffer.cmd.bits;
        bool validBits =
            (bits == Varicode::JS8Call ||
             ((bits & Varicode::JS8CallFirst) == Varicode::JS8CallFirst) ||
             ((bits & Varicode::JS8CallLast) == Varicode::JS8CallLast) ||
             ((bits & Varicode::JS8CallData) == Varicode::JS8CallData));
        if (!validBits) {
            qCDebug(mainwindow_js8) << "-> buffer.cmd bits is invalid...skip";
            continue;
        }

        // if we need two compound calls, but less than two have arrived...skip
        if (buffer.cmd.from == "<....>" && buffer.cmd.to == "<....>" &&
            buffer.compound.length() < 2) {
            qCDebug(mainwindow_js8)
                << "-> buffer needs two compound, but has less...skip";
            continue;
        }

        // if we need one compound call, but non have arrived...skip
        if ((buffer.cmd.from == "<....>" || buffer.cmd.to == "<....>") &&
            buffer.compound.length() < 1) {
            qCDebug(mainwindow_js8)
                << "-> buffer needs one compound, but has less...skip";
            continue;
        }

        if (buffer.cmd.from == "<....>") {
            auto d = buffer.compound.dequeue();
            buffer.cmd.from = d.call;
            buffer.cmd.grid = d.grid;
            buffer.cmd.isCompound = true;
            buffer.cmd.utcTimestamp =
                qMin(buffer.cmd.utcTimestamp, d.utcTimestamp);

            if ((d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast) {
                buffer.cmd.bits = d.bits;
            }
        }

        if (buffer.cmd.to == "<....>") {
            auto d = buffer.compound.dequeue();
            buffer.cmd.to = d.call;
            buffer.cmd.isCompound = true;
            buffer.cmd.utcTimestamp =
                qMin(buffer.cmd.utcTimestamp, d.utcTimestamp);

            if ((d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast) {
                buffer.cmd.bits = d.bits;
            }
        }

        if ((buffer.cmd.bits & Varicode::JS8CallLast) !=
            Varicode::JS8CallLast) {
            qCDebug(mainwindow_js8) << "-> still not last message...skip";
            continue;
        }

        // fixup the datetime with the "minimum" dt seen
        // this will allow us to delete the activity lines
        // when the compound buffered command comes in.
        auto dt = buffer.cmd.utcTimestamp;
        foreach (auto c, buffer.compound) {
            dt = qMin(dt, c.utcTimestamp);
        }
        foreach (auto m, buffer.msgs) {
            dt = qMin(dt, m.utcTimestamp);
        }
        buffer.cmd.utcTimestamp = dt;

        qCDebug(mainwindow_js8)
            << "buffered compound command ready" << buffer.cmd.from
            << buffer.cmd.to << buffer.cmd.cmd;

        m_rxCommandQueue.append(buffer.cmd);
        m_messageBuffer.remove(freq);

        // TODO: only if to me?
        m_lastClosedMessageBufferOffset = freq;
    }
}

QString UI_Constructor::inboxPath() {
    return QDir::toNativeSeparators(
        m_config.writeable_data_dir().absoluteFilePath("inbox.db3"));
}

void UI_Constructor::refreshInboxCounts() {
    auto inbox = Inbox(inboxPath());
    if (inbox.open()) {
        // reset inbox counts
        m_rxInboxCountCache.clear();

        // compute new counts from db
        auto v = inbox.values("UNREAD", "$", "%", 0, 10000);
        foreach (auto pair, v) {
            auto params = pair.second.params();
            auto to = params.value("TO").toString();
            if (to.isEmpty() ||
                (to != m_config.my_callsign() &&
                 to != Radio::base_callsign(m_config.my_callsign()))) {
                continue;
            }
            auto from = params.value("FROM").toString();
            if (from.isEmpty()) {
                continue;
            }

            m_rxInboxCountCache[from] = m_rxInboxCountCache.value(from, 0) + 1;

            if (!m_callActivity.contains(from)) {
                auto const utc = params.value("UTC").toString();
                auto const snr = params.value("SNR").toInt();
                auto const dial = params.value("DIAL").toInt();
                auto const offset = params.value("OFFSET").toInt();
                auto const tdrift = params.value("TDRIFT").toInt();
                auto const submode = params.value("SUBMODE").toInt();

                CallDetail cd;
                cd.call = from;
                cd.snr = snr;
                cd.dial = dial;
                cd.offset = offset;
                cd.tdrift = tdrift;
                cd.utcTimestamp =
                    QDateTime::fromString(utc, "yyyy-MM-dd hh:mm:ss");
                cd.utcTimestamp.setTimeZone(QTimeZone::utc());
                cd.ackTimestamp = cd.utcTimestamp;
                cd.submode = submode;
                logCallActivity(cd, false);
            }
        }

        // Now handle group message counts
        QMap<QString, int> groupMessageCounts = inbox.getGroupMessageCounts();
        foreach (auto key, groupMessageCounts.keys()) {
            m_rxInboxCountCache[key] = groupMessageCounts[key];
        }
    }
}

bool UI_Constructor::hasMessageHistory(QString call) {
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return false;
    }

    int store = inbox.count("STORE", "$.params.TO", call);
    int unread = inbox.count("UNREAD", "$.params.FROM", call);
    int read = inbox.count("READ", "$.params.FROM", call);
    return (store + unread + read) > 0;
}

int UI_Constructor::addCommandToMyInbox(CommandDetail d) {
    // local cache for inbox count
    m_rxInboxCountCache[d.from] = m_rxInboxCountCache.value(d.from, 0) + 1;

    // add it to my unread inbox
    return addCommandToStorage("UNREAD", d);
}

int UI_Constructor::addCommandToStorage(QString type, CommandDetail d) {
    // inbox:
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    QVariantMap v = {
        {"UTC", QVariant(d.utcTimestamp.toString("yyyy-MM-dd hh:mm:ss"))},
        {"TO", QVariant(d.to)},
        {"FROM", QVariant(d.from)},
        {"PATH", QVariant(d.relayPath)},
        {"TDRIFT", QVariant(d.tdrift)},
        {"FREQ", QVariant(d.dial + d.offset)},
        {"DIAL", QVariant(d.dial)},
        {"OFFSET", QVariant(d.offset)},
        {"CMD", QVariant(d.cmd)},
        {"SNR", QVariant(d.snr)},
        {"SUBMODE", QVariant(d.submode)},
    };

    if (!d.grid.isEmpty()) {
        v["GRID"] = QVariant(d.grid);
    }

    if (!d.extra.isEmpty()) {
        v["EXTRA"] = QVariant(d.extra);
    }

    if (!d.text.isEmpty()) {
        v["TEXT"] = QVariant(d.text);
    }

    auto m = Message(type, "", v);

    int msgId = inbox.append(m);

    emit messageAdded(msgId);

    return msgId;
}

int UI_Constructor::getNextMessageIdForCallsign(QString callsign) {
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    auto v1 = inbox.values("STORE", "$.params.TO", callsign, 0, 10);
    foreach (auto pair, v1) {
        auto params = pair.second.params();
        auto text = params.value("TEXT").toString().trimmed();
        if (!text.isEmpty()) {
            return pair.first;
        }
    }

    auto v2 = inbox.values("STORE", "$.params.TO",
                           Radio::base_callsign(callsign), 0, 10);
    foreach (auto pair, v2) {
        auto params = pair.second.params();
        auto text = params.value("TEXT").toString().trimmed();
        if (!text.isEmpty()) {
            return pair.first;
        }
    }

    return -1;
}

int UI_Constructor::getLookaheadMessageIdForCallsign(QString callsign,
                                                     int msgId) {
    auto inbox = Inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    int mid = inbox.getLookaheadMessageIdForCallsign(callsign, msgId);

    if (mid == -1) {
        mid = inbox.getLookaheadMessageIdForCallsign(
            Radio::base_callsign(callsign), msgId);
    }

    if (mid != -1) {
        return mid;
    }

    return -1;
}

// Facade for Inbox::getNextGroupMessageIdForCallsign
int UI_Constructor::getNextGroupMessageIdForCallsign(QString group_name,
                                                     QString callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    return inbox.getNextGroupMessageIdForCallsign(group_name, callsign);
}

// Facade for Inbox::getLookaheadGroupMessageIdForCallsign
int UI_Constructor::getLookaheadGroupMessageIdForCallsign(QString group_name,
                                                          QString callsign,
                                                          int afterMsgId) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return -1;
    }

    int mid = inbox.getLookaheadGroupMessageIdForCallsign(group_name, callsign,
                                                          afterMsgId);

    if (mid == -1) {
        mid = inbox.getLookaheadGroupMessageIdForCallsign(
            group_name, Radio::base_callsign(callsign), afterMsgId);
    }

    if (mid != -1) {
        return mid;
    }

    return -1;
}

// Facade for Inbox::countUnreadForCallsign
int UI_Constructor::countUnreadForCallsign(const QString &callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return 0;
    }

    return inbox.countUnreadForCallsign(callsign);
}

// Facade for Inbox::countGroupUnreadForCallsign
int UI_Constructor::countGroupUnreadForCallsign(const QString &group_name,
                                            const QString &callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return 0;
    }

    return inbox.countGroupUnreadForCallsign(group_name, callsign);
}

// Facade for Inbox::markGroupMsgDeliveredForCallsign
bool UI_Constructor::markGroupMsgDeliveredForCallsign(int msgId,
                                                      QString callsign) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return false;
    }

    return inbox.markGroupMsgDeliveredForCallsign(msgId, callsign);
}

bool UI_Constructor::markMsgDelivered(int mid, Message msg) {
    Inbox inbox(inboxPath());
    if (!inbox.open()) {
        return false;
    }

    msg.setType("DELIVERED");
    return inbox.set(mid, msg);
}

QStringList UI_Constructor::parseRelayPathCallsigns(QString from,
                                                    QString text) {
    QStringList calls;
    QString callDePattern = {
        R"(\s([*]DE[*]|VIA)\s(?<callsign>\b(?<prefix>[A-Z0-9]{1,4}\/)?(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<suffix>\/[A-Z0-9]{1,4})?)\b)"};
    QRegularExpression re(callDePattern);
    auto iter = re.globalMatch(text);
    while (iter.hasNext()) {
        auto match = iter.next();
        calls.prepend(match.captured("callsign"));
    }
    calls.prepend(from);
    return calls;
}

void UI_Constructor::processSpots() {
    if (!m_config.spot_to_reporting_networks()) {
        m_rxCallQueue.clear();
        return;
    }

    if (m_rxCallQueue.isEmpty()) {
        return;
    }

    // Is it ok to post spots to PSKReporter?
    int nsec = DriftingDateTime::currentSecsSinceEpoch() - m_secBandChanged;
    bool okToPost = (nsec > (4 * m_TRperiod) / 5);
    if (!okToPost) {
        return;
    }

    while (!m_rxCallQueue.isEmpty()) {
        CallDetail d = m_rxCallQueue.dequeue();
        if (d.call.isEmpty()) {
            continue;
        }

        if (m_config.spot_blacklist().contains(d.call) ||
            m_config.spot_blacklist().contains(Radio::base_callsign(d.call))) {
            continue;
        }

        qCDebug(mainwindow_js8) << "spotting call to reporting networks"
                                << d.call << d.snr << d.dial << d.offset;

        spotReport(d.submode, d.dial, d.offset, d.snr, d.call, d.grid);
        pskLogReport("JS8", d.dial, d.offset, d.snr, d.call, d.grid,
                     d.utcTimestamp);

        if (canSendNetworkMessage()) {
            sendNetworkMessage("RX.SPOT", "",
                               {
                                   {"_ID", QVariant(-1)},
                                   {"FREQ", QVariant(d.dial + d.offset)},
                                   {"DIAL", QVariant(d.dial)},
                                   {"OFFSET", QVariant(d.offset)},
                                   {"CALL", QVariant(d.call)},
                                   {"SNR", QVariant(d.snr)},
                                   {"GRID", QVariant(d.grid)},
                               });
        }
    }
}

/**
 * @brief Processes the outgoing message queue and initiates transmission.
 *
 * This function is called periodically (once per second) to check if there
 * are pending messages in m_txMessageQueue that can be transmitted. It
 * implements several guard conditions to ensure safe transmission:
 *
 * - The frame queue (m_txFrameQueue) must be empty
 * - The message text box must be empty
 * - No active transmission in progress (m_transmitting and m_txFrameCount)
 * - Low priority messages must wait 30 seconds after last transmission
 *
 * When conditions are met, the next message is dequeued, placed in the
 * message text box, and transmission is initiated for high-priority messages.
 *
 * @note This function works in conjunction with resetMessageTransmitQueue()
 *       to support queuing multiple messages (e.g., APRS relay messages)
 *       that are transmitted sequentially.
 */
void UI_Constructor::processTxQueue() {
#if IDLE_BLOCKS_TX
    if (m_tx_watchdog) {
        return;
    }
#endif

    if (m_txMessageQueue.isEmpty()) {
        return;
    }

    // grab the next message...
    auto head = m_txMessageQueue.head();

    // decide if it's ok to transmit...
    int f = head.offset;
    if (f == -1) {
        f = freq();
    }

    // we need a valid frequency...
    if (f <= 0) {
        return;
    }

    // tx frame queue needs to be empty...
    if (!m_txFrameQueue.isEmpty()) {
        return;
    }

    // our message box needs to be empty...
    if (!ui->extFreeTextMsgEdit->toPlainText().isEmpty()) {
        return;
    }

    // don't process if we're currently transmitting...
    if (isMessageQueuedForTransmit()) {
        return;
    }

    // and if we are a low priority message, we need to have not transmitted
    // in the past 30 seconds...
    if (head.priority <= PriorityLow &&
        m_lastTxStartTime.secsTo(DriftingDateTime::currentDateTimeUtc()) <=
            30) {
        return;
    }

    // if so... dequeue the next message from the queue...
    auto message = m_txMessageQueue.dequeue();

    // add the message to the outgoing message text box
    addMessageText(message.message, true);

    // check to see if this is a high priority message, or if we have
    // autoreply enabled, or if this is a ping and the ping button is
    // enabled
    if (message.priority >= PriorityHigh ||
        message.message.contains(" HEARTBEAT ") ||
        message.message.contains(" HB ") || message.message.contains(" ACK ") ||
        ui->actionModeAutoreply->isChecked()) {
        if (m_sliderFreqBeforeHB == 0) {
            m_sliderFreqBeforeHB = freq(); // save current freq before HB changes it
        }
        changeFreq(f);
        toggleTx(true);
    }

    if (message.callback) {
        message.callback();
    }
}

void UI_Constructor::displayActivity(bool force) {
    if (!m_rxDisplayDirty && !force) {
        return;
    }

    // Band Activity
    displayBandActivity();

    // Call Activity
    displayCallActivity();

    m_rxDisplayDirty = false;
}

void UI_Constructor::emitPTT(bool on) {
    qCDebug(mainwindow_js8) << "Setting PTT to" << (on ? "on" : "off");

    Q_EMIT m_config.transceiver_ptt(on);

    // emit to network
    sendNetworkMessage(
        "RIG.PTT", on ? "on" : "off",
        {
            {"_ID", QVariant(-1)},
            {"PTT", QVariant(on)},
            {"UTC",
             QVariant(
                 DriftingDateTime::currentDateTimeUtc().toMSecsSinceEpoch())},
        });
}

void UI_Constructor::emitTones() {
    if (!canSendNetworkMessage()) {
        return;
    }

    // emit tone numbers to network
    QVariantList t;
    for (int i = 0; i < JS8_NUM_SYMBOLS; i++) {
        // qCDebug(mainwindow_js8) << "tone" << i << "=" << itone[i];
        t.append(QVariant((int)itone[i]));
    }

    sendNetworkMessage("TX.FRAME", "", {{"_ID", QVariant(-1)}, {"TONES", t}});
}

void UI_Constructor::udpNetworkMessage(Message const &message) {
    if (!m_config.udpEnabled()) {
        return;
    }

    if (!m_config.accept_udp_requests()) {
        return;
    }

    networkMessage(message);
}

void UI_Constructor::tcpNetworkMessage(Message const &message) {
    if (!m_config.tcpEnabled()) {
        return;
    }

    if (!m_config.accept_tcp_requests()) {
        return;
    }

    networkMessage(message);
}

bool UI_Constructor::canSendNetworkMessage() {
    return m_config.udpEnabled() || m_config.tcpEnabled();
}

void UI_Constructor::sendNetworkMessage(QString const &type,
                                        QString const &message) {
    if (!canSendNetworkMessage()) {
        return;
    }

    auto m = Message(type, message);

    if (m_config.udpEnabled()) {
        m_messageClient->send(m);
    }

    if (m_config.tcpEnabled()) {
        m_messageServer->send(m);
    }
}

void UI_Constructor::sendNetworkMessage(QString const &type,
                                        QString const &message,
                                        QVariantMap const &params) {
    if (!canSendNetworkMessage()) {
        return;
    }

    auto m = Message(type, message, params);

    if (m_config.udpEnabled()) {
        m_messageClient->send(m);
    }

    if (m_config.tcpEnabled()) {
        m_messageServer->send(m);
    }
}

void UI_Constructor::pskReporterError(QString const &message) {
    qCDebug(mainwindow_js8) << "PSK Reporter Error:" << message;

    showStatusMessage(tr("Spotting to PSK Reporter unavailable"));
}

void UI_Constructor::setRig(Frequency f) {
    if (f) {
        m_freqNominal = f;
        m_freqTxNominal = m_freqNominal - m_XIT;
    }

    if (m_transmitting && !m_config.tx_qsy_allowed())
        return;

    if ((m_monitoring || m_transmitting) && m_config.transceiver_online()) {
        if (m_config.split_mode()) {
            Q_EMIT m_config.transceiver_tx_frequency(m_freqTxNominal);
        }

        Q_EMIT m_config.transceiver_frequency(m_freqNominal);
    }
}

/**
 * @brief Update and send station status
 *
 * Sends station status updates to both WSJT-X protocol clients (if enabled)
 * and native JSON API clients (if not conflicting). When WSJT-X protocol
 * is enabled on the same port/address as the native JSON API, the native
 * JSON messages are skipped to avoid conflicts.
 */
void UI_Constructor::statusUpdate() {
    // Send WSJT-X Status message if protocol is enabled
    if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
        QString dx_call = callsignSelected();
        QString dx_grid = "";
        if (!dx_call.isEmpty() && m_callActivity.contains(dx_call)) {
            dx_grid = m_callActivity[dx_call].grid;
        }
        QString mode = JS8::Submode::name(m_nSubMode);
        QString tx_message = m_transmitting ? m_currentMessage : "";

        m_wsjtxMessageMapper->sendStatusUpdate(
            dialFrequency(), freq(),
            "JS8", // mode
            dx_call, m_config.my_callsign(), m_config.my_grid(), dx_grid,
            true, // tx_enabled - JS8Call always allows TX when not in
                  // special modes
            m_transmitting,
            m_decoderBusy, // decoding (match WSJT-X: decoder busy only)
            tx_message);
    }

    // Send native JSON message only if not conflicting with WSJT-X
    if (canSendNetworkMessage()) {
        // Don't send JSON if WSJT-X is enabled on the same port/address
        bool skip_json = false;
        if (m_config.wsjtx_protocol_enabled() &&
            m_config.wsjtx_server_port() == m_config.udp_server_port() &&
            m_config.wsjtx_server_name() == m_config.udp_server_name()) {
            skip_json = true;
        }

        if (!skip_json) {
            sendNetworkMessage("STATION.STATUS", "",
                               {
                                   {"FREQ", QVariant(dialFrequency() + freq())},
                                   {"DIAL", QVariant(dialFrequency())},
                                   {"OFFSET", QVariant(freq())},
                                   {"SPEED", QVariant(m_nSubMode)},
                                   {"SELECTED", QVariant(callsignSelected())},
                               });
        }
    }
}

void UI_Constructor::childEvent(QChildEvent *e) {
    if (e->child()->isWidgetType()) {
        switch (e->type()) {
        case QEvent::ChildAdded:
            add_child_to_event_filter(e->child());
            break;
        case QEvent::ChildRemoved:
            remove_child_from_event_filter(e->child());
            break;
        default:
            break;
        }
    }
    QMainWindow::childEvent(e);
}

// add widget and any child widgets to our event filter so that we can
// take action on key press ad mouse press events anywhere in the main
// window
void UI_Constructor::add_child_to_event_filter(QObject *target) {
    if (target && target->isWidgetType()) {
        target->installEventFilter(this);
    }
    auto const &children = target->children();
    for (auto iter = children.begin(); iter != children.end(); ++iter) {
        add_child_to_event_filter(*iter);
    }
}

// recursively remove widget and any child widgets from our event filter
void UI_Constructor::remove_child_from_event_filter(QObject *target) {
    auto const &children = target->children();
    for (auto iter = children.begin(); iter != children.end(); ++iter) {
        remove_child_from_event_filter(*iter);
    }
    if (target && target->isWidgetType()) {
        target->removeEventFilter(this);
    }
}

void UI_Constructor::resetIdleTimer() {
    if (m_idleMinutes) {
        m_idleMinutes = 0;
        qCDebug(mainwindow_js8) << "idle" << m_idleMinutes << "minutes";
    }
}

void UI_Constructor::incrementIdleTimer() {
    m_idleMinutes++;
    qCDebug(mainwindow_js8)
        << "increment idle to" << m_idleMinutes << "minutes";
}

void UI_Constructor::tx_watchdog(bool triggered) {
    auto prior = m_tx_watchdog;
    m_tx_watchdog = triggered;
    if (triggered) {
        m_isTimeToSend = false;
        if (m_tune)
            stop_tuning();
        if (m_auto)
            auto_tx_mode(false);
        stopTx();
        {
            tx_status_label.setStyleSheet(txStatusLabelStyle(TxStatusAppearance::IdleTimeout));
        }
        tx_status_label.setText("Idle timeout");

        // if the watchdog is triggered...we're no longer active
        bool wasAuto = ui->actionModeAutoreply->isChecked();
        bool wasHB = ui->hbMacroButton->isChecked();
        bool wasCQ = ui->cqMacroButton->isChecked();

        // save the button states
        ui->actionModeAutoreply->setChecked(false);
        qCDebug(mainwindow_js8) << "Unchecking the hbMacroButton and "
                                   "cqMacroButton from TX watchdog.";
        ui->hbMacroButton->setChecked(false);
        ui->cqMacroButton->setChecked(false);
        auto_reply_label.setText(QString("Auto Reply: %1").arg("Off"));

        // clear the tx queues
        resetMessageTransmitQueue();

        QMessageBox *msgBox = new QMessageBox(this);
        msgBox->setIcon(QMessageBox::Information);
        msgBox->setWindowTitle("Idle Timeout");
        msgBox->setInformativeText(
            QString("You have been idle for more than %1 minutes.")
                .arg(m_config.watchdog()));
        msgBox->addButton(QMessageBox::Ok);

        connect(msgBox, &QMessageBox::finished, this,
                [this, wasAuto, wasHB, wasCQ](int /*result*/) {
                    // restore the button states
                    ui->actionModeAutoreply->setChecked(wasAuto);
                    {
                        QString autoReplyState = ui->actionModeAutoreply->isChecked() ? "On" : "Off";
                        auto_reply_label.setText(QString("Auto Reply: %1").arg(autoReplyState));
                    }
                    ui->hbMacroButton->setChecked(wasHB);
                    ui->cqMacroButton->setChecked(wasCQ);

                    this->tx_watchdog(false);
                });
        msgBox->setModal(true);
        msgBox->show();
    }
    if (prior != triggered)
        statusUpdate();
}

void UI_Constructor::write_frequency_entry(QString const &file_name) {
    if (!m_config.write_logs()) {
        return;
    }

    // Write freq changes to ALL.TXT only below 30 MHz.
    QFile f2{m_config.writeable_data_dir().absoluteFilePath(file_name)};
    if (f2.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f2);
        out << DriftingDateTime::currentDateTimeUtc().toString(
                   "yyyy-MM-dd hh:mm:ss")
            << "  " << qSetRealNumberPrecision(12) << (m_freqNominal / 1.e6)
            << " MHz  "
            << "JS8" << Qt::endl;
        f2.close();
    } else {
        QTimer::singleShot(0, [this,
                               message = tr("Cannot open \"%1\" for append: %2")
                                             .arg(f2.fileName())
                                             .arg(f2.errorString())] {
            JS8MessageBox::warning_message(this, tr("Log File Error"), message);
        });
    }
}

void UI_Constructor::write_transmit_entry(QString const &file_name) {
    if (!m_config.write_logs()) {
        return;
    }

    QFile f{m_config.writeable_data_dir().absoluteFilePath(file_name)};
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f);
        auto time = DriftingDateTime::currentDateTimeUtc();
        time = time.addSecs(-(time.time().second() % m_TRperiod));
        auto dt =
            DecodedText(m_currentMessage, m_currentMessageBits, m_nSubMode);
        out << time.toString("yyyy-MM-dd hh:mm:ss") << "  Transmitting "
            << qSetRealNumberPrecision(12) << (m_freqNominal / 1.e6) << " MHz  "
            << "JS8"
            << ":  " << dt.message() << Qt::endl;
        f.close();
    } else {
        QTimer::singleShot(0, [this,
                               message = tr("Cannot open \"%1\" for append: %2")
                                             .arg(f.fileName())
                                             .arg(f.errorString())] {
            JS8MessageBox::warning_message(this, tr("Log File Error"), message);
        });
    }
}

void UI_Constructor::writeAllTxt(QStringView message) {
    if (!m_config.write_logs())
        return;

    // Write decoded text to file "ALL.TXT".

    QFile f{m_config.writeable_data_dir().absoluteFilePath("ALL.TXT")};

    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f);

        if (m_RxLog == 1) {
            out << DriftingDateTime::currentDateTimeUtc().toString(
                       "yyyy-MM-dd hh:mm:ss")
                << "  " << qSetRealNumberPrecision(12) << (m_freqNominal / 1.e6)
                << " MHz  JS8" << Qt::endl;

            m_RxLog = 0;
        }

        out << message << Qt::endl;

        f.close();
    } else {
        JS8MessageBox::warning_message(this, tr("File Open Error"),
                                       tr("Cannot open \"%1\" for append: %2")
                                           .arg(f.fileName())
                                           .arg(f.errorString()));
    }
}

void UI_Constructor::writeMsgTxt(QStringView message, int snr, int offset) {
    if (!m_config.write_logs())
        return;

    // Write decoded text to file "DIRECTED.TXT".

    QFile f{m_config.writeable_data_dir().absoluteFilePath("DIRECTED.TXT")};

    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream out(&f);
        QString output = DriftingDateTime::currentDateTimeUtc().toString(
                             "yyyy-MM-dd hh:mm:ss") %
                         "\t" % Radio::frequency_MHz_string(m_freqNominal) %
                         "\t" % QString::number(offset) % "\t" %
                         Varicode::formatSNR(snr) % "\t" % message;

        out << output << Qt::endl;

        f.close();
    } else {
        JS8MessageBox::warning_message(this, tr("File Open Error"),
                                       tr("Cannot open \"%1\" for append: %2")
                                           .arg(f.fileName())
                                           .arg(f.errorString()));
    }
}

QByteArray UI_Constructor::wisdomFileName() const {
    return QDir::toNativeSeparators(
               m_config.writeable_data_dir().absoluteFilePath(
                   "js8call_wisdom.dat"))
        .toLocal8Bit();
}

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)
Q_LOGGING_CATEGORY(mainwindow_js8, "mainwindow.js8", QtWarningMsg)
