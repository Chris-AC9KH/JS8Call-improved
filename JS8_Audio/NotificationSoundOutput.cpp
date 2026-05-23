/**
 * @file NotificationSoundOutput.cpp
 * @brief Self-contained audio output for notification sounds.
 *
 * Designed for fire-and-forget playback of buffered audio data.
 * Manages its own buffer lifecycle and releases the audio device
 * automatically when playback completes.
 */

#include "NotificationSoundOutput.h"

#include <QAudioSink>
#include <QBuffer>

NotificationSoundOutput::NotificationSoundOutput(QObject *parent)
    : QObject(parent)
{
}

NotificationSoundOutput::~NotificationSoundOutput()
{
    if (m_sink) {
        disconnect(m_sink.get(), nullptr, this, nullptr);
        m_sink->stop();
    }
}

/**
 * @brief Sets the audio device and buffer size.
 * @param device The QAudioDevice to use.
 * @param msBuffer The buffer size in milliseconds.
 */
void NotificationSoundOutput::setDevice(QAudioDevice const &device,
                                        unsigned const msBuffer)
{
    m_device = device;
    m_msBuffer = msBuffer;
}

/**
 * @brief Sets the attenuation in dB.
 * @param a The attenuation value (0 = full volume).
 */
void NotificationSoundOutput::setAttenuation(qreal const a)
{
    Q_ASSERT(0.0 <= a && a <= 999.0);
    m_volume = qPow(10.0, -a / 20.0);

    if (m_sink)
        m_sink->setVolume(m_volume);
}

/**
 * @brief Plays audio from the provided data and format.
 * @param data The raw PCM audio data.
 * @param format The QAudioFormat describing the data.
 *
 * Any currently playing notification is stopped and replaced.
 */
void NotificationSoundOutput::play(QByteArray const &data,
                                   QAudioFormat const &format)
{
#ifdef Q_OS_LINUX
    /**
     * @details
     * Linux-only debounce to mitigate issues with third-party FFmpeg-backed
     * Qt audio plugins that create invalid QAudioSink instances and never
     * close them when play() is invoked in quick succession. By rate-limiting
     * sink creation to at most once per 1000 ms, we avoid entering the buggy
     * path in those backends.
     *
     * This is intentionally scoped to Linux to avoid surprising behavior on
     * platforms where the official Qt audio backends adhere to the expected
     * threading/state contract.
     */
    if (m_sink && m_sink->state() == QAudio::ActiveState)
        return;
    
    if (m_lastPlayed.isValid() && m_lastPlayed.elapsed() < 1000)
        return;
#endif
    
    if (m_sink) {
        disconnect(m_sink.get(), nullptr, this, nullptr);
        m_sink->stop();
    }

    if (m_buffer) {
        m_buffer->close();
        m_buffer.reset();
    }

    if (m_device.isNull()) {
        Q_EMIT error(tr("No audio output device configured."));
        return;
    }

    if (!format.isValid()) {
        Q_EMIT error(tr("Requested audio format is not valid."));
        return;
    }

    if (!m_device.isFormatSupported(format)) {
        Q_EMIT error(tr("Requested audio format is not supported on device."));
        return;
    }

    if (data.isEmpty()) {
        Q_EMIT error(tr("Audio data is empty."));
        return;
    }

    m_buffer.reset(new QBuffer);
    m_buffer->setData(data);

    if (!m_buffer->open(QIODevice::ReadOnly)) {
        m_buffer.reset();
        Q_EMIT error(tr("Unable to open audio buffer."));
        return;
    }

    // Only create a new sink if we don't have one, or if the
    // format has changed since the last playback.
    if (!m_sink || m_currentFormat != format) {
        m_sink.reset(new QAudioSink(m_device, format));
        m_sink->setVolume(m_volume);
        m_currentFormat = format;

        if (m_msBuffer > 0) {
            m_sink->setBufferSize(
                m_sink->format().bytesForDuration(m_msBuffer));
        }
    }

    connect(m_sink.get(), &QAudioSink::stateChanged,
            this, &NotificationSoundOutput::handleStateChanged);

    m_sink->start(m_buffer.get());
#ifdef Q_OS_LINUX
    /**
     * @brief Mark the timestamp of the last successful play attempt.
     *
     * Used by the Linux-only debounce to avoid re-entering known-buggy paths
     * in certain FFmpeg-backed audio plugins when play() is called frequently.
     */
    m_lastPlayed.restart();
#endif

    if (m_sink->error() != QAudio::NoError) {
        Q_EMIT error(tr("Failed to start audio output."));
        m_sink->stop();
#ifdef Q_OS_LINUX
        /**
         * @brief Invalidate the debounce timer on failed start.
         *
         * If starting the sink failed, clear the last-play timestamp so that
         * a subsequent attempt isn't incorrectly throttled by the Linux-only
         * debounce logic.
         */
        m_lastPlayed.invalidate();
#endif
        if (m_buffer) {
            m_buffer->close();
            m_buffer.reset();
        }
    }
}

/**
 * @brief Stops playback and releases all resources.
 */
void NotificationSoundOutput::stop()
{
    release();
}

// Private
void NotificationSoundOutput::release()
{
    if (m_sink) {
        disconnect(m_sink.get(), nullptr, this, nullptr);
        m_sink->stop();
    }

    if (m_buffer) {
        m_buffer->close();
        m_buffer.reset();
    }
}

/**
 * @brief Handles state changes from the QAudioSink.
 * @param newState The new audio state.
 */
void NotificationSoundOutput::handleStateChanged(QAudio::State const newState)
{
    switch (newState) {
    case QAudio::ActiveState:
        Q_EMIT status(tr("Active"));
        break;

    case QAudio::SuspendedState:
        Q_EMIT status(tr("Suspended"));
        break;

    case QAudio::IdleState:
    
    case QAudio::StoppedState:
        if (m_buffer) {
            m_buffer->close();
            m_buffer.reset();
        }
        Q_EMIT status(tr("Idle"));
        break;
    }
}
