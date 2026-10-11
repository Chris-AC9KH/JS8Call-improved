/**
 * @file SoundInput.cpp
 * @brief Implementation of SoundInput class
 */
#include "SoundInput.h"
#include "JS8_Main/DriftingDateTime.h"

#include <QAudioFormat>
#include <QLoggingCategory>
#include <QSysInfo>

#include "moc_SoundInput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundin_js8)

/**
 * @brief Checks for audio errors and emits appropriate error messages.
 *
 * @return true
 * @return false
 */
bool SoundInput::audioError() const {
    bool result(true);

    Q_ASSERT_X(m_stream, "SoundInput", "programming error");
    if (m_stream) {
        switch (m_stream->error()) {
        case QtAudio::OpenError:
            Q_EMIT error(
                tr("An error opening the audio input device has occurred."));
            break;

        case QtAudio::IOError:
            Q_EMIT error(tr(
                "An error occurred during read from the audio input device."));
            break;

        case QtAudio::FatalError:
            Q_EMIT error(tr("Non-recoverable error, audio input device not "
                            "usable at this time."));
            break;

        case QtAudio::NoError:
            default: // Includes the deprecated UnderrunError, which Qt never emits
            result = false;
            break;
        }
    }
    return result;
}

/**
 * @brief Starts audio input from the specified device.
 *
 * @param device The QAudioDevice to use for input.
 * @param framesPerBuffer The number of frames per buffer.
 * @param sink The AudioDevice sink to write audio data to.
 * @param channel The audio channel configuration (Mono or Stereo).
 */
void SoundInput::start(QAudioDevice const &device, int framesPerBuffer,
                       AudioDevice *sink, AudioDevice::Channel channel) {
    Q_ASSERT(sink);

    stop();

    m_sink = sink;

    QAudioFormat format(device.preferredFormat());
    format.setSampleFormat(QAudioFormat::Int16);
    format.setChannelCount(AudioDevice::Mono == channel ? 1 : 2);
    format.setSampleRate(48000);
    if (!format.isValid()) {
        Q_EMIT error(tr("Requested input audio format is not valid."));
        return;
    }

    if (!device.isFormatSupported(format)) {
        Q_EMIT error(
            tr("Requested input audio format is not supported on device."));
        return;
    }

    m_stream.reset(new QAudioSource{device, format});
    if (audioError()) {
        return;
    }

    connect(m_stream.data(), &QAudioSource::stateChanged, this,
            &SoundInput::handleStateChanged);

    m_stream->setBufferSize(m_stream->format().bytesForFrames(framesPerBuffer));
    if (sink->initialize(QIODevice::WriteOnly, channel)) {
        m_stream->start(sink);
        audioError();
    } else {
        Q_EMIT error(tr("Failed to initialize audio sink device"));
    }
}

/**
 * @brief Suspends audio input.
 */
void SoundInput::suspend() {
    if (m_stream) {
        m_stream->suspend();
        audioError();
    }
}

/**
 * @brief Resumes audio input.
 */
void SoundInput::resume() {
    if (m_sink) {
        m_sink->reset();
    }

    if (m_stream) {
        m_stream->resume();
        audioError();
    }
}

/**
 * @brief Handles state changes of the audio input.
 * @param newState The new state of the audio input.
 */
void SoundInput::handleStateChanged(QtAudio::State newState) const {
    switch (newState) {
    case QtAudio::IdleState:
        Q_EMIT status(tr("Idle"));
        break;

    case QtAudio::ActiveState:
        Q_EMIT status(tr("Receiving"));
        break;

    case QtAudio::SuspendedState:
        Q_EMIT status(tr("Suspended"));
        break;

    case QtAudio::StoppedState:
        if (audioError()) {
            Q_EMIT status(tr("Error"));
        } else {
            Q_EMIT status(tr("Stopped"));
        }
        break;
    }
}

/**
 * @brief Stops audio input.
 */
void SoundInput::stop() {
    if (m_stream) {
        m_stream->stop();
    }
    m_stream.reset();

    if (m_sink) {
        m_sink->close();
    }
}

/**
 * @brief Destructs the SoundInput object.
 */
SoundInput::~SoundInput() { stop(); }

Q_LOGGING_CATEGORY(soundin_js8, "soundin.js8", QtWarningMsg)
