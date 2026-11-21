#ifndef TCI_AUDIO_OUTPUT_HPP_
#define TCI_AUDIO_OUTPUT_HPP_

#include "BWFFile.h"
#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/AudioOutputStream.h"
#include "JS8_Network/TCIClient.h"
#include "JS8_Network/TCISession.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class TCIAudioOutput final : public AudioOutputStream {
  Q_OBJECT

public:
  explicit TCIAudioOutput(QObject *parent = nullptr);
  ~TCIAudioOutput() override;

public Q_SLOTS:
  void restart(QIODevice *source) override;
  void suspend() override;
  void resume() override;
  void reset() override;
  void stop() override;
  void set_attenuation(qreal attenuation_db);
  void reset_attenuation();
  void handle_tx_chrono_requested(TciTxAudioPolicy policy);
  void setSession(TCISession *session);
  void setUrl(QUrl const &url);


private Q_SLOTS:
  void handle_ready();
  void handle_disconnected();
  void handle_error(QString message);
  void pump_audio();

private:
  void create_client();
  void destroy_client();
  void create_pump_timer();

  void stop_pump_timer();
  QByteArray read_mono_int16_samples(int samples);
  void stop_tx_audio_if_active();
  void reset_tx_state();

  TCISession *m_session = nullptr;
  TCIClient *m_client = nullptr;

  QTimer *m_pump_timer = nullptr;

  QPointer<QIODevice> m_source;
  QUrl m_url;

  QByteArray m_buffer;

  qreal m_volume = 0.177827941; // -15 dB

  bool m_started = false;
  bool m_suspended = false;
  bool m_stopping = false;
  bool m_tx_audio_started = false;
  bool m_chrono_mode = false;
  bool m_error_reported = false;

  QElapsedTimer m_tx_clock;
  qint64 m_tx_samples_sent = 0;
  qint64 m_pump_count = 0;
  qint64 m_total_read = 0;
};

#endif