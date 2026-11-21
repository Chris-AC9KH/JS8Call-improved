#ifndef TCI_AUDIO_INPUT_HPP_
#define TCI_AUDIO_INPUT_HPP_

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Network/TCIClient.h"
#include "JS8_Network/TCISession.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class TCIAudioInput final : public QObject {
  Q_OBJECT

public:
  explicit TCIAudioInput(QObject *parent = nullptr);
  ~TCIAudioInput() override;

  Q_SLOT void start(QUrl const &url, int framesPerBuffer,
                    AudioDevice *sink,
                    AudioDevice::Channel channel = AudioDevice::Mono);

  Q_SLOT void setSession(TCISession *session);
  Q_SLOT void setAttenuation(qreal attenuationDb);
  Q_SLOT void suspend();
  Q_SLOT void resume();
  Q_SLOT void stop();

  Q_SIGNAL void error(QString message) const;
  Q_SIGNAL void status(QString message) const;

private Q_SLOTS:
  void handle_ready();
  void handle_disconnected();
  void handle_rx_audio_frame(QByteArray pcm, int sampleRate);
  void handle_error(QString message);

private:
  void create_client();
  void destroy_client();
  void retry_connection();

  TCISession *m_session = nullptr;
  TCIClient *m_client = nullptr;

  QPointer<AudioDevice> m_sink;
  qreal m_rxGain = 1.0;
  QUrl m_url;
  QTimer *m_reconnectTimer = nullptr;
  int m_framesPerBuffer = 0;
  QElapsedTimer m_rx_statistics_clock;
  qint64 m_rx_frames = 0;
  qint64 m_rx_bytes = 0;
  qint64 m_rx_short_writes = 0;
  bool m_started = false;
  bool m_suspended = false;
  bool m_stopping = false;
};

#endif