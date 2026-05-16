/**
 * \file UI_Constructor.cpp
 * @brief explicit member function of the UI_Constructor class
 *   constructs and connects UI elements to the JS8 "engine"
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/styles.h"

int ms_minute_error() {
    auto const now = DriftingDateTime::currentDateTimeLocal();
    auto const time = now.time();
    auto const second = time.second();

    return now.msecsTo(now.addSecs(second > 30 ? 60 - second : -second)) -
           time.msec();
}

//--------------------------------------------------- UI_Constructor
UI_Constructor::UI_Constructor(QString const &program_info,
                               QDir const &temp_directory, bool const multiple,
                               MultiSettings *multi_settings, QWidget *parent)
    : QMainWindow(parent), m_stopTxButtonIsLongterm{true},
      m_hbButtonIsLongterm{true}, m_cqButtonIsLongterm{true},
      m_network_manager{this}, m_valid{true}, m_multiple{multiple},
      m_multi_settings{multi_settings}, m_configurations_button{0},
      m_settings{multi_settings->settings()}, m_settings_read{false},
      ui(new Ui::UI_Constructor), m_config{temp_directory, m_settings, this},
      m_rigErrorMessageBox{JS8MessageBox::Critical, tr("Rig Control Error"),
                           JS8MessageBox::Cancel | JS8MessageBox::Ok |
                               JS8MessageBox::Retry},
      m_wideGraph(new WideGraph(m_settings)),
      // no parent so that it has a taskbar icon
      m_logDlg(new LogQSO(program_title(), m_settings, &m_config, nullptr)),
      m_lastDialFreq{0},
      m_detector{new Detector{JS8_RX_SAMPLE_RATE, JS8_NTMAX}},
      m_FFTSize{6912 / 2}, // conservative value to avoid buffer overruns
      m_soundInput{new SoundInput}, m_modulator{new Modulator},
      m_soundOutput{new SoundOutput}, m_notification{new NotificationAudio},
      m_cq_loop{new TxLoop{"CQ calls"}}, m_hb_loop{new TxLoop{"HB calls"}},
      m_decoder{this}, m_secBandChanged{0}, m_freqNominal{0},
      m_freqTxNominal{0}, m_XIT{0}, m_sec0{-1},
      m_RxLog{1}, // Write Date and Time to RxLog
      m_nutc0{999999}, m_TRperiod{60}, m_inGain{0}, m_idleMinutes{0},
      m_nSubMode{Default::SUBMODE},
      m_frequency_list_fcal_iter{m_config.frequencies()->begin()}, m_i3bit{0},
      m_btxok{false}, m_auto{false}, m_restart{false}, m_currentMessageType{-1},
      m_lastMessageType{-1}, m_tuneup{false}, m_isTimeToSend{false}, m_ihsym{0},
      m_px{0.0}, m_iptt0{0}, m_btxok0{false}, m_onAirFreq0{0.0},
      m_first_error{true}, tx_status_label{"Receiving"},
      m_appDir{QApplication::applicationDirPath()}, m_palette{"Linrad"},
      m_txFrameCountEstimate{0}, m_txFrameCount{0}, m_txFrameCountSent{0},
      m_txTextDirty{false}, m_driftMsMMA{0}, m_driftMsMMA_N{0},
      m_hbInterval{0}, m_cqInterval{0}, m_hbPaused{false},
      m_msAudioOutputBuffered(0u),
      m_framesAudioInputBuffered(JS8_RX_SAMPLE_RATE / 10),
      m_audioThreadPriority(QThread::HighPriority),
      m_notificationAudioThreadPriority(QThread::LowPriority),
      m_decoderThreadPriority(QThread::HighPriority), m_splitMode{false},
      m_monitoring{false}, m_generateAudioWhenPttConfirmedByTX{false},
      m_transmitting{false}, m_tune{false}, m_tx_watchdog{false},
      m_block_pwr_tooltip{false}, m_PwrBandSetOK{true},
      m_lastMonitoredFrequency{Default::DIAL_FREQUENCY},
      m_messageClient{new MessageClient{m_config.udp_server_name(),
                                        m_config.udp_server_port(), this}},
      m_messageServer{new MessageServer()}, m_wsjtxMessageClient{nullptr},
      m_wsjtxMessageMapper{nullptr}, m_n3fjpClient{new TCPClient{this}},
      m_pskReporter{new PSKReporter{&m_config, program_info}}, // UR
      m_spotClient{new SpotClient{"spot.js8call.com", 50000, program_info}},
      m_aprsClient{new APRSISClient{"rotate.aprs2.net", 14580}},
      m_aprsInboundRelay{nullptr} {
    ui->setupUi(this);
    ui->frame->setStyleSheet(logFrameStyle());
    ui->logWidget->setStyleSheet(Styles::LogWidgetStyle);
    ui->dialFreqUpButton->setStyleSheet(Styles::DialFreqUpDownButtonStyle);
    ui->dialFreqDownButton->setStyleSheet(Styles::DialFreqUpDownButtonStyle);
    ui->labCallsign->setStyleSheet(Styles::LabCallsignStyle);
    ui->labUTC->setStyleSheet(Styles::LabUTCStyle);
    ui->buttonGrid->setStyleSheet(Styles::ButtonGridStyle);
    ui->monitorTxButton->setStyleSheet(Styles::MonitorTxButtonStyle);
    ui->monitorButton->setStyleSheet(Styles::MonitorButtonStyle);
    ui->logQSOButton->setStyleSheet(Styles::LogQSOButtonStyle);
    ui->tuneButton->setStyleSheet(Styles::TuneButtonStyle);
    ui->modeButton->setStyleSheet(Styles::ModeButtonStyle);
    ui->spotButton->setStyleSheet(Styles::SpotButtonStyle);

    createStatusBar();
    add_child_to_event_filter(this);

    m_baseCall = Radio::base_callsign(m_config.my_callsign());
    m_opCall = m_config.opCall();

    // Closedown.
    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);

    // parts of the rig error message box that are fixed
    m_rigErrorMessageBox.setInformativeText(
        tr("Do you want to reconfigure the radio interface?"));
    m_rigErrorMessageBox.setDefaultButton(JS8MessageBox::Ok);

    // start audio thread and hook up slots & signals for shutdown management
    // these objects need to be in the audio thread so that invoking
    // their slots is done in a thread safe way
    m_soundOutput->moveToThread(&m_audioThread);
    m_modulator->moveToThread(&m_audioThread);
    m_soundInput->moveToThread(&m_audioThread);
    m_detector->moveToThread(&m_audioThread);

    // notification audio operates in its own thread at a lower priority
    m_notification->moveToThread(&m_notificationAudioThread);

    // Move the aprs client message server, psk reporter, and spot client
    // to the network thread at a lower priority.

    m_aprsClient->moveToThread(&m_networkThread);
    m_messageServer->moveToThread(&m_networkThread);
    m_pskReporter->moveToThread(&m_networkThread);
    m_spotClient->moveToThread(&m_networkThread);

    // hook up the message server slots and signals and disposal
    connect(m_messageServer, &MessageServer::message, this,
            &UI_Constructor::tcpNetworkMessage);
    connect(this, &UI_Constructor::apiSetMaxConnections, m_messageServer,
            &MessageServer::setMaxConnections);
    connect(this, &UI_Constructor::apiSetServer, m_messageServer,
            &MessageServer::setServer);
    connect(this, &UI_Constructor::apiStartServer, m_messageServer,
            &MessageServer::start);
    connect(this, &UI_Constructor::apiStopServer, m_messageServer,
            &MessageServer::stop);
    connect(&m_config, &Configuration::tcp_server_changed, m_messageServer,
            &MessageServer::setServerHost);
    connect(&m_config, &Configuration::tcp_server_port_changed, m_messageServer,
            &MessageServer::setServerPort);
    connect(&m_config, &Configuration::tcp_max_connections_changed,
            m_messageServer, &MessageServer::setMaxConnections);
    connect(&m_networkThread, &QThread::finished, m_messageServer,
            &QObject::deleteLater);

    m_aprsInboundRelay = new AprsInboundRelay(
        &m_config,
        [this](QString const &call) {
            AprsInboundRelay::CallActivityInfo info;
            auto const it = m_callActivity.constFind(call);
            if (it == m_callActivity.constEnd()) {
                return info;
            }
            info.heard = true;
            info.lastHeardUtc = it->utcTimestamp;
            return info;
        },
        [this](QDateTime const &utc, QString const &text) {
            writeNoticeTextToUI(utc, text);
        },
        [this](QString const &relayMsg) {
            enqueueMessage(PriorityHigh, relayMsg, -1, nullptr);
        },
        [this](QString const &fromCall, QString const &toCall,
               QString const &messageId) {
            emit aprsClientEnqueueAck(fromCall, toCall, messageId);
        },
        this);

    // hook up the aprs client slots and signals and disposal
    connect(this, &UI_Constructor::aprsClientEnqueueSpot, m_aprsClient,
            &APRSISClient::enqueueSpot);
    connect(this, &UI_Constructor::aprsClientEnqueueThirdParty, m_aprsClient,
            &APRSISClient::enqueueThirdParty);
    connect(this, &UI_Constructor::aprsClientEnqueueAck, m_aprsClient,
            &APRSISClient::enqueueMessageAck);
    connect(this, &UI_Constructor::aprsClientSendReports, m_aprsClient,
            &APRSISClient::sendReports);
    connect(this, &UI_Constructor::aprsClientSetLocalStation, m_aprsClient,
            &APRSISClient::setLocalStation);
    connect(this, &UI_Constructor::aprsClientSetPaused, m_aprsClient,
            &APRSISClient::setPaused);
    connect(this, &UI_Constructor::aprsClientSetServer, m_aprsClient,
            &APRSISClient::setServer);
    connect(this, &UI_Constructor::aprsClientSetSkipPercent, m_aprsClient,
            &APRSISClient::setSkipPercent);
    connect(this, &UI_Constructor::aprsClientSetIncomingRelayEnabled,
            m_aprsClient, &APRSISClient::setIncomingRelayEnabled);
    connect(&m_config, &Configuration::spot_to_aprs_relay_changed, m_aprsClient,
            &APRSISClient::setIncomingRelayEnabled);
    connect(m_aprsClient, &APRSISClient::messageReceived, m_aprsInboundRelay,
            &AprsInboundRelay::onMessageReceived);
    connect(&m_networkThread, &QThread::finished, m_aprsClient,
            &QObject::deleteLater);

    // hook up the psk reporter slots and signals and disposal
    connect(m_pskReporter, &PSKReporter::errorOccurred, this,
            &UI_Constructor::pskReporterError);
    connect(this, &UI_Constructor::pskReporterSendReport, m_pskReporter,
            &PSKReporter::sendReport);
    connect(this, &UI_Constructor::pskReporterAddRemoteStation, m_pskReporter,
            &PSKReporter::addRemoteStation);
    connect(this, &UI_Constructor::pskReporterSetLocalStation, m_pskReporter,
            &PSKReporter::setLocalStation);
    connect(&m_networkThread, &QThread::started, m_pskReporter,
            &PSKReporter::start);
    connect(&m_networkThread, &QThread::finished, m_pskReporter,
            &QObject::deleteLater);

    // hook up the spot client signals and disposal
    connect(this, &UI_Constructor::spotClientEnqueueCmd, m_spotClient,
            &SpotClient::enqueueCmd);
    connect(this, &UI_Constructor::spotClientEnqueueSpot, m_spotClient,
            &SpotClient::enqueueSpot);
    connect(this, &UI_Constructor::spotClientSetLocalStation, m_spotClient,
            &SpotClient::setLocalStation);
    connect(&m_networkThread, &QThread::started, m_spotClient,
            &SpotClient::start);
    connect(&m_networkThread, &QThread::finished, m_spotClient,
            &QObject::deleteLater);

    // hook up sound output stream slots & signals and disposal
    connect(this, &UI_Constructor::initializeAudioOutputStream, m_soundOutput,
            &SoundOutput::setFormat);
    connect(m_soundOutput, &SoundOutput::error, this,
            &UI_Constructor::showSoundOutError);
    connect(m_soundOutput, &SoundOutput::error, &m_config,
            &Configuration::invalidate_audio_output_device);
    connect(this, &UI_Constructor::outAttenuationChanged, m_soundOutput,
            &SoundOutput::setAttenuation);
    connect(&m_audioThread, &QThread::finished, m_soundOutput,
            &QObject::deleteLater);

    connect(this, &UI_Constructor::initializeNotificationAudioOutputStream,
            m_notification, &NotificationAudio::setDevice);
    connect(&m_config, &Configuration::test_notify, this,
            &UI_Constructor::tryNotify);
    connect(this, &UI_Constructor::playNotification, m_notification,
            &NotificationAudio::play);
    connect(&m_notificationAudioThread, &QThread::finished, m_notification,
            &QObject::deleteLater);

    // hook up Modulator slots and disposal
    connect(this, &UI_Constructor::transmitFrequency, m_modulator,
            &Modulator::setAudioFrequency);
    connect(this, &UI_Constructor::endTransmitMessage, m_modulator,
            &Modulator::stop);
    connect(this, &UI_Constructor::tune, m_modulator, &Modulator::tune);
    connect(this, &UI_Constructor::sendMessage, m_modulator, &Modulator::start);
    connect(&m_audioThread, &QThread::finished, m_modulator,
            &QObject::deleteLater);

    // hook up the audio input stream signals, slots and disposal
    connect(this, &UI_Constructor::startAudioInputStream, m_soundInput,
            &SoundInput::start);
    connect(this, &UI_Constructor::suspendAudioInputStream, m_soundInput,
            &SoundInput::suspend);
    connect(this, &UI_Constructor::resumeAudioInputStream, m_soundInput,
            &SoundInput::resume);
    connect(this, &UI_Constructor::finished, m_soundInput, &SoundInput::stop);
    connect(m_soundInput, &SoundInput::error, this,
            &UI_Constructor::showSoundInError);
    connect(m_soundInput, &SoundInput::error, &m_config,
            &Configuration::invalidate_audio_input_device);
    // connect(m_soundInput, &SoundInput::status, this,
    // &UI_Constructor::showStatusMessage);
    connect(&m_audioThread, &QThread::finished, m_soundInput,
            &QObject::deleteLater);

    connect(this, &UI_Constructor::finished, this, &UI_Constructor::close);

    // hook up the detector signals, slots and disposal
    connect(this, &UI_Constructor::FFTSize, m_detector,
            &Detector::setBlockSize);
    connect(m_detector, &Detector::framesWritten, this,
            &UI_Constructor::dataSink);
    connect(&m_audioThread, &QThread::finished, m_detector,
            &QObject::deleteLater);

    // setup the waterfall
    connect(m_wideGraph.data(), &WideGraph::f11f12, this,
            &UI_Constructor::f11f12);
    connect(m_wideGraph.data(), &WideGraph::setXIT, this,
            &UI_Constructor::setXIT);

    connect(this, &UI_Constructor::finished, m_wideGraph.data(),
            &WideGraph::close);

    // setup the log QSO dialog
    connect(m_logDlg.data(), &LogQSO::acceptQSO, this,
            &UI_Constructor::acceptQSO);
    connect(this, &UI_Constructor::finished, m_logDlg.data(), &LogQSO::close);

    // Network message handling
    connect(m_messageClient, &MessageClient::message, this,
            &UI_Constructor::udpNetworkMessage);

    /**
     * @brief Initialize WSJT-X protocol if enabled
     *
     * Creates and configures the WSJT-X message client and mapper when the
     * WSJT-X protocol is enabled. Sets up signal connections for configuration
     * changes and disables the native JSON client if it conflicts with WSJT-X
     * on the same port/address.
     */
    if (m_config.wsjtx_protocol_enabled()) {
        QString id = QApplication::applicationName();
        QString version = QApplication::applicationVersion();
        QString revision = ""; // Get from your version system if available

        m_wsjtxMessageClient = new WSJTXMessageClient{
            id,
            version,
            revision,
            m_config.wsjtx_server_name(),
            m_config.wsjtx_server_port(),
            m_config.wsjtx_interface_names(), // Use selected interfaces
            m_config.wsjtx_TTL(),
            this};

        m_wsjtxMessageClient->enable(m_config.wsjtx_accept_requests());

        m_wsjtxMessageMapper =
            new WSJTXMessageMapper(m_wsjtxMessageClient, this, this);

        // Disable native JSON client if it's using the same port/address as
        // WSJT-X
        if (m_config.wsjtx_server_port() == m_config.udp_server_port() &&
            m_config.wsjtx_server_name() == m_config.udp_server_name()) {
            m_messageClient->set_server_port(0); // Disable native JSON client
        }

        // Connect configuration changes
        connect(&m_config, &Configuration::wsjtx_server_changed,
                [this](QString const &server_name) {
                    m_wsjtxMessageClient->set_server(
                        server_name, m_config.wsjtx_interface_names());
                    // Check if we need to disable native JSON client
                    if (m_config.wsjtx_protocol_enabled() &&
                        m_config.wsjtx_server_port() ==
                            m_config.udp_server_port() &&
                        server_name == m_config.udp_server_name()) {
                        m_messageClient->set_server_port(0);
                    } else if (m_config.wsjtx_protocol_enabled() &&
                               m_config.wsjtx_server_port() !=
                                   m_config.udp_server_port()) {
                        m_messageClient->set_server_port(
                            m_config.udp_server_port());
                    }
                });
        connect(&m_config, &Configuration::wsjtx_server_port_changed,
                [this](quint16 port) {
                    m_wsjtxMessageClient->set_server_port(port);
                    // Check if we need to disable native JSON client
                    if (m_config.wsjtx_protocol_enabled() &&
                        port == m_config.udp_server_port() &&
                        m_config.wsjtx_server_name() ==
                            m_config.udp_server_name()) {
                        m_messageClient->set_server_port(0);
                    } else if (m_config.wsjtx_protocol_enabled() &&
                               port != m_config.udp_server_port()) {
                        m_messageClient->set_server_port(
                            m_config.udp_server_port());
                    }
                });
        connect(&m_config, &Configuration::wsjtx_TTL_changed, this,
                [this](int ttl) {
                    if (m_wsjtxMessageClient) {
                        m_wsjtxMessageClient->set_TTL(ttl);
                    }
                });
        connect(&m_config, &Configuration::wsjtx_interfaces_changed,
                [this](QStringList const &interfaces) {
                    if (m_wsjtxMessageClient) {
                        m_wsjtxMessageClient->set_server(
                            m_config.wsjtx_server_name(), interfaces);
                    }
                });
    }

    // decoder queue handler
    // connect (&m_decodeThread, &QThread::finished, m_notification,
    // &QObject::deleteLater); connect(this, &UI_Constructor::decodedLineReady,
    // this, &UI_Constructor::processDecodedLine);
    connect(&m_decoder, &JS8::Decoder::decodeEvent, this,
            &UI_Constructor::processDecodeEvent);

    m_dateTimeQSOOn = QDateTime{};

    // initialize decoded text font and hook up font change signals
    // defer initialization until after construction otherwise menu
    // fonts do not get set
    QTimer::singleShot(0, this, &UI_Constructor::initialize_fonts);
    connect(&m_config, &Configuration::gui_text_font_changed,
            [this](QFont const &font) { set_application_font(font); });
    connect(&m_config, &Configuration::table_font_changed,
            [this](QFont const &) {
                ui->tableWidgetRXAll->setFont(m_config.table_font());
                ui->tableWidgetCalls->setFont(m_config.table_font());
            });
    connect(&m_config, &Configuration::rx_text_font_changed,
            [this](QFont const &) {
                setTextEditFont(ui->textEditRX, m_config.rx_text_font());
            });
    connect(&m_config, &Configuration::compose_text_font_changed,
            [this](QFont const &) {
                setTextEditFont(ui->extFreeTextMsgEdit,
                                m_config.compose_text_font());
            });
    connect(&m_config, &Configuration::colors_changed, [this]() {
        setTextEditStyle(ui->textEditRX, m_config.color_rx_foreground(),
                         m_config.color_rx_background(),
                         m_config.rx_text_font());
        setTextEditStyle(
            ui->extFreeTextMsgEdit, m_config.color_compose_foreground(),
            m_config.color_compose_background(), m_config.compose_text_font());
        ui->extFreeTextMsgEdit->setFont(m_config.compose_text_font(),
                                        m_config.color_compose_foreground(),
                                        m_config.color_compose_background());

        // update pill enabled state, colors, and rehighlight compose box
        applyPillSettings();
        ui->extFreeTextMsgEdit->highlight();

        // rehighlight
        auto d = ui->textEditRX->document();
        if (d) {
            for (int i = 0; i < d->lineCount(); i++) {
                auto b = d->findBlockByLineNumber(i);

                switch (b.userState()) {
                case State::RX:
                    highlightBlock(b, m_config.rx_text_font(),
                                   m_config.color_rx_foreground(),
                                   QColor(Qt::transparent));
                    break;
                case State::TX:
                    highlightBlock(b, m_config.tx_text_font(),
                                   m_config.color_tx_foreground(),
                                   QColor(Qt::transparent));
                    break;
                }
            }
        }
    });

    // Apply initial pill settings (colors_changed is not emitted at startup)
    applyPillSettings();

    setWindowTitle(program_title());
    buildColumnLabelMap();

    // Hook up working frequencies.

    ui->currentFreq->setCursor(QCursor(Qt::PointingHandCursor));
    ui->currentFreq->setText("14.078 000");
    ui->currentFreq->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *event) {
            QMenu *menu = new QMenu(ui->currentFreq);
            buildFrequencyMenu(menu);
            menu->popup(event->globalPosition().toPoint());
            return true;
        },
        this));

    freqOffsetWidget = new Styles::OffsetSliderWidget(nullptr);
    QLayout *parentLayout = ui->labDialFreqOffset->parentWidget()->layout();
    if (auto *vbox = qobject_cast<QVBoxLayout *>(parentLayout)) {
        int index = vbox->indexOf(ui->labDialFreqOffset);
        vbox->removeWidget(ui->labDialFreqOffset);
        ui->labDialFreqOffset->hide();
        vbox->insertWidget(index, freqOffsetWidget);
    }
    freqOffsetWidget->setOnValueChanged([this](int val) {
        changeFreq(val);
    });

    // Hook up callsign label click to open preferences

    ui->labCallsign->setCursor(QCursor(Qt::PointingHandCursor));
    ui->labCallsign->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *) {
            openSettings(0);
            return true;
        },
        this));

    // hook up configuration signals
    connect(&m_config, &Configuration::transceiver_update, this,
            &UI_Constructor::handle_transceiver_update);
    connect(&m_config, &Configuration::transceiver_failure, this,
            &UI_Constructor::handle_transceiver_failure);
    connect(&m_config, &Configuration::udp_server_name_changed, m_messageClient,
            &MessageClient::set_server_name);
    connect(&m_config, &Configuration::udp_server_port_changed, m_messageClient,
            &MessageClient::set_server_port);

    // Disable native JSON client if WSJT-X protocol is enabled on the same
    // port/address This prevents JSON PING messages from interfering with
    // WSJT-X binary protocol
    connect(
        &m_config, &Configuration::wsjtx_protocol_enabled_changed, this,
        [this](bool enabled) {
            if (enabled &&
                m_config.wsjtx_server_port() == m_config.udp_server_port() &&
                m_config.wsjtx_server_name() == m_config.udp_server_name()) {
                // Disable native JSON client to avoid conflicts with WSJT-X
                // protocol
                m_messageClient->set_server_port(0);
            } else if (!enabled) {
                // Re-enable native JSON client if WSJT-X is disabled
                m_messageClient->set_server_port(m_config.udp_server_port());
            }
        });
    connect(&m_config, &Configuration::band_schedule_changed, this,
            [this]() { this->m_bandHopped = true; });
    connect(&m_config, &Configuration::auto_switch_bands_changed, this,
            [this](bool auto_switch_bands) {
                this->m_bandHopped = this->m_bandHopped || auto_switch_bands;
            });
    connect(&m_config, &Configuration::manual_band_hop_requested, this,
            &UI_Constructor::manualBandHop);
    connect(&m_config, &Configuration::enumerating_audio_devices,
            [this]() { showStatusMessage(tr("Enumerating audio devices")); });

    // set up configurations menu
    connect(m_multi_settings, &MultiSettings::configurationNameChanged,
            [this](QString const &name) {
                if ("Default" != name) {
                    config_label.setText(name);
                    config_label.show();
                } else {
                    config_label.hide();
                }
            });
    m_multi_settings->create_menu_actions(this, ui->menuConfig);
    m_configurations_button = m_rigErrorMessageBox.addButton(
        tr("Configurations..."), QMessageBox::ActionRole);
    connect(ui->extFreeTextMsgEdit, &QTextEdit::textChanged,
            [this]() { currentTextChanged(); });

    m_guiTimer.setTimerType(Qt::PreciseTimer);
    m_guiTimer.setSingleShot(true);
    connect(&m_guiTimer, &QTimer::timeout, this, &UI_Constructor::guiUpdate);
    m_guiTimer.start(UI_POLL_INTERVAL_MS);

    pttReleaseTimer.setTimerType(Qt::PreciseTimer);
    pttReleaseTimer.setSingleShot(true);
    connect(&pttReleaseTimer, &QTimer::timeout, this, &UI_Constructor::stopTx2);

    logQSOTimer.setSingleShot(true);
    connect(&logQSOTimer, &QTimer::timeout, this,
            &UI_Constructor::on_logQSOButton_clicked);

    tuneButtonTimer.setSingleShot(true);
    connect(&tuneButtonTimer, &QTimer::timeout, this,
            &UI_Constructor::end_tuning);

    tuneATU_Timer.setSingleShot(true);
    connect(&tuneATU_Timer, &QTimer::timeout, this,
            &UI_Constructor::stopTuneATU);

    TxAgainTimer.setSingleShot(true);
    connect(&TxAgainTimer, &QTimer::timeout, this, &UI_Constructor::TxAgain);

    connect(m_wideGraph.data(), &WideGraph::changeFreq, this,
            &UI_Constructor::changeFreq);
    connect(m_wideGraph.data(), &WideGraph::qsy, this, &UI_Constructor::qsy);

    // DriftingDateTime management:
    connect(m_wideGraph.data(), &WideGraph::want_new_drift,
            &DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::setDrift);

    // Distribute Drift change:
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, this,
            &UI_Constructor::onDriftChanged);
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, m_wideGraph.data(),
            &WideGraph::onDriftChanged);
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, m_cq_loop,
            &TxLoop::onDriftChange);
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, m_hb_loop,
            &TxLoop::onDriftChange);

    // HB and CQ loop:
    // For now, disable HB loop while CQ loop runs and vice versa:
    connect(m_cq_loop, &TxLoop::nextActivityChanged, this,
            [this](const QDateTime &) { this->m_hb_loop->onLoopCancel(); });
    connect(m_hb_loop, &TxLoop::nextActivityChanged, this,
            [this](const QDateTime &) { this->m_cq_loop->onLoopCancel(); });
    // It is not advisable to send a HB in one period and a CQ in the very next,
    // or a CQ first and then a HB too soon, so that transmissions of people
    // interested in the QSO might be drowned.
    //
    // We could conceivably devise a clean conflict resolution mechanism
    // disallowing automatic transmissions of one type if an automatic
    // transmission of the other type has happened recently. In the absence of
    // such a mechanism, just do either one or the other.
    //
    // People who call CQ are expected to monitor somewhat closely, so they will
    // have no problems triggering HBs at will.

    // Propagate tx submode changes to the CQ and HB loop:
    connect(this, &UI_Constructor::submodeChanged, this->m_hb_loop,
            &TxLoop::onModeChange);
    connect(this, &UI_Constructor::submodeChanged, this->m_cq_loop,
            &TxLoop::onModeChange);

    // When the loops are switched off, tell the UI:
    connect(m_hb_loop, &TxLoop::canceled, ui->hbMacroButton,
            [this]() { this->ui->hbMacroButton->setChecked(false); });
    connect(m_cq_loop, &TxLoop::canceled, ui->cqMacroButton,
            [this]() { this->ui->cqMacroButton->setChecked(false); });

    // The loops can trigger transmissions. That is what they are for.
    connect(m_hb_loop, &TxLoop::triggerTxNow, this,
            [this]() { this->sendHB(); });
    connect(m_cq_loop, &TxLoop::triggerTxNow, this,
            [this]() { this->sendCQ(true); });

    // Something like this would be nice to have:
    // connect(m_config, &Configuration::txDelayChanged, m_cq_loop,
    // &TxLoop::onTxDelayChange); connect(m_config,
    // &Configuration::txDelayChanged, m_hb_loop, &TxLoop::onTxDelayChange); But
    // the pertaining signals are not offered by Configuration, and the code was
    // somewhat of a beast to get into to change that, so some equivalent is
    // done in a pedestrian way via our polling routine.  That pedestrian code
    // also handles conversion of incoming tx delay in (double) seconds to
    // outgoing (qint64) milliseconds.

    decodeBusy(false);

    m_msg[0][0] = 0;

    displayDialFrequency();
    readSettings(); // Restore user's setup params

    {
        std::lock_guard<std::mutex> lock(fftw_mutex);
        fftwf_import_wisdom_from_filename(wisdomFileName());
    }

    m_networkThread.start(m_networkThreadPriority);
    m_audioThread.start(m_audioThreadPriority);
    m_notificationAudioThread.start(m_notificationAudioThreadPriority);
    m_decoder.start(m_decoderThreadPriority);

    Q_EMIT startAudioInputStream(m_config.audio_input_device(),
                                 m_framesAudioInputBuffered, m_detector,
                                 m_config.audio_input_channel());
    Q_EMIT initializeAudioOutputStream(
        m_config.audio_output_device(),
        AudioDevice::Mono == m_config.audio_output_channel() ? 1 : 2,
        m_msAudioOutputBuffered);
    Q_EMIT initializeNotificationAudioOutputStream(
        m_config.notification_audio_output_device(), m_msAudioOutputBuffered);
    Q_EMIT transmitFrequency(freq() + m_XIT);

    enable_DXCC_entity(
        m_config
            .DXCC()); // sets text window proportions and (re)inits the logbook

    // this must be done before initializing the mode as some modes need
    // to turn off split on the rig e.g. WSPR
    m_config.transceiver_online();

    setupJS8();

    Q_EMIT transmitFrequency(freq() + m_XIT);

    statusChanged();

    connect(&minuteTimer, &QTimer::timeout, this,
            &UI_Constructor::on_the_minute);
    minuteTimer.setSingleShot(true);
    minuteTimer.start(ms_minute_error() + 60 * 1000);

    QTimer::singleShot(0, this, &UI_Constructor::checkStartupWarnings);

    // UI Customizations & Tweaks
    ui->horizontalLayoutBand->insertSpacing(1, 6);
    ui->horizontalLayoutBand->insertWidget(2, m_wideGraph.data(), 1);
    ui->horizontalLayoutBand->insertSpacing(3, 8);

    // remove disabled menus from the menu bar
    foreach (auto action, ui->menuBar->actions()) {
        if (action->isEnabled()) {
            continue;
        }
        ui->menuBar->removeAction(action);
    }

    ui->actionModeAutoreply->setChecked(m_config.autoreply_on_at_startup());
    ui->spotButton->setChecked(m_config.spot_to_reporting_networks());

    QActionGroup *modeActionGroup = new QActionGroup(this);
    ui->actionModeJS8Normal->setActionGroup(modeActionGroup);
    ui->actionModeJS8Fast->setActionGroup(modeActionGroup);
    ui->actionModeJS8Turbo->setActionGroup(modeActionGroup);
    ui->actionModeJS8Slow->setActionGroup(modeActionGroup);
    ui->actionModeJS8Ultra->setActionGroup(modeActionGroup);

    ui->modeButton->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *event) {
            ui->menuModeJS8->popup(event->globalPosition().toPoint());
            return true;
        },
        this));

    if (!JS8_ENABLE_JS8A)
        ui->actionModeJS8Normal->setVisible(false);
    if (!JS8_ENABLE_JS8B)
        ui->actionModeJS8Fast->setVisible(false);
    if (!JS8_ENABLE_JS8C)
        ui->actionModeJS8Turbo->setVisible(false);
    if (!JS8_ENABLE_JS8E)
        ui->actionModeJS8Slow->setVisible(false);
    if (!JS8_ENABLE_JS8I)
        ui->actionModeJS8Ultra->setVisible(false);

    // prep
    prepareMonitorControls();
    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());

    ui->extFreeTextMsgEdit->installEventFilter(new EventFilter::EnterKeyPress(
        [this](QKeyEvent *const event) {
            if (event->modifiers() & Qt::ShiftModifier)
                return false;
            if (ui->extFreeTextMsgEdit->isReadOnly())
                return false;

            if (ui->extFreeTextMsgEdit->toPlainText().trimmed().isEmpty())
                return true;
            if (!ensureCanTransmit())
                return true;
            if (!ensureCallsignSet(true))
                return true;

            toggleTx(true);
            return true;
        },
        this));

    ui->textEditRX->viewport()->installEventFilter(
        new EventFilter::MouseButtonDblClick(
            [this](QMouseEvent *) {
                QTimer::singleShot(150, this, [this]() {
                    // When we double click the rx window, we send the selected
                    // text to the log dialog when the text could be an snr
                    // value prefixed with a - or +, we extend the selection to
                    // include it.

                    auto textCursor = ui->textEditRX->textCursor();
                    auto text = textCursor.selectedText();

                    if (text.isEmpty())
                        return;

                    auto const start = textCursor.selectionStart();
                    auto const end = textCursor.selectionEnd();

                    textCursor.clearSelection();
                    textCursor.setPosition(start);
                    textCursor.movePosition(QTextCursor::PreviousCharacter,
                                            QTextCursor::MoveAnchor);
                    textCursor.movePosition(QTextCursor::NextCharacter,
                                            QTextCursor::KeepAnchor,
                                            1 + end - start);

                    if (auto const prev = textCursor.selectedText();
                        prev.startsWith("-") || prev.startsWith("+")) {
                        ui->textEditRX->setTextCursor(textCursor);
                        text = prev;
                    }

                    m_logDlg->acceptText(text);
                });
                return false;
            },
            this));

    auto clearActionSep = new QAction(nullptr);
    clearActionSep->setSeparator(true);

    auto clearActionAll = new QAction(QString("Clear All Lists"), nullptr);
    connect(clearActionAll, &QAction::triggered, this, [this]() {
        if (QMessageBox::Yes !=
            QMessageBox::question(
                this, "Clear All Activity",
                "Are you sure you would like to clear all activity?",
                QMessageBox::Yes | QMessageBox::No)) {
            return;
        }

        clearActivity();
    });

    // setup tablewidget context menus
    auto clearAction1 = new QAction(QString("Clear"), ui->textEditRX);
    connect(clearAction1, &QAction::triggered, this,
            [this]() { clearRXActivity(); });

    auto saveAction = new QAction(QString("Save As..."), ui->textEditRX);
    connect(saveAction, &QAction::triggered, this, [this]() {
        auto writePath =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        auto writeDir = QDir(writePath);
        auto defaultFilename = writeDir.absoluteFilePath(
            QString("js8call-%1.txt")
                .arg(DriftingDateTime::currentDateTimeUtc().toString(
                    "yyyyMMdd")));

        QString selectedFilter = "*.txt";

        auto filename = QFileDialog::getSaveFileName(
            this, "Save As...", defaultFilename,
            "Text files (*.txt);; All files (*)", &selectedFilter);
        if (filename.isEmpty()) {
            return;
        }

        auto text = ui->textEditRX->toPlainText();
        QFile f(filename);
        if (f.open(QIODevice::Truncate | QIODevice::WriteOnly |
                   QIODevice::Text)) {
            QTextStream stream(&f);
            stream << text;
        }
    });

    ui->textEditRX->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->textEditRX, &QTableWidget::customContextMenuRequested, this,
        [this, clearAction1, clearActionAll, saveAction](QPoint const &point) {
            QMenu *menu = new QMenu(ui->textEditRX);

            buildEditMenu(menu, ui->textEditRX);

            menu->addSeparator();

            menu->addAction(clearAction1);
            menu->addAction(clearActionAll);

            menu->addSeparator();
            menu->addAction(saveAction);

            menu->popup(ui->textEditRX->mapToGlobal(point));
        });

    auto clearAction2 = new QAction(QString("Clear"), ui->extFreeTextMsgEdit);
    connect(clearAction2, &QAction::triggered, this, [this]() {
        resetMessage();
        m_lastTxMessage.clear();
    });

    auto restoreAction = new QAction(QString("Restore Previous Message"),
                                     ui->extFreeTextMsgEdit);
    connect(restoreAction, &QAction::triggered, this,
            [this]() { this->restoreMessage(); });

    ui->extFreeTextMsgEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->extFreeTextMsgEdit, &QTableWidget::customContextMenuRequested, this,
        [this, clearAction2, clearActionAll,
         restoreAction](QPoint const &point) {
            QMenu *menu = new QMenu(ui->extFreeTextMsgEdit);

            auto selectedCall = callsignSelected();
            bool missingCallsign = selectedCall.isEmpty();

            buildSuggestionsMenu(menu, ui->extFreeTextMsgEdit, point);

            restoreAction->setDisabled(m_lastTxMessage.isEmpty());
            menu->addAction(restoreAction);

            auto savedMenu = menu->addMenu("Saved Messages...");
            buildSavedMessagesMenu(savedMenu);

            auto directedMenu =
                menu->addMenu(QString("Directed to %1...").arg(selectedCall));
            directedMenu->setDisabled(missingCallsign);
            buildQueryMenu(directedMenu, selectedCall);

            auto relayMenu = menu->addMenu("Relay via...");
            relayMenu->setDisabled(
                ui->extFreeTextMsgEdit->toPlainText().isEmpty() ||
                m_callActivity.isEmpty());
            buildRelayMenu(relayMenu);

            menu->addSeparator();

            buildEditMenu(menu, ui->extFreeTextMsgEdit);

            menu->addSeparator();

            menu->addAction(clearAction2);
            menu->addAction(clearActionAll);

            menu->popup(ui->extFreeTextMsgEdit->mapToGlobal(point));

            displayActivity(true);
        });

    auto clearAction3 = new QAction(QString("Clear"), ui->tableWidgetRXAll);
    connect(clearAction3, &QAction::triggered, this,
            [this]() { clearBandActivity(); });

    auto removeActivity =
        new QAction(QString("Remove Activity"), ui->tableWidgetRXAll);
    connect(removeActivity, &QAction::triggered, this, [this]() {
        if (ui->tableWidgetRXAll->selectedItems().isEmpty()) {
            return;
        }

        auto selectedItems = ui->tableWidgetRXAll->selectedItems();
        int selectedOffset = selectedItems.first()->data(Qt::UserRole).toInt();

        m_bandActivity.remove(selectedOffset);
        displayActivity(true);
    });

    auto logAction = new QAction(QString("Log..."), ui->tableWidgetCalls);
    connect(logAction, &QAction::triggered, this,
            &UI_Constructor::on_logQSOButton_clicked);

    // Disable default header mouseover and click behaviors, they are confusing
    // to users because they give the appearance of allowing sorting by header
    // clicks, which is not actually implemented
    ui->tableWidgetRXAll->horizontalHeader()->setHighlightSections(false);
    ui->tableWidgetRXAll->horizontalHeader()->setSectionsClickable(false);

    ui->tableWidgetRXAll->horizontalHeader()->setContextMenuPolicy(
        Qt::CustomContextMenu);
    connect(
        ui->tableWidgetRXAll->horizontalHeader(),
        &QHeaderView::customContextMenuRequested, this,
        [this](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetRXAll);

            QMenu *sortByMenu = menu->addMenu("Sort By...");
            buildBandActivitySortByMenu(sortByMenu);

            QMenu *showColumnsMenu = menu->addMenu("Show Columns...");
            buildShowColumnsMenu(showColumnsMenu, "band");

            menu->popup(
                ui->tableWidgetRXAll->horizontalHeader()->mapToGlobal(point));
        });

    ui->tableWidgetRXAll->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->tableWidgetRXAll, &QTableWidget::customContextMenuRequested, this,
        [this, clearAction3, clearActionAll, removeActivity,
         logAction](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetRXAll);

            // clear the selection of the call widget on right click
            // but only if the table has rows.
            if (ui->tableWidgetRXAll->rowAt(point.y()) != -1) {
                ui->tableWidgetCalls->selectionModel()->clearSelection();
            }

            QString selectedCall = callsignSelected();
            bool missingCallsign = selectedCall.isEmpty();
            bool isAllCall = isAllCallIncluded(selectedCall);

            int selectedOffset = -1;
            if (!ui->tableWidgetRXAll->selectedItems().isEmpty()) {
                auto selectedItems = ui->tableWidgetRXAll->selectedItems();
                selectedOffset =
                    selectedItems.first()->data(Qt::UserRole).toInt();
            }

            if (selectedOffset != -1) {
                auto qsyAction = menu->addAction(
                    QString("Jump to %1Hz").arg(selectedOffset));
                connect(qsyAction, &QAction::triggered, this,
                        [this, selectedOffset]() {
                            changeFreq(selectedOffset);
                        });

                if (m_wideGraph->filterEnabled()) {
                    auto filterQsyAction = menu->addAction(
                        QString("Center filter at %1Hz").arg(selectedOffset));
                    connect(filterQsyAction, &QAction::triggered, this,
                            [this, selectedOffset]() {
                                m_wideGraph->setFilterCenter(selectedOffset);
                            });
                }

                auto items = m_bandActivity.value(selectedOffset);
                if (!items.isEmpty()) {
                    int submode = items.last().submode;
                    auto speed = JS8::Submode::name(submode);
                    if (submode != m_nSubMode) {
                        auto qrqAction =
                            menu->addAction(QString("Jump to %1%2 speed")
                                                .arg(speed.left(1))
                                                .arg(speed.mid(1).toLower()));
                        connect(qrqAction, &QAction::triggered, this,
                                [this, submode]() { setSubmode(submode); });
                    }

                    int tdrift = -int(items.last().tdrift * 1000);
                    auto qtrAction = menu->addAction(
                        QString("Jump to %1 ms time drift").arg(tdrift));
                    connect(qtrAction, &QAction::triggered, this,
                            [this, tdrift]() { setDrift(tdrift); });
                }

                menu->addSeparator();
            }

            menu->addAction(logAction);
            logAction->setDisabled(missingCallsign || isAllCall);

            menu->addSeparator();

            auto savedMenu = menu->addMenu("Saved Messages...");
            buildSavedMessagesMenu(savedMenu);

            auto directedMenu =
                menu->addMenu(QString("Directed to %1...").arg(selectedCall));
            directedMenu->setDisabled(missingCallsign);
            buildQueryMenu(directedMenu, selectedCall);

            auto relayAction = buildRelayAction(selectedCall);
            relayAction->setText(QString("Relay via %1...").arg(selectedCall));
            relayAction->setDisabled(missingCallsign);
            menu->addActions({relayAction});

            auto deselectAction =
                menu->addAction(QString("Deselect %1").arg(selectedCall));
            deselectAction->setDisabled(missingCallsign);
            connect(deselectAction, &QAction::triggered, this, [this]() {
                ui->tableWidgetRXAll->clearSelection();
                ui->tableWidgetCalls->clearSelection();
                messagePanel_->setCall("%");
            });

            menu->addSeparator();

            removeActivity->setDisabled(selectedOffset == -1);
            menu->addAction(removeActivity);

            menu->addSeparator();
            menu->addAction(clearAction3);
            menu->addAction(clearActionAll);

            menu->popup(ui->tableWidgetRXAll->mapToGlobal(point));

            displayActivity(true);
        });

    auto clearAction4 =
        new QAction(QString("Clear Entire List"), ui->tableWidgetCalls);
    connect(clearAction4, &QAction::triggered, this,
            [this]() { clearCallActivity(); });

    auto addStation = new QAction(QString("Add New Station or Group..."),
                                  ui->tableWidgetCalls);
    connect(addStation, &QAction::triggered, this, [this]() {
        bool ok = false;
        QString callsign =
            QInputDialog::getText(this, tr("Add New Station or Group"),
                                  tr("Station or Group Callsign:"),
                                  QLineEdit::Normal, "", &ok)
                .toUpper()
                .trimmed();
        if (!ok || callsign.trimmed().isEmpty()) {
            return;
        }

        // if we're adding allcall, turn off allcall avoidance
        if (callsign == "@ALLCALL") {
            m_config.set_avoid_allcall(false);
        } else if (callsign.startsWith("@")) {
            if (Varicode::isCompoundCallsign(callsign)) {
                m_config.addGroup(callsign);
            } else {
                JS8MessageBox::critical_message(
                    this, QString("%1 is not a valid group").arg(callsign));
            }

        } else {
            if (Varicode::isValidCallsign(callsign, nullptr)) {
                CallDetail cd = {};
                cd.call = callsign;
                m_callActivity[callsign] = cd;
            } else {
                JS8MessageBox::critical_message(
                    this, QString("%1 is not a valid callsign or group")
                              .arg(callsign));
            }
        }

        displayActivity(true);
    });

    auto removeStation =
        new QAction(QString("Remove Station"), ui->tableWidgetCalls);
    connect(removeStation, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        if (selectedCall == "@ALLCALL") {
            m_config.set_avoid_allcall(true);
        } else if (selectedCall.startsWith("@")) {
            m_config.removeGroup(selectedCall);
        } else if (m_callActivity.contains(selectedCall)) {
            m_callActivity.remove(selectedCall);
        }

        displayActivity(true);
    });
          
    auto blockStation =
        new QAction(QString("Block This Station"), ui->tableWidgetCalls);
    connect(blockStation, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty() || selectedCall.startsWith("@")) {
            return;
        }

        m_config.add_to_rx_callsign_blocklist(selectedCall);

        m_rxBlockedOffsets.clear();
        displayActivity(true);
    });

    connect(ui->actionShow_Message_Inbox, &QAction::toggled, this,
            [this](bool checked) {

        if (checked) {
            ensureMessageDock();
            messageDock_->show();
            messageDock_->raise();

            /*
             * Disable the automatic selected call filter now that the inbox lives
             * in a dockable panel
            QString selectedCall = callsignSelected();
            if (selectedCall.isEmpty()) selectedCall = "%";
            messagePanel_->setCall(selectedCall);
            */
          messagePanel_->setCall("%");
        } else {
            if (messageDock_) {
                // pick ONE: hide or close; for docks, hide is simplest
                messageDock_->hide();
            }
        }
    });

    // When a message is added to the inbox, refresh the message panel
    connect(this, &UI_Constructor::messageAdded, messagePanel_, &MessagePanel::refresh);

    auto historyAction =
        new QAction(QString("Show Message Inbox..."), ui->tableWidgetCalls);
    connect(historyAction, &QAction::triggered, this, [this]() {
      ensureMessageDock();
      messageDock_->show();
      messageDock_->raise();

      QString selectedCall = callsignSelected();
      messagePanel_->setCall(selectedCall);
    });

    auto localMessageAction =
        new QAction(QString("Store Message..."), ui->tableWidgetCalls);
    connect(localMessageAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        auto m = new MessageReplyDialog(this);
        m->setWindowTitle("Message");
        m->setLabel(
            QString("Store this message locally for %1:").arg(selectedCall));
        if (m->exec() != QMessageBox::Accepted) {
            return;
        }

        CommandDetail d = {};
        d.cmd = " MSG ";
        d.to = selectedCall;
        d.from = m_config.my_callsign();
        d.relayPath = d.from;
        d.text = m->textValue();
        d.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
        d.submode = m_nSubMode;

        addCommandToStorage("STORE", d);
    });

    // Disable default header mouseover and click behaviors, they are confusing
    // to users because they give the appearance of allowing sorting by header
    // clicks, which is not actually implemented
    ui->tableWidgetCalls->horizontalHeader()->setHighlightSections(false);
    ui->tableWidgetCalls->horizontalHeader()->setSectionsClickable(false);

    ui->tableWidgetCalls->horizontalHeader()->setContextMenuPolicy(
        Qt::CustomContextMenu);
    connect(
        ui->tableWidgetCalls->horizontalHeader(),
        &QHeaderView::customContextMenuRequested, this,
        [this](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetCalls);

            QMenu *sortByMenu = menu->addMenu("Sort By...");
            buildCallActivitySortByMenu(sortByMenu);

            QMenu *showColumnsMenu = menu->addMenu("Show Columns...");
            buildShowColumnsMenu(showColumnsMenu, "call");

            menu->popup(
                ui->tableWidgetCalls->horizontalHeader()->mapToGlobal(point));
        });

    ui->tableWidgetCalls->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
            ui->tableWidgetCalls, &QTableWidget::customContextMenuRequested, this,
            [this, logAction, historyAction, localMessageAction, clearAction4,
             clearActionAll, addStation, removeStation, blockStation](QPoint const &point) {
                
            QMenu *menu = new QMenu(ui->tableWidgetCalls);

            // clear the selection of the call widget on right click
            // but only if the table has rows.
            if (ui->tableWidgetCalls->rowAt(point.y()) != -1) {
                ui->tableWidgetRXAll->selectionModel()->clearSelection();
            }

            QString selectedCall = callsignSelected();
            bool isAllCall = isAllCallIncluded(selectedCall);
            // bool isGroupCall = isGroupCallIncluded(selectedCall);
            bool missingCallsign = selectedCall.isEmpty();

            if (!missingCallsign && !isAllCall) {
                int selectedOffset = m_callActivity[selectedCall].offset;
                if (selectedOffset != -1) {
                    auto qsyAction = menu->addAction(
                        QString("Jump to %1Hz").arg(selectedOffset));
                    connect(qsyAction, &QAction::triggered, this,
                            [this, selectedOffset]() {
                                changeFreq(selectedOffset);
                            });

                    if (m_wideGraph->filterEnabled()) {
                        auto filterQsyAction =
                            menu->addAction(QString("Center filter at %1Hz")
                                                .arg(selectedOffset));
                        connect(filterQsyAction, &QAction::triggered, this,
                                [this, selectedOffset]() {
                                    m_wideGraph->setFilterCenter(
                                        selectedOffset);
                                });
                    }

                    int submode = m_callActivity[selectedCall].submode;
                    auto speed = JS8::Submode::name(submode);
                    if (submode != m_nSubMode) {
                        auto qrqAction =
                            menu->addAction(QString("Jump to %1%2 speed")
                                                .arg(speed.left(1))
                                                .arg(speed.mid(1).toLower()));
                        connect(qrqAction, &QAction::triggered, this,
                                [this, submode]() { setSubmode(submode); });
                    }

                    int tdrift =
                        -int(m_callActivity[selectedCall].tdrift * 1000);
                    auto qtrAction = menu->addAction(
                        QString("Jump to %1 ms time drift").arg(tdrift));
                    connect(qtrAction, &QAction::triggered, this,
                            [this, tdrift]() { setDrift(tdrift); });

                    menu->addSeparator();
                }
            }

            menu->addAction(logAction);
            logAction->setDisabled(missingCallsign || isAllCall);

            menu->addAction(historyAction);
            historyAction->setDisabled(missingCallsign || isAllCall ||
                                       !hasMessageHistory(selectedCall));

            menu->addAction(localMessageAction);
            localMessageAction->setDisabled(missingCallsign || isAllCall);

            menu->addSeparator();

            auto savedMenu = menu->addMenu("Saved Messages...");
            buildSavedMessagesMenu(savedMenu);

            auto directedMenu =
                menu->addMenu(QString("Directed to %1...").arg(selectedCall));
            directedMenu->setDisabled(missingCallsign);
            buildQueryMenu(directedMenu, selectedCall);

            auto relayAction = buildRelayAction(selectedCall);
            relayAction->setText(QString("Relay via %1...").arg(selectedCall));
            relayAction->setDisabled(missingCallsign || isAllCall);
            menu->addActions({relayAction});

            auto deselect =
                menu->addAction(QString("Deselect %1").arg(selectedCall));
            deselect->setDisabled(missingCallsign);
            connect(deselect, &QAction::triggered, this, [this]() {
                ui->tableWidgetRXAll->clearSelection();
                ui->tableWidgetCalls->clearSelection();
                messagePanel_->setCall("%");
            });

            menu->addSeparator();

            menu->addAction(addStation);
            removeStation->setDisabled(missingCallsign);
            removeStation->setText(selectedCall.startsWith("@")
                                       ? "Remove This Group"
                                       : "Remove This Station");
            menu->addAction(removeStation);
            
            blockStation->setDisabled(missingCallsign || isAllCall ||
                                      selectedCall.startsWith("@"));

            menu->addAction(blockStation);

            menu->addSeparator();
            menu->addAction(clearAction4);
            menu->addAction(clearActionAll);

            menu->popup(ui->tableWidgetCalls->mapToGlobal(point));
        });

    connect(ui->tableWidgetRXAll->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &UI_Constructor::tableSelectionChanged);
    connect(ui->tableWidgetCalls->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &UI_Constructor::tableSelectionChanged);

    auto p = ui->tableWidgetRXAll->palette();
    p.setColor(QPalette::Inactive, QPalette::Highlight,
               p.color(QPalette::Active, QPalette::Highlight));
    ui->tableWidgetRXAll->setPalette(p);

    p = ui->tableWidgetCalls->palette();
    p.setColor(QPalette::Inactive, QPalette::Highlight,
               p.color(QPalette::Active, QPalette::Highlight));
    ui->tableWidgetCalls->setPalette(p);

    ui->hbMacroButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->hbMacroButton, &QPushButton::customContextMenuRequested, this,
            [this](QPoint const &point) {
                QMenu *menu = new QMenu(ui->hbMacroButton);

                buildHeartbeatMenu(menu);

                menu->popup(ui->hbMacroButton->mapToGlobal(point));
            });

    ui->cqMacroButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->cqMacroButton, &QPushButton::customContextMenuRequested, this,
            [this](QPoint const &point) {
                QMenu *menu = new QMenu(ui->cqMacroButton);

                buildCQMenu(menu);

                menu->popup(ui->cqMacroButton->mapToGlobal(point));
            });

    // Don't block heartbeat's first run...
    m_lastTxStartTime = DriftingDateTime::currentDateTimeUtc().addSecs(-300);

    // But do block the decoder's first run until 50% through next transmit
    // period
    m_lastTxStopTime = nextTransmitCycle().addSecs(-m_TRperiod / 2);

    int width = 75;
    /*
    QList<QPushButton*> btns;
    foreach(auto child, ui->buttonGrid->children()){
        if(!child->isWidgetType()){
            continue;
        }

        if(!child->objectName().contains("Button")){
            continue;
        }

        auto b = qobject_cast<QPushButton*>(child);
        width = qMax(width, b->geometry().width());
        btns.append(b);
    }
    */
    foreach (auto child, ui->buttonGrid->children()) {
        if (!child->isWidgetType()) {
            continue;
        }

        if (!child->objectName().contains("Button")) {
            continue;
        }

        auto b = qobject_cast<QPushButton *>(child);
        b->setCursor(QCursor(Qt::PointingHandCursor));
    }
    auto buttonLayout = ui->buttonGrid->layout();
    auto gridButtonLayout = qobject_cast<QGridLayout *>(buttonLayout);
    gridButtonLayout->setColumnMinimumWidth(0, width);
    gridButtonLayout->setColumnMinimumWidth(1, width);
    gridButtonLayout->setColumnMinimumWidth(2, width);
    gridButtonLayout->setColumnStretch(0, 1);
    gridButtonLayout->setColumnStretch(1, 1);
    gridButtonLayout->setColumnStretch(2, 1);

    // dial up and down buttons sizes
    ui->dialFreqUpButton->setFixedSize(30, 24);
    ui->dialFreqDownButton->setFixedSize(30, 24);

    // Prepare spotting configuration...
    prepareApi();
    prepareSpotting();

    displayActivity(true);

    m_txTextDirtyDebounce.setSingleShot(true);
    connect(&m_txTextDirtyDebounce, &QTimer::timeout, this,
            &UI_Constructor::refreshTextDisplay);
    qCDebug(mainwindow_js8)
        << "Main window constructor has done all connect (aka plumbing) work.";

    m_TxDelay = m_config.txDelay();
    m_hb_loop->onTxDelayChange(llround(m_TxDelay * 1000.0));
    m_cq_loop->onTxDelayChange(llround(m_TxDelay * 1000.0));
    m_hb_loop->onPlumbingCompleted();
    m_cq_loop->onPlumbingCompleted();
    DriftingDateTimeSingleton::getSingleton().onPlumbingCompleted();
    qCDebug(mainwindow_js8)
        << "Initialization with onPlumbingCompleted() has completed.";

    QTimer::singleShot(500, this, &UI_Constructor::initializeDummyData);
    QTimer::singleShot(500, this, &UI_Constructor::initializeGroupMessage);

    // this must be the last statement of constructor
    if (!m_valid)
        throw std::runtime_error{"Fatal initialization exception"};
}
