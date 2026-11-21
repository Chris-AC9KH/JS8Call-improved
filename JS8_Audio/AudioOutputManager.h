#ifndef AUDIO_OUTPUT_MANAGER_HPP_
#define AUDIO_OUTPUT_MANAGER_HPP_

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/AudioOutputDevice.h"
#include "JS8_Audio/AudioOutputStream.h"
#include "JS8_Network/TCISession.h"

#include <QObject>

class SoundOutput;
class TCIAudioOutput;

class AudioOutputManager final : public AudioOutputStream {
  Q_OBJECT

public:
  explicit AudioOutputManager(QObject *parent = nullptr);
  ~AudioOutputManager() override;

  Q_SLOT void setDevice(AudioOutputDevice const &device,
                        unsigned channels,
                        unsigned msBuffered = 0u);

  Q_SLOT void restart(QIODevice *source) override;
  Q_SLOT void suspend() override;
  Q_SLOT void resume() override;
  Q_SLOT void reset() override;
  Q_SLOT void stop() override;

  Q_SLOT void setTciSession(TCISession *session);

  Q_SLOT void setAttenuation(qreal attenuationDb);
  Q_SLOT void resetAttenuation();

  AudioOutputDevice::BackendType activeBackend() const {
    return m_activeBackend;
  }

private:
  AudioOutputStream *activeStream() const;

  SoundOutput *m_systemOutput = nullptr;
  TCIAudioOutput *m_tciOutput = nullptr;

  TCISession *m_tciSession = nullptr;

  AudioOutputDevice m_device;
  AudioOutputDevice::BackendType m_activeBackend =
      AudioOutputDevice::BackendType::System;

  unsigned m_channels = 1;
  unsigned m_msBuffered = 0u;
  qreal m_attenuationDb = 0.0;
};

#endif