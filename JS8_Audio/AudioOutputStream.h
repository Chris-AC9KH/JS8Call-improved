#ifndef AUDIO_OUTPUT_STREAM_HPP_
#define AUDIO_OUTPUT_STREAM_HPP_

#include <QObject>
#include <QIODevice>
#include <QString>

class AudioOutputStream : public QObject {
  Q_OBJECT

public:
  explicit AudioOutputStream(QObject *parent = nullptr)
      : QObject(parent) {}

  ~AudioOutputStream() override = default;

public Q_SLOTS:
  virtual void restart(QIODevice *source) = 0;
  virtual void suspend() = 0;
  virtual void resume() = 0;
  virtual void reset() = 0;
  virtual void stop() = 0;

  Q_SIGNALS:
    void error(QString message) const;
  void status(QString message) const;
};

#endif