#ifndef TCI_TRANSCEIVER_HPP_
#define TCI_TRANSCEIVER_HPP_

#include "JS8_Network/TCISession.h"

#include "PollingTransceiver.h"
#include "TransceiverFactory.h"

#include <QScopedPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

class TCIClient;
class TCISession;


class TCITransceiver final : public PollingTransceiver {
  Q_OBJECT

public:
  static QString const transceiver_name_;

  // Reuse ParameterPack for now so the factory/settings path can be wired
  // similarly to network-style CAT backends.
  explicit TCITransceiver(TransceiverFactory::ParameterPack const &,
                          TCISession *session,
                          QObject *parent = nullptr);

private:
  int do_start() override;
  void do_stop() override;

  void do_frequency(Frequency, MODE, bool no_ignore) override;
  void do_tx_frequency(Frequency, MODE, bool no_ignore) override;
  void do_mode(MODE) override;
  void do_ptt(bool) override;

  void poll() override;

  QUrl make_url(TransceiverFactory::ParameterPack const &) const;

  Transceiver::MODE map_mode(QString const &) const;
  QString map_mode(Transceiver::MODE) const;

  void handle_ready();
  void handle_frequency_changed(qint64);
  void handle_tx_frequency_changed(qint64);
  void handle_split_changed(bool);
  void handle_mode_changed(QString);
  void handle_ptt_changed(bool);
  void handle_rx_audio_frame(QByteArray, int sampleRate);
  void handle_disconnected();
  void handle_error(QString);

private:
  void report_connection_loss(QString const &reason);
  void create_reconnect_timer();
  void schedule_reconnect();
  void cancel_reconnect();

  TCISession *m_session = nullptr;
  TCIClient *m_client = nullptr;
  QUrl m_url;

  bool started_ = false;
  bool ready_ = false;
  bool connection_loss_reported_ = false;
  bool applying_remote_state_ = false;

  qint64 rx_audio_frames_ = 0;
  qint64 rx_audio_bytes_ = 0;
  int rx_audio_sample_rate_ = 0;

  QTimer *reconnect_timer_ = nullptr;
  bool should_reconnect_ = false;
  bool stopping_ = false;
  int reconnect_attempt_ = 0;
};

#endif