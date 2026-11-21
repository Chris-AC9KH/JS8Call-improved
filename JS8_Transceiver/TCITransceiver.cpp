/**
 * @file TCITransceiver.cpp
 * @brief TCI transceiver implementation
 *
 * This class adapts JS8Call's Transceiver/PollingTransceiver interface to the
 * TCIClient WebSocket CAT client.
 */

#include "TCITransceiver.h"

#include "JS8_Network/TCIClient.h"

#include <QEventLoop>
#include <QtGlobal>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>

#include "moc_TCITransceiver.cpp"

Q_DECLARE_LOGGING_CATEGORY(tcitransceiver_js8)

QString const TCITransceiver::transceiver_name_{"TCI"};

TCITransceiver::TCITransceiver(TransceiverFactory::ParameterPack const &params,
                               TCISession *session,
                               QObject *parent)
    : PollingTransceiver{params.poll_interval, parent},
      m_session{session}
{
    m_url = QUrl(params.network_port);

    if (!m_url.isValid() || m_url.scheme().isEmpty()) {
        m_url = QUrl(QStringLiteral("ws://%1").arg(params.network_port));
    }

    qWarning() << "TCITransceiver ctor entry"
              << "this=" << this
              << "session=" << m_session;

    if (!m_session) {
        throw error{tr("TCI session is not available.")};
    }

    m_client = m_session->client();

    qWarning() << "TCITransceiver ctor client"
               << "this=" << this
               << "session=" << m_session
               << "client=" << m_client;

    if (!m_client) {
        throw error{tr("TCI client is not available.")};
    }

    connect(m_client, &TCIClient::frequencyChanged,
            this, &TCITransceiver::handle_frequency_changed);

    connect(m_client, &TCIClient::txFrequencyChanged,
        this, &TCITransceiver::handle_tx_frequency_changed);

    connect(m_client, &TCIClient::splitChanged,
            this, &TCITransceiver::handle_split_changed);

    connect(m_client, &TCIClient::modeChanged,
            this, &TCITransceiver::handle_mode_changed);

    connect(m_client, &TCIClient::pttChanged,
            this, &TCITransceiver::handle_ptt_changed);

    connect(m_client, &TCIClient::rxAudioFrame,
        this, &TCITransceiver::handle_rx_audio_frame);

    connect(m_client, &TCIClient::ready,
        this, &TCITransceiver::handle_ready,
        Qt::QueuedConnection);

    connect(m_client, &TCIClient::disconnected,
            this, &TCITransceiver::handle_disconnected,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::error,
            this, &TCITransceiver::handle_error,
            Qt::QueuedConnection);
}

QUrl TCITransceiver::make_url(TransceiverFactory::ParameterPack const &params) const
{
    QString endpoint = params.network_port.trimmed();

    if (endpoint.isEmpty()) {
        endpoint = QStringLiteral("127.0.0.1:40001");
    }

    if (endpoint.startsWith(QStringLiteral("ws://"), Qt::CaseInsensitive) ||
        endpoint.startsWith(QStringLiteral("wss://"), Qt::CaseInsensitive)) {
        return QUrl{endpoint};
    }

    // Accept either "host:port" or just "host".
    QString host = endpoint;
    quint16 port = 40001;

    const int colon = endpoint.lastIndexOf(':');

    if (colon > 0 && colon < endpoint.size() - 1) {
        bool ok = false;
        const auto parsed = endpoint.mid(colon + 1).toUShort(&ok);

        if (ok && parsed > 0) {
            host = endpoint.left(colon);
            port = parsed;
        }
    }

    return QUrl{QStringLiteral("ws://%1:%2").arg(host).arg(port)};
}

int TCITransceiver::do_start()
{
    TRACE_CAT("TCITransceiver", "opening" << m_url);

    if (!m_session) {
        throw error{tr("TCI session is not available.")};
    }

    if (!m_client) {
        m_client = m_session->client();
    }

    if (!m_client) {
        throw error{tr("TCI client is not available.")};
    }

    ready_ = false;
    started_ = false;
    stopping_ = false;
    should_reconnect_ = true;
    reconnect_attempt_ = 0;

    create_reconnect_timer();
    cancel_reconnect();

    QEventLoop wait_loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    auto ready_connection =
        connect(m_client, &TCIClient::ready,
                &wait_loop, &QEventLoop::quit,
                Qt::QueuedConnection);

    auto error_connection =
        connect(m_client, &TCIClient::error,
                &wait_loop, &QEventLoop::quit,
                Qt::QueuedConnection);

    auto timeout_connection =
        connect(&timeout, &QTimer::timeout,
                &wait_loop, &QEventLoop::quit);

    m_session->connectToServer(m_url);
    m_client->emitReadyIfReady();

    timeout.start(5000);
    wait_loop.exec();

    disconnect(ready_connection);
    disconnect(error_connection);
    disconnect(timeout_connection);

    if (!ready_ && m_client->isReady()) {
        ready_ = true;
    }

    if (!ready_) {
        throw error{
            tr("TCI connection failed or timed out: %1")
                .arg(m_url.toString())
        };
    }

    started_ = true;
    connection_loss_reported_ = false;

    // handle_ready() issues the one-time state queries for each session.
    // The TCI server broadcasts subsequent state changes.
    m_client->configureAudio(48000, 1, 512);
    m_client->startRxAudio();

    rx_audio_frames_ = 0;
    rx_audio_bytes_ = 0;
    rx_audio_sample_rate_ = 0;

    TRACE_CAT("TCITransceiver", "started" << state());

    return 0;
}

void TCITransceiver::do_stop()
{
    TRACE_CAT("TCITransceiver", "stopping" << state());

    stopping_ = true;
    should_reconnect_ = false;
    cancel_reconnect();

    if (m_client && m_client->isConnected()) {
        // The shared session's RX stream belongs to TCIAudioInput.
        // Stopping CAT must not interrupt another session consumer.
        m_client->setPtt(false);
    }

    started_ = false;
    ready_ = false;
}

void TCITransceiver::do_frequency(Frequency f, MODE m, bool no_ignore)
{
    Q_UNUSED(no_ignore);

    TRACE_CAT("TCITransceiver", "set frequency" << f << "mode" << m);

    if (!m_client || !m_client->isConnected())
        return;

    m_client->setFrequency(static_cast<qint64>(f));
    update_rx_frequency(f);

    if (UNK != m) {
        m_client->setMode(map_mode(m));
        update_mode(m);
    }
}

void TCITransceiver::do_tx_frequency(Frequency tx, MODE mode, bool no_ignore)
{
    Q_UNUSED(no_ignore);

    TRACE_CAT("TCITransceiver", "set TX frequency" << tx << "mode" << mode);

    if (!m_client || !m_client->isConnected())
        return;

    if (tx) {
        m_client->setTxFrequency(static_cast<qint64>(tx));

        if (UNK != mode) {
            m_client->setMode(map_mode(mode));
            update_mode(mode);
        }

        m_client->setSplit(true);

        update_other_frequency(tx);
        update_split(true);
    } else {
        m_client->setSplit(false);

        update_other_frequency(0);
        update_split(false);
    }
}

void TCITransceiver::do_mode(MODE mode)
{
    TRACE_CAT("TCITransceiver", "set mode" << mode);

    if (!m_client || !m_client->isConnected())
        return;

    m_client->setMode(map_mode(mode));
    update_mode(mode);
}

void TCITransceiver::do_ptt(bool on)
{
    TRACE_CAT("TCITransceiver", "set PTT" << on);

    if (!m_client || !m_client->isConnected())
        return;

    m_client->setPtt(on);

    // Optimistically update local state. The TCI server should also echo or
    // broadcast trx state; if transmit is denied by tci-bridge, the subsequent
    // trx:false update will correct us.
    update_PTT(on);
}

void TCITransceiver::poll()
{
    // PollingTransceiver still needs its timer for state synchronization,
    // but TCI broadcasts state changes. No periodic network queries.
}

void TCITransceiver::handle_ready()
{
    TRACE_CAT("TCITransceiver", "ready");

    cancel_reconnect();
    reconnect_attempt_ = 0;

    // Protocol READY is not equivalent to a successful TransceiverBase
    // startup. Only do_start() may mark this transceiver started.
    ready_ = true;

    m_client->queryFrequency();
    m_client->queryTxFrequency();
    m_client->queryMode();
    m_client->querySplit();
    m_client->queryPtt();
}

void TCITransceiver::handle_frequency_changed(qint64 hz)
{
    TRACE_CAT_POLL("TCITransceiver", "frequency changed" << hz);

    if (hz <= 0)
        return;

    applying_remote_state_ = true;
    update_rx_frequency(static_cast<Frequency>(hz));
    applying_remote_state_ = false;
}

void TCITransceiver::handle_tx_frequency_changed(qint64 hz)
{
    TRACE_CAT_POLL("TCITransceiver", "TX frequency changed" << hz);

    if (hz <= 0)
        return;

    applying_remote_state_ = true;
    update_other_frequency(static_cast<Frequency>(hz));
    applying_remote_state_ = false;
}

void TCITransceiver::handle_split_changed(bool enabled)
{
    TRACE_CAT_POLL("TCITransceiver", "split changed" << enabled);

    applying_remote_state_ = true;
    update_split(enabled);
    applying_remote_state_ = false;
}

void TCITransceiver::handle_mode_changed(QString mode)
{
    TRACE_CAT_POLL("TCITransceiver", "mode changed" << mode);

    const MODE mapped = map_mode(mode);

    if (UNK == mapped)
        return;

    applying_remote_state_ = true;
    update_mode(mapped);
    applying_remote_state_ = false;
}

void TCITransceiver::handle_ptt_changed(bool on)
{
    TRACE_CAT_POLL("TCITransceiver", "PTT changed" << on);

    applying_remote_state_ = true;
    update_PTT(on);
    applying_remote_state_ = false;
}

void TCITransceiver::handle_rx_audio_frame(QByteArray pcm, int sampleRate)
{
    rx_audio_frames_++;
    rx_audio_bytes_ += pcm.size();
    rx_audio_sample_rate_ = sampleRate;

    if (!(rx_audio_frames_ % 50)) {
        TRACE_CAT_POLL("TCITransceiver",
                       "RX audio frames =" << rx_audio_frames_
                                           << "bytes =" << rx_audio_bytes_
                                           << "last frame =" << pcm.size()
                                           << "sample rate =" << sampleRate);
    }

    // Phase 1: diagnostic only.
    // Next step: feed pcm into JS8Call's existing decoder/audio-input path.
}

void TCITransceiver::schedule_reconnect()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                schedule_reconnect();
            },
            Qt::QueuedConnection);
        return;
    }

    create_reconnect_timer();

    if (!reconnect_timer_)
        return;

    if (reconnect_timer_->isActive())
        return;

    if (!should_reconnect_ || stopping_)
        return;

    ++reconnect_attempt_;

    int const delay_ms = qMin(10000, 500 * reconnect_attempt_);

    TRACE_CAT("TCITransceiver",
              "scheduling reconnect in"
                  << delay_ms
                  << "ms attempt"
                  << reconnect_attempt_
                  << "timerThread" << reconnect_timer_->thread()
                  << "ownerThread" << thread()
                  << "currentThread" << QThread::currentThread());

    reconnect_timer_->start(delay_ms);
}

void TCITransceiver::create_reconnect_timer()
{
    if (reconnect_timer_)
        return;

    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                create_reconnect_timer();
            },
            Qt::QueuedConnection);
        return;
    }

    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);

    TRACE_CAT("TCITransceiver",
              "created reconnect timer"
                  << reconnect_timer_
                  << "timerThread" << reconnect_timer_->thread()
                  << "ownerThread" << thread()
                  << "currentThread" << QThread::currentThread());

    connect(reconnect_timer_, &QTimer::timeout,
        this,
        [this]() {
            if (!should_reconnect_ || stopping_)
                return;

            TRACE_CAT("TCITransceiver",
                      "attempting reconnect"
                          << reconnect_attempt_
                          << m_url
                          << "thread" << QThread::currentThread()
                          << "objectThread" << thread());

            if (m_session)
                m_session->connectToServer(m_url);
        });
}

void TCITransceiver::cancel_reconnect()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                cancel_reconnect();
            },
            Qt::QueuedConnection);
        return;
    }

    if (reconnect_timer_ && reconnect_timer_->isActive())
        reconnect_timer_->stop();
}

void TCITransceiver::report_connection_loss(QString const &reason)
{
    // Configuration owns the transceiver lifecycle. An established CAT
    // connection must fail through TransceiverBase so Configuration can
    // update the UI, close this instance, and create a new one via its
    // TCI-specific retry timer (introduced by patch 0001).
    //
    // A failed *initial* connection is handled by do_start()'s timeout.
    // Socket error and disconnected signals can both arrive for one
    // failure; only report it once.
    if (!started_ || stopping_ || connection_loss_reported_)
        return;

    connection_loss_reported_ = true;
    ready_ = false;
    started_ = false;

    const QString message =
        tr("TCI connection lost: %1 (%2)")
            .arg(m_url.toString(), reason);

    qCWarning(tcitransceiver_js8) << message;

    // Emits Transceiver::failure and shuts down the base transceiver.
    // Do not start the old reconnect timer: Configuration will create
    // a fresh instance through the normal TransceiverBase startup path.
    offline(message);
}

void TCITransceiver::handle_disconnected()
{
    report_connection_loss(tr("WebSocket disconnected"));
}

void TCITransceiver::handle_error(QString message)
{
    report_connection_loss(message);
}

Transceiver::MODE TCITransceiver::map_mode(QString const &mode) const
{
    const QString m = mode.trimmed().toUpper();

    if (m == "AM")
        return AM;

    if (m == "CW")
        return CW;

    if (m == "CWR" || m == "CW_R")
        return CW_R;

    if (m == "USB")
        return USB;

    if (m == "LSB")
        return LSB;

    if (m == "RTTY" || m == "FSK")
        return FSK;

    if (m == "RTTYR" || m == "FSK_R")
        return FSK_R;

    if (m == "DIGL" || m == "DIG_L" || m == "PKTLSB")
        return DIG_L;

    if (m == "DIGU" || m == "DIG_U" || m == "PKTUSB")
        return DIG_U;

    if (m == "FM")
        return FM;

    if (m == "DIGFM" || m == "DIG_FM" || m == "PKTFM")
        return DIG_FM;

    return UNK;
}

QString TCITransceiver::map_mode(Transceiver::MODE mode) const
{
    switch (mode) {
    case AM:
        return QStringLiteral("am");

    case CW:
        return QStringLiteral("cw");

    case CW_R:
        return QStringLiteral("cwr");

    case USB:
        return QStringLiteral("usb");

    case LSB:
        return QStringLiteral("lsb");

    case FSK:
        return QStringLiteral("rtty");

    case FSK_R:
        return QStringLiteral("rttyr");

    case DIG_L:
        return QStringLiteral("digl");

    case DIG_U:
        return QStringLiteral("digu");

    case FM:
        return QStringLiteral("fm");

    case DIG_FM:
        return QStringLiteral("digfm");

    case UNK:
    default:
        return QStringLiteral("usb");
    }
}

Q_LOGGING_CATEGORY(tcitransceiver_js8, "tcitransceiver.js8", QtWarningMsg)