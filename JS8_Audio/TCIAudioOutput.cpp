#include "TCIAudioOutput.h"
#include "JS8_Network/TciTxChronoPolicy.h"

#include <QLoggingCategory>
#include <QtMath>
#include <QMetaObject>
#include <QThread>

#include "moc_TCIAudioOutput.cpp"

Q_DECLARE_LOGGING_CATEGORY(tciaudiooutput_js8)

namespace {
    void apply_gain_int16(QByteArray &pcm, qreal gain)
    {
        if (qFuzzyCompare(gain, 1.0))
            return;

        auto *samples = reinterpret_cast<qint16 *>(pcm.data());
        int const count = pcm.size() / int(sizeof(qint16));

        for (int i = 0; i < count; ++i) {
            int const scaled = qRound(double(samples[i]) * gain);
            samples[i] = static_cast<qint16>(
                qBound(-32768, scaled, 32767));
        }
    }
}

TCIAudioOutput::TCIAudioOutput(QObject *parent)
    : AudioOutputStream(parent)
{
}

TCIAudioOutput::~TCIAudioOutput()
{
    stop();
}

void TCIAudioOutput::setUrl(QUrl const &url)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, url]() {
                setUrl(url);
            },
            Qt::QueuedConnection);
        return;
    }

    if (m_url == url && m_client)
        return;

    if (m_client)
        destroy_client();

    m_url = url;

    if (m_url.isValid()) {
        create_client();

        if (m_session && m_client) {
            m_session->connectToServer(m_url);
            m_client->emitReadyIfReady();
        }
    }
}

void TCIAudioOutput::set_attenuation(qreal attenuation_db)
{
    // Some UI paths may represent TX level as "-15 dB".
    // SoundOutput conceptually wants positive attenuation: 15 dB.
    attenuation_db = qAbs(attenuation_db);

    attenuation_db = qBound<qreal>(0.0, attenuation_db, 999.0);

    m_volume = qPow(10.0, -attenuation_db / 20.0);

    qCInfo(tciaudiooutput_js8)
        << "TCI TX attenuation"
        << attenuation_db
        << "dB gain =" << m_volume;
}

void TCIAudioOutput::reset_attenuation()
{
    m_volume = 1.0;
}

void TCIAudioOutput::setSession(TCISession *session)
{
    m_session = session;
}

void TCIAudioOutput::create_client()
{
    if (m_client)
        return;

    if (!m_session) {
        Q_EMIT error(tr("TCI session is not available."));
        return;
    }

    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
                                  [this]() { create_client(); },
                                  Qt::QueuedConnection);
        return;
    }

    m_client = m_session->client();

    connect(m_client, &TCIClient::ready,
            this, &TCIAudioOutput::handle_ready,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::disconnected,
            this, &TCIAudioOutput::handle_disconnected,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::error,
            this, &TCIAudioOutput::handle_error,
            Qt::QueuedConnection);

    connect(m_client, &TCIClient::txChronoRequested,
                        this, &TCIAudioOutput::handle_tx_chrono_requested,
                        Qt::QueuedConnection);
}

void TCIAudioOutput::destroy_client()
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

    stop_pump_timer();
    stop_tx_audio_if_active();

    TCIClient *client = m_client;
    m_client = nullptr;

    disconnect(client, nullptr, this, nullptr);
}

void TCIAudioOutput::create_pump_timer()
{
    if (m_pump_timer)
        return;

    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
                                  [this]() { create_pump_timer(); },
                                  Qt::QueuedConnection);
        return;
    }

    m_pump_timer = new QTimer(this);
    m_pump_timer->setTimerType(Qt::PreciseTimer);

    connect(m_pump_timer, &QTimer::timeout,
            this, &TCIAudioOutput::pump_audio);
}

void TCIAudioOutput::stop_pump_timer()
{
    if (m_pump_timer && m_pump_timer->isActive())
        m_pump_timer->stop();
}

void TCIAudioOutput::stop_tx_audio_if_active()
{
    if (!m_tx_audio_started)
        return;

    m_tx_audio_started = false;
}

void TCIAudioOutput::reset_tx_state()
{
    m_started = false;
    m_suspended = false;
    m_tx_audio_started = false;
    m_chrono_mode = false;

    m_source.clear();
    m_buffer.clear();

    m_tx_samples_sent = 0;
    m_pump_count = 0;
    m_total_read = 0;
}

void TCIAudioOutput::restart(QIODevice *source)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, source]() {
                restart(source);
            },
            Qt::QueuedConnection);
        return;
    }

    stop();

    if (!source || !source->isOpen() || !source->isReadable()) {
        Q_EMIT error(tr("TCI TX audio source is not readable."));
        return;
    }

    m_source = source;
    m_started = true;
    m_suspended = false;
    m_stopping = false;
    m_chrono_mode = false;

    if (!m_client && m_url.isValid())
        create_client();

    if (!m_client) {
        m_started = false;
        m_source.clear();
        Q_EMIT error(tr("TCI client is not available."));
        return;
    }

    if (m_session)
        m_session->connectToServer(m_url);

    // Already connected: activate immediately.
    // Otherwise, TCIClient::ready will call handle_ready().
    if (m_client->isConnected() && m_client->isReady())
        handle_ready();
}

void TCIAudioOutput::pump_audio()
{
    if (!m_started || m_suspended || !m_client || !m_source)
        return;

    if (!m_tx_audio_started || m_chrono_mode)
        return;

    if (!m_client->isConnected() || !m_client->isReady())
        return;

    if (!m_source->isOpen() || !m_source->isReadable()) {
        stop();
        return;
    }

    constexpr qint64 sample_rate = 48000;
    constexpr int samples_per_frame = 512;
    constexpr int bytes_per_sample = int(sizeof(qint16));
    constexpr int bytes_per_frame = samples_per_frame * bytes_per_sample;

    qint64 const elapsed_ns = m_tx_clock.nsecsElapsed();

    qint64 const samples_should_have_sent =
        (elapsed_ns * sample_rate) / 1000000000LL;

    qint64 const samples_due =
        samples_should_have_sent - m_tx_samples_sent;

    if (samples_due < samples_per_frame)
        return;

    int frames_to_send = int(samples_due / samples_per_frame);

    // Avoid huge catch-up bursts if the event loop stalls.
    frames_to_send = qMin(frames_to_send, 4);

    for (int frame = 0; frame < frames_to_send; ++frame) {
        m_buffer.resize(bytes_per_frame);

        qint64 const read = m_source->read(m_buffer.data(), m_buffer.size());

        if (read <= 0) {
            if (read < 0) {
                qCWarning(tciaudiooutput_js8)
                    << "TCI TX source read failed:"
                    << m_source->errorString();
            } else {
                qCInfo(tciaudiooutput_js8)
                    << "TCI TX source returned no data; stopping stream"
                    << "samplesSent=" << m_tx_samples_sent;
            }

            stop();
            return;
        }

        if (read < m_buffer.size()) {
            std::fill(m_buffer.begin() + read, m_buffer.end(), char(0));
        }

        apply_gain_int16(m_buffer, m_volume);

        m_client->sendTxAudioMonoInt16(m_buffer);

        // IMPORTANT: this is samples, not bytes.
        m_tx_samples_sent += samples_per_frame;

        ++m_pump_count;
        m_total_read += read;

        if (!(m_pump_count % 500)) {
            qCInfo(tciaudiooutput_js8)
                << "TCI TX pump"
                << "pumps =" << m_pump_count
                << "totalRead =" << m_total_read
                << "lastRead =" << read
                << "wanted =" << m_buffer.size()
                << "sourceOpen =" << (m_source ? m_source->isOpen() : false)
                << "sourceReadable =" << (m_source ? m_source->isReadable() : false);
        }
    }
}

QByteArray TCIAudioOutput::read_mono_int16_samples(int samples)
{
    if (!m_source || !m_source->isOpen() || !m_source->isReadable())
        return {};

    int const bytesWanted = samples * int(sizeof(qint16));

    QByteArray pcm;
    pcm.resize(bytesWanted);

    qint64 const read = m_source->read(pcm.data(), pcm.size());

    if (read <= 0)
        return {};

    if (read < pcm.size())
        std::fill(pcm.begin() + read, pcm.end(), char(0));

    apply_gain_int16(pcm, m_volume);

    return pcm;
}

void TCIAudioOutput::suspend()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
                                  [this]() { suspend(); },
                                  Qt::QueuedConnection);
        return;
    }

    if (m_suspended)
        return;

    m_suspended = true;

    stop_pump_timer();

    Q_EMIT status(tr("Suspended"));
}

void TCIAudioOutput::resume()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                resume();
            },
            Qt::QueuedConnection);
        return;
    }

    if (!m_suspended)
        return;

    m_suspended = false;

    if (!m_started || !m_client || !m_source)
        return;

    if (!m_client->isConnected() || !m_client->isReady()) {
        Q_EMIT status(tr("Waiting for TCI connection"));
        return;
    }

    if (!m_tx_audio_started) {
        handle_ready();
        return;
    }

    if (!m_chrono_mode) {
        create_pump_timer();

        // Discard elapsed-time debt accumulated while suspended.
        m_tx_clock.restart();
        m_tx_samples_sent = 0;

        if (m_pump_timer && !m_pump_timer->isActive())
            m_pump_timer->start(2);
    }

    Q_EMIT status(tr("Sending"));
}

void TCIAudioOutput::reset()
{
    stop();
}

void TCIAudioOutput::stop()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                stop();
            },
            Qt::QueuedConnection);
        return;
    }

    m_stopping = true;

    stop_pump_timer();
    stop_tx_audio_if_active();

    m_source.clear();
    m_buffer.clear();

    m_started = false;
    m_suspended = false;
    m_chrono_mode = false;
    m_stopping = false;
}

void TCIAudioOutput::handle_ready()
{
    if (m_stopping || !m_started || m_suspended)
        return;

    if (!m_client || !m_client->isConnected() ||
        !m_client->isReady() || !m_source)
        return;

    if (!m_source->isOpen() || !m_source->isReadable())
        return;

    // A duplicate ready notification must not reset an active TX stream.
    if (m_tx_audio_started)
        return;

    qCInfo(tciaudiooutput_js8)
        << "TCI TX audio ready; starting TX stream";

    m_client->configureAudio(48000, 1, 512);

    m_tx_audio_started = true;
    m_error_reported = false;

    m_tx_clock.restart();
    m_tx_samples_sent = 0;
    m_pump_count = 0;
    m_total_read = 0;

    // Push mode is driven by the timer.
    // Chrono mode is driven by TX_CHRONO requests.
    if (!m_chrono_mode) {
        create_pump_timer();

        if (m_pump_timer && !m_pump_timer->isActive())
            m_pump_timer->start(2);
    }

    Q_EMIT status(tr("Sending"));
}

void TCIAudioOutput::handle_tx_chrono_requested(TciTxAudioPolicy policy)
{
    // Do not disable client pacing for stale, early, or inactive requests.
    if (!m_client || !m_source ||
        !TciTxChronoPolicy::canAccept(
            m_stopping, m_started, m_suspended, m_tx_audio_started,
            m_client->isConnected(), m_client->isReady(),
            m_source->isOpen() && m_source->isReadable()))
        return;

    const int monoSamplesNeeded =
        TciTxChronoPolicy::monoSamplesRequested(policy);
    if (monoSamplesNeeded == 0) {
        qCWarning(tciaudiooutput_js8)
            << "Ignoring invalid or unsupported TCI TX_CHRONO request:"
            << "rate=" << policy.sampleRate
            << "channels=" << policy.channels
            << "type=" << policy.sampleType
            << "sampleCount=" << policy.samplesPerFrame;
        return;
    }

    // Only a valid server request is allowed to take ownership of pacing.
    // Stop the push timer before consuming samples to avoid duplicate audio.
    if (!m_chrono_mode) {
        m_chrono_mode = true;
        stop_pump_timer();
        qCInfo(tciaudiooutput_js8) << "TCI TX switched to server pacing";
    }

    QByteArray const monoInt16 =
        read_mono_int16_samples(monoSamplesNeeded);

    if (monoInt16.isEmpty()) {
        stop();
        return;
    }

    m_client->sendTxAudio(monoInt16, policy);
}

void TCIAudioOutput::handle_disconnected()
{
    stop_pump_timer();

    reset_tx_state();

    Q_EMIT status(tr("Stopped"));
}

void TCIAudioOutput::handle_error(QString message)
{
    qCWarning(tciaudiooutput_js8)
        << "TCI audio output error:" << message;

    if (!m_stopping)
        stop();

    if (m_error_reported)
        return;

    m_error_reported = true;

    Q_EMIT error(message);
}

Q_LOGGING_CATEGORY(tciaudiooutput_js8, "tciaudiooutput.js8", QtWarningMsg)