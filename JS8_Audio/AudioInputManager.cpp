#include "AudioInputManager.h"

#include "JS8_Audio/SoundInput.h"
#include "JS8_Audio/TCIAudioInput.h"

#include <QDebug>
#include <QThread>

AudioInputManager::AudioInputManager(QObject *parent)
    : QObject(parent),
      m_systemInput(new SoundInput(this)),
      m_tciInput(new TCIAudioInput(this))
{
    connect(m_systemInput, &SoundInput::error,
        this, &AudioInputManager::error);

    connect(m_systemInput, &SoundInput::error,
            this, &AudioInputManager::systemAudioError);

    connect(m_systemInput, &SoundInput::status,
            this, &AudioInputManager::status);

    connect(m_tciInput, &TCIAudioInput::error,
            this, &AudioInputManager::error);

    connect(m_tciInput, &TCIAudioInput::error,
            this, &AudioInputManager::tciAudioError);

    connect(m_tciInput, &TCIAudioInput::status,
            this, &AudioInputManager::status);
}

AudioInputManager::~AudioInputManager()
{
    stop();
}

void AudioInputManager::start(AudioInputDevice const &device,
                              int framesPerBuffer,
                              AudioDevice *sink,
                              AudioDevice::Channel channel)
{
    stop();

    m_activeBackend = device.backendType();

    switch (device.backendType()) {
    case AudioInputDevice::BackendType::System:
        m_systemInput->start(device.systemDevice(),
                             framesPerBuffer,
                             sink,
                             channel);
        break;

    case AudioInputDevice::BackendType::TCI:
        m_tciInput->start(device.tciUrl(),
                          framesPerBuffer,
                          sink,
                          AudioDevice::Mono);
        break;
    }
}

void AudioInputManager::suspend()
{
    switch (m_activeBackend) {
    case AudioInputDevice::BackendType::System:
        m_systemInput->suspend();
        break;

    case AudioInputDevice::BackendType::TCI:
        m_tciInput->suspend();
        break;
    }
}

void AudioInputManager::resume()
{
    switch (m_activeBackend) {
    case AudioInputDevice::BackendType::System:
        m_systemInput->resume();
        break;

    case AudioInputDevice::BackendType::TCI:
        m_tciInput->resume();
        break;
    }
}

void AudioInputManager::stop()
{
    if (m_systemInput)
        m_systemInput->stop();

    if (m_tciInput)
        m_tciInput->stop();
}

void AudioInputManager::setTciSession(TCISession *session)
{
    if (m_tciInput)
        m_tciInput->setSession(session);
}

void AudioInputManager::setTciRxAttenuation(qreal attenuationDb)
{
    if (m_tciInput)
        m_tciInput->setAttenuation(attenuationDb);
}
