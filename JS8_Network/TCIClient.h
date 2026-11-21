#pragma once

#include "TciProtocolHandler.h"

#include <QObject>
#include <QUrl>
#include <QWebSocket>

#include <memory>

class TCIClient final : public QObject
{
  Q_OBJECT

public:
  explicit TCIClient(QObject *parent = nullptr);

  void connectToServer(const QUrl &url);
  void disconnectFromServer();

  bool isConnected() const;
  bool isReady() const;

  void setFrequency(qint64 hz);
  void queryFrequency();

  void setTxFrequency(qint64 hz);
  void queryTxFrequency();

  void setSplit(bool enabled);
  void querySplit();

  void setMode(const QString &mode);
  void queryMode();

  void setPtt(bool enabled);
  void queryPtt();

  void configureAudio(int sampleRate = 48000,
                    int channels = 1,
                    int samplesPerFrame = 512);

  void startRxAudio();
  void stopRxAudio();

  void sendTxAudio(QByteArray const &monoInt16Pcm,
                 TciTxAudioPolicy const &policy);

  void sendTxAudioMonoInt16(QByteArray const &pcm);

  bool isConnecting() const { return connecting_; }
  QUrl connectionUrl() const { return url_; }

  Q_SLOT void emitReadyIfReady();

signals:
  void connected();
  void disconnected();
  void ready();
  void error(QString message);

  void frequencyChanged(qint64 hz);
  void txFrequencyChanged(qint64 hz);
  void splitChanged(bool enabled);
  void modeChanged(QString mode);
  void pttChanged(bool enabled);
  void txChronoRequested(TciTxAudioPolicy policy);

  void rxAudioFrame(QByteArray monoInt16Pcm, int sampleRate);

private slots:
  void onConnected();
  void onDisconnected();
  void onTextMessageReceived(const QString &message);
  void onBinaryMessageReceived(QByteArray const &message);
  void onErrorOccurred(QAbstractSocket::SocketError error);

private:
  void createSocket();
  void destroySocket();

  void sendText(QString const &message);
  bool canSend() const;
  void parseTextMessage(const QString &message);

  static QString commandName(const QString &message);
  static QStringList commandArgs(const QString &message);
  static QString semicolon(QString message);

private:
  void resetProtocolState();
  void ensureProtocolHandler();
  void selectProtocolHandler();
  void logServerInfo() const;
  void logCapabilities() const;
  void logTxPolicy();
  void logBinaryFrame(QString const &direction,
                      TCIStream::StreamFrame const &frame,
                      qsizetype messageBytes) const;

  QWebSocket *socket_ = nullptr;

  bool connected_ = false;
  bool ready_ = false;

  int audio_sample_rate_ = 48000;
  int audio_channels_ = 1;
  int audio_samples_per_frame_ = 512;

  TciServerInfo server_info_;
  TciCapabilities capabilities_;
  std::unique_ptr<TciProtocolHandler> protocol_handler_;
  TciProtocolGeneration protocol_generation_ = TciProtocolGeneration::Unknown;

  QUrl url_;
  bool connecting_ = false;

  bool rx_format_logged_ = false;
  bool chrono_format_logged_ = false;
  bool server_info_logged_ = false;
  bool policy_logged_ = false;
  bool push_policy_logged_ = false;
  bool tx_frame_logged_ = false;
  bool ptt_state_seen_ = false;
  bool ptt_state_ = false;
};
