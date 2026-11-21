#include "AudioOutputManager.h"

#include "JS8_Audio/SoundOutput.h"
#include "JS8_Audio/TCIAudioOutput.h"

#include <QtMath>

AudioOutputManager::AudioOutputManager(QObject *parent)
    : AudioOutputStream(parent),
      m_systemOutput(new SoundOutput(this)),
      m_tciOutput(new TCIAudioOutput(this))
{
    connect(m_systemOutput, &SoundOutput::error,
            this, &AudioOutputManager::error);

    connect(m_systemOutput, &SoundOutput::status,
            this, &AudioOutputManager::status);

    connect(m_tciOutput, &TCIAudioOutput::error,
            this, &AudioOutputManager::error);

    connect(m_tciOutput, &TCIAudioOutput::status,
            this, &AudioOutputManager::status);
}

AudioOutputManager::~AudioOutputManager()
{
    stop();
}

void AudioOutputManager::setDevice(AudioOutputDevice const &device,
                                   unsigned channels,
                                   unsigned msBuffered)
{
    stop();

    m_device = device;
    m_activeBackend = device.backendType();
    m_channels = channels;
    m_msBuffered = msBuffered;

    switch (m_activeBackend) {
    case AudioOutputDevice::BackendType::System:
        m_systemOutput->setFormat(device.systemDevice(),
                                  m_channels,
                                  m_msBuffered);
        m_systemOutput->setAttenuation(m_attenuationDb);
        break;

    case AudioOutputDevice::BackendType::TCI:
        if (!m_tciOutput)
            m_tciOutput = new TCIAudioOutput(this);

        m_tciOutput->setSession(m_tciSession);
        m_tciOutput->setUrl(device.tciUrl());
        m_tciOutput->set_attenuation(m_attenuationDb);
        break;
    }
}

void AudioOutputManager::setTciSession(TCISession *session)
{
    m_tciSession = session;

    if (m_tciOutput)
        m_tciOutput->setSession(session);
}

AudioOutputStream *AudioOutputManager::activeStream() const
{
    switch (m_activeBackend) {
    case AudioOutputDevice::BackendType::System:
        return m_systemOutput;

    case AudioOutputDevice::BackendType::TCI:
        return m_tciOutput;
    }

    return nullptr;
}

void AudioOutputManager::restart(QIODevice *source)
{
    if (AudioOutputStream *stream = activeStream())
        stream->restart(source);
}

void AudioOutputManager::suspend()
{
    if (AudioOutputStream *stream = activeStream())
        stream->suspend();
}

void AudioOutputManager::resume()
{
    if (AudioOutputStream *stream = activeStream())
        stream->resume();
}


void AudioOutputManager::reset()
{
    /*
     * We could reset only the active stream, but let's reset both to
     * make quick-abort cleanup deterministic even if the active backend
     * changed during configuration churn.

    if (AudioOutputStream *stream = activeStream())
        stream->reset();
    */

    if (m_systemOutput)
        m_systemOutput->reset();

    if (m_tciOutput)
        m_tciOutput->reset();
}

void AudioOutputManager::stop()
{
    if (m_systemOutput)
        m_systemOutput->stop();

    if (m_tciOutput)
        m_tciOutput->stop();
}

void AudioOutputManager::setAttenuation(qreal attenuationDb)
{
    // The existing UI path conceptually expresses this as attenuation.
    // Be tolerant of alternate paths that might pass a negative dB value.
    m_attenuationDb = qAbs(attenuationDb);

    if (m_systemOutput)
        m_systemOutput->setAttenuation(m_attenuationDb);

    if (m_tciOutput)
        m_tciOutput->set_attenuation(m_attenuationDb);
}

void AudioOutputManager::resetAttenuation()
{
    m_attenuationDb = 0.0;

    if (m_systemOutput)
        m_systemOutput->resetAttenuation();

    if (m_tciOutput)
        m_tciOutput->reset_attenuation();
}