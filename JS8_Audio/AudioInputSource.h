#ifndef AUDIO_INPUT_SOURCE_HPP_
#define AUDIO_INPUT_SOURCE_HPP_

#include <QObject>

#include "JS8_Audio/AudioDevice.h"

class AudioInputSource : public QObject {
  Q_OBJECT

public:
  explicit AudioInputSource(QObject *parent = nullptr)
      : QObject(parent) {}

  ~AudioInputSource() override = default;

public Q_SLOTS:
  virtual void start(unsigned framesBuffered,
                     AudioDevice *sink,
                     AudioDevice::Channel channel) = 0;

  virtual void stop() = 0;

  Q_SIGNALS:
    void error(QString message);
};

#endif