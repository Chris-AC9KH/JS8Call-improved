/**
 * @file SoundOutput.cpp
 * @brief Checks for audio errors and emits appropriate error messages.
 * @return true if no error, false otherwise
 */

#include "SoundOutput.h"

#include <QAudioOutput>
#include <QDateTime>
#include <QLoggingCategory>
#include <QSysInfo>
#include <qmath.h>

#include "moc_SoundOutput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundout_js8)

bool SoundOutput::checkStream() const {
    bool result{false};

    Q_ASSERT_X(m_stream, "SoundOutput", "programming error");
    if (m_stream) {
        switch (m_stream->error()) {
        case QtAudio::OpenError:
            Q_EMIT error(
                tr("An error opening the audio output device has occurred."));
            break;

        case QtAudio::IOError:
            Q_EMIT error(tr(
                "An error occurred during write to the audio output device."));
            break;

        case QtAudio::FatalError:
            Q_EMIT error(tr("Non-recoverable error, audio output device not "
                            "usable at this time."));
            break;

        case QtAudio::NoError:
            default: // Includes the deprecated UnderrunError, which Qt never emits
            result = true;
            break;
        }
    }
    return result;
}

/**
 * @brief Sets the audio format based on the device and channel count.
 * @param device The QAudioDevice to use.
 * @param channels The number of audio channels (1 for mono, 2 for stereo).
 * @param msBuffered The buffer size in milliseconds.
 */
void SoundOutput::setFormat(QAudioDevice const &device, unsigned channels,
                            unsigned msBuffered) {

    Q_ASSERT(0 < channels && channels < 3);

    QAudioFormat format(device.preferredFormat());

    format.setChannelCount(channels);
    format.setSampleRate(48000);
    format.setSampleFormat(QAudioFormat::Int16);

    setDeviceFormat(device, format, msBuffered);
}

/**
 * @brief Sets the audio device and format.
 * @param device The QAudioDevice to use.
 * @param format The QAudioFormat to set.
 * @param msBuffered The buffer size in milliseconds.
 */
void SoundOutput::setDeviceFormat(QAudioDevice const &device,
                                  QAudioFormat const &format,
                                  unsigned msBuffered) {
    if (!format.isValid()) {
        Q_EMIT error(tr("Requested output audio format is not valid."));
        return;
    }
    if (!device.isFormatSupported(format)) {
        Q_EMIT error(
            tr("Requested output audio format is not supported on device."));
        return;
    }

    m_device = device;
    m_format = format;
    m_msBuffered = msBuffered;
}

/**
 * @brief Restarts audio output with the specified source.
 * @param source The QIODevice to read audio data from.
 */
void SoundOutput::restart(QIODevice *source) {
    if (!m_device.isNull()) {
        m_stream.reset(new QAudioSink(m_device, m_format));
        qCDebug(soundout_js8)
            << "SoundOutput::restart Selected audio output format:"
            << m_stream->format();
        checkStream();
        m_stream->setVolume(m_volume);
        m_error = false;

        connect(m_stream.data(), &QAudioSink::stateChanged, this,
                &SoundOutput::handleStateChanged);
    }

    if (!m_stream) {
        if (!m_error) {
            m_error = true; // only signal error once
            Q_EMIT error(tr("No audio output device configured."));
        }
        return;
    } else {
        m_error = false;
    }

    // we have to set this before every start on the stream because the
    // Windows implementation seems to forget the buffer size after a
    // stop.
    if (m_msBuffered > 0) {
        m_stream->setBufferSize(
            m_stream->format().bytesForDuration(m_msBuffered));
    }

    m_stream->start(source);
}

/**
 * @brief Suspends audio output.
 */
void SoundOutput::suspend() {
    if (m_stream && QtAudio::ActiveState == m_stream->state()) {
        m_stream->suspend();
        checkStream();
    }
}

/**
 * @brief Resumes audio output.
 */
void SoundOutput::resume() {
    if (m_stream && QtAudio::SuspendedState == m_stream->state()) {
        m_stream->resume();
        checkStream();
    }
}

/**
 * @brief Resets the audio output.
 */
void SoundOutput::reset() {
    if (m_stream) {
        m_stream->reset();
        checkStream();
    }
}

/**
 * @brief Stops audio output.
 */
void SoundOutput::stop() {
    if (m_stream) {
        m_stream->reset();
        m_stream->stop();
    }
    // m_stream.reset ();  // XXX in WSJTX, seems like a bug, will assert after
    // stop checks
}

/**
 * @brief Gets the current attenuation in decibels.
 * @return The attenuation value.
 */
qreal SoundOutput::attenuation() const {
    return -(20. * qLn(m_volume) / qLn(10.));
}

/**
 * @brief Gets the current audio format.
 * @return The QAudioFormat object.
 */
QAudioFormat SoundOutput::format() const { return m_format; }

/**
 * @brief Sets the attenuation in decibels.
 * @param a The attenuation value.
 */
void SoundOutput::setAttenuation(qreal a) {
    Q_ASSERT(0. <= a && a <= 999.);
    m_volume = qPow(10.0, -a / 20.0);
    if (m_stream) {
        m_stream->setVolume(m_volume);
    }
}

/**
 * @brief Resets the attenuation to zero.
 */
void SoundOutput::resetAttenuation() {
    m_volume = 1.;
    if (m_stream) {
        m_stream->setVolume(m_volume);
    }
}

/**
 * @brief Handles state changes of the audio output.
 * @param newState The new state of the audio output.
 */
void SoundOutput::handleStateChanged(QtAudio::State newState) const {
    switch (newState) {
    case QtAudio::IdleState:
        Q_EMIT status(tr("Idle"));
        break;

    case QtAudio::ActiveState:
        Q_EMIT status(tr("Sending"));
        break;

    case QtAudio::SuspendedState:
        Q_EMIT status(tr("Suspended"));
        break;

    case QtAudio::StoppedState:
        if (!checkStream()) {
            Q_EMIT status(tr("Error"));
        } else {
            Q_EMIT status(tr("Stopped"));
        }
        break;
    }
}

Q_LOGGING_CATEGORY(soundout_js8, "soundout.js8", QtWarningMsg)
