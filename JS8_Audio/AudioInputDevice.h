#ifndef AUDIO_INPUT_DEVICE_HPP_
#define AUDIO_INPUT_DEVICE_HPP_

#include <QAudioDevice>
#include <QMetaType>
#include <QUrl>

class AudioInputDevice {
public:
  enum class BackendType {
    System,
    TCI
};

  static AudioInputDevice system(QAudioDevice const &device) {
    AudioInputDevice d;
    d.m_backendType = BackendType::System;
    d.m_systemDevice = device;
    return d;
  }

  static AudioInputDevice tci(QUrl const &url) {
    AudioInputDevice d;
    d.m_backendType = BackendType::TCI;
    d.m_tciUrl = url;
    return d;
  }

  BackendType backendType() const { return m_backendType; }

  bool isSystemAudio() const {
    return BackendType::System == m_backendType;
  }

  bool isTCIAudio() const {
    return BackendType::TCI == m_backendType;
  }

  QAudioDevice const &systemDevice() const { return m_systemDevice; }
  QUrl const &tciUrl() const { return m_tciUrl; }

  QString description() const {
    if (isTCIAudio()) {
      return QStringLiteral("TCI Network Audio (%1)")
          .arg(m_tciUrl.toString());
    }

    return m_systemDevice.description();
  }

  bool isNull() const
  {
    switch (m_backendType) {
    case BackendType::System:
      return m_systemDevice.isNull();

    case BackendType::TCI:
      return !m_tciUrl.isValid() || m_tciUrl.isEmpty();

    default:
      return true;
    }
  }

  bool isValid() const
  {
    return !isNull();
  }

private:
  BackendType m_backendType = BackendType::System;
  QAudioDevice m_systemDevice;
  QUrl m_tciUrl;
};

Q_DECLARE_METATYPE(AudioInputDevice)

#endif