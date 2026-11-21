#include "TCIAudioInput.h"

#include <QLoggingCategory>
#include <QThread>
#include <QMetaObject>
#include <QtMath>

#include "moc_TCIAudioInput.cpp"

Q_DECLARE_LOGGING_CATEGORY(tciaudioinput_js8)

TCIAudioInput::TCIAudioInput(QObject *parent)
    : QObject(parent)
{
    // Child of this object so it moves with the audio input to m_audioThread.
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(2000);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &TCIAudioInput::retry_connection);
}

TCIAudioInput::~TCIAudioInput()
{
    stop();
}

void TCIAudioInput::start(QUrl const &url, int framesPerBuffer,
                          AudioDevice *sink,
                          AudioDevice::Channel channel)
{
    Q_UNUSED(channel);

    stop();

    if (!sink) {
        Q_EMIT error(tr("No audio sink supplied for TCI input."));
        return;
    }

    m_url = url;
    m_sink = sink;
    m_framesPerBuffer = framesPerBuffer;
    m_started = false;
    m_suspended = false;
    m_stopping = false;


    if (!m_sink) {
        Q_EMIT error(tr("No audio sink supplied for TCI input."));
        return;
    }

    // TCI bridge RX audio is 48 kHz mono signed int16.
    // Force Mono here. The configured left/right/both channel selector applies
    // to physical stereo devices, not the network mono stream.
    if (!m_sink->initialize(QIODevice::WriteOnly, AudioDevice::Mono)) {
        Q_EMIT error(tr("Failed to initialize TCI audio sink device."));
        m_sink.clear();
        return;
    }

    Q_EMIT status(tr("Connecting"));

    qCInfo(tciaudioinput_js8) << "Connecting TCI audio input:" << url;

    create_client();

    m_session->connectToServer(m_url);
    m_client->emitReadyIfReady();
    m_reconnectTimer->start();
}

void TCIAudioInput::retry_connection()
{
    if (m_stopping || !m_sink || !m_session || !m_url.isValid()) {
        m_reconnectTimer->stop();
        return;
    }
    if (m_client && m_client->isConnected() && m_client->isReady()) {
        m_reconnectTimer->stop();
        if (!m_started && !m_suspended)
            handle_ready();
        return;
    }
    m_session->connectToServer(m_url);
}

void TCIAudioInput::handle_ready()
{
    if (m_stopping || m_suspended || m_started)
        return;

    qWarning() << "TCI RX READY"
           << "alreadyStarted=" << m_started
           << "sinkValid=" << bool(m_sink);

    // An unexpected duplicate READY should not clear live RX samples.
    // Instrument only: leave original behavior unchanged for diagnosis.
    if (!m_sink || !m_client ||
        !m_client->isConnected() || !m_client->isReady())
        return;

    m_reconnectTimer->stop();
    m_sink->reset();

    if (m_suspended) {
        m_started = false;
        Q_EMIT status(tr("Suspended"));
        return;
    }

    qCInfo(tciaudioinput_js8)
        << "TCI audio input ready; starting RX stream";

    m_client->configureAudio(48000, 1, 512);
    m_client->startRxAudio();

    m_started = true;

    Q_EMIT status(tr("Receiving"));
}

/*
void TCIAudioInput::handle_rx_audio_frame(QByteArray pcm, int sampleRate)
{
    if (!m_started || m_suspended || !m_sink)
        return;

    if (sampleRate != 48000) {
        Q_EMIT error(tr("Unsupported TCI audio sample rate: %1 Hz.")
                         .arg(sampleRate));
        return;
    }

    if (pcm.isEmpty())
        return;

    qint64 const written = m_sink->write(pcm.constData(), pcm.size());

    if (written != pcm.size()) {
        qCWarning(tciaudioinput_js8)
            << "Short write to TCI audio sink:"
            << written
            << "of"
            << pcm.size();
    }
}
*/
void TCIAudioInput::handle_rx_audio_frame(QByteArray pcm, int sampleRate)
{
    if (!m_started || m_suspended || !m_sink)
        return;

    if (sampleRate != 48000) {
        Q_EMIT error(tr("Unsupported TCI audio sample rate: %1 Hz.")
                         .arg(sampleRate));
        return;
    }

    if (pcm.isEmpty())
        return;

    // TCI-only attenuation. Scale decoded mono int16 PCM before passing it
    // to the detector; the system audio backend is left untouched.
    if (m_rxGain < 1.0) {
        auto *samples = reinterpret_cast<qint16 *>(pcm.data());
        const qsizetype count = pcm.size() / qsizetype(sizeof(qint16));
        for (qsizetype i = 0; i < count; ++i)
            samples[i] = static_cast<qint16>(qRound(samples[i] * m_rxGain));
    }

    qint64 const written = m_sink->write(pcm.constData(), pcm.size());

    ++m_rx_frames;
    m_rx_bytes += pcm.size();
    if (written != pcm.size())
        ++m_rx_short_writes;
    if (!m_rx_statistics_clock.isValid())
        m_rx_statistics_clock.start();
    if (m_rx_statistics_clock.elapsed() >= 20000) {
        qWarning() << "TCI RX SINK 20s"
                   << "frames=" << m_rx_frames
                   << "bytes=" << m_rx_bytes
                   << "shortWrites=" << m_rx_short_writes
                   << "elapsedMs=" << m_rx_statistics_clock.elapsed();
        m_rx_statistics_clock.restart();
        m_rx_frames = 0;
        m_rx_bytes = 0;
        m_rx_short_writes = 0;
    }

    if (written != pcm.size()) {
        qCWarning(tciaudioinput_js8)
            << "Short write to TCI audio sink:"
            << written
            << "of"
            << pcm.size();
    }
}

void TCIAudioInput::setSession(TCISession *session)
{
    m_session = session;
}

void TCIAudioInput::setAttenuation(qreal attenuationDb)
{
    attenuationDb = qBound<qreal>(0.0, attenuationDb, 45.0);
    m_rxGain = qPow(10.0, -attenuationDb / 20.0);
}

void TCIAudioInput::suspend()
{
    m_suspended = true;

    /*
     * TODO remove after verifying chrono TX
    if (m_client && m_client->isConnected())
        m_client->stopRxAudio();
    */

    Q_EMIT status(tr("Suspended"));
}

void TCIAudioInput::resume()
{
    if (m_sink)
        m_sink->reset();

    m_suspended = false;

    if (m_client && m_client->isConnected() && m_client->isReady()) {
        m_client->startRxAudio();
        m_started = true;
        Q_EMIT status(tr("Receiving"));
        return;
    }

    m_started = false;

    if (!m_stopping) {
        m_reconnectTimer->start();
        Q_EMIT status(tr("Waiting for TCI connection"));

        if (m_session && m_url.isValid())
            m_session->connectToServer(m_url);

        return;
    }

    Q_EMIT status(tr("Stopped"));
}

void TCIAudioInput::stop()
{
    m_stopping = true;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    m_started = false;
    m_suspended = false;

    if (m_client && m_client->isConnected())
        m_client->stopRxAudio();

    destroy_client();

    if (m_sink) {
        m_sink->close();
        m_sink.clear();
    }

    Q_EMIT status(tr("Stopped"));
}


void TCIAudioInput::handle_disconnected()
{
    qCInfo(tciaudioinput_js8)
        << "TCI audio input disconnected"
        << "stopping =" << m_stopping;

    m_started = false;
    // Preserve m_suspended across a connection loss. RX must not restart
    // during TX until the audio manager explicitly calls resume().

    if (m_stopping) {
        Q_EMIT status(tr("Stopped"));
        return;
    }

    m_reconnectTimer->start();
    Q_EMIT status(tr("Disconnected"));
}

void TCIAudioInput::handle_error(QString message)
{
    qCWarning(tciaudioinput_js8)
        << "TCI audio input error:" << message;

    m_started = false;

    if (!m_stopping && m_reconnectTimer)
        m_reconnectTimer->start();

    if (m_stopping) {
        Q_EMIT status(tr("Error"));
        return;
    }
}

void TCIAudioInput::create_client()
{
    if (m_client)
        return;

    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                create_client();
            },
            Qt::QueuedConnection);
        return;
    }

    if (!m_session) {
        Q_EMIT error(tr("TCI session is not available."));
        return;
    }

    m_client = m_session->client();

    qCInfo(tciaudioinput_js8)
        << "Created TCI audio client"
        << "client =" << m_client
        << "clientThread =" << m_client->thread()
        << "ownerThread =" << thread()
        << "currentThread =" << QThread::currentThread();


    connect(m_client, &TCIClient::ready,
            this,
            &TCIAudioInput::handle_ready,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::disconnected,
            this,
            &TCIAudioInput::handle_disconnected,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::rxAudioFrame,
            this,
            &TCIAudioInput::handle_rx_audio_frame,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::error,
            this,
            &TCIAudioInput::handle_error,
            Qt::QueuedConnection);
}


void TCIAudioInput::destroy_client()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                destroy_client();
            },
            Qt::QueuedConnection);
        return;
    }

    if (!m_client)
        return;

    TCIClient *client = m_client;
    m_client = nullptr;

    disconnect(client, nullptr, this, nullptr);

    client->stopRxAudio();
}

Q_LOGGING_CATEGORY(tciaudioinput_js8, "tciaudioinput.js8", QtWarningMsg)