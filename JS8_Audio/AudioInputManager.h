#ifndef AUDIO_INPUT_MANAGER_HPP_
#define AUDIO_INPUT_MANAGER_HPP_

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/AudioInputDevice.h"
#include "JS8_Network/TCISession.h"

#include <QObject>

class SoundInput;
class TCIAudioInput;

class AudioInputManager final : public QObject {
  Q_OBJECT

public:
  explicit AudioInputManager(QObject *parent = nullptr);
  ~AudioInputManager() override;

  Q_SLOT void start(AudioInputDevice const &device, int framesPerBuffer,
                    AudioDevice *sink,
                    AudioDevice::Channel channel = AudioDevice::Mono);

  Q_SLOT void suspend();
  Q_SLOT void resume();
  Q_SLOT void stop();

  Q_SLOT void setTciSession(TCISession *session);
  Q_SLOT void setTciRxAttenuation(qreal attenuationDb);

  Q_SIGNAL void error(QString message) const;
  Q_SIGNAL void status(QString message) const;

  Q_SIGNAL void systemAudioError(QString message) const;
  Q_SIGNAL void tciAudioError(QString message) const;

private:
  SoundInput *m_systemInput = nullptr;
  TCIAudioInput *m_tciInput = nullptr;
  AudioInputDevice::BackendType m_activeBackend =
      AudioInputDevice::BackendType::System;
};

#endif