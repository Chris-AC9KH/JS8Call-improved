#ifndef TCI_STREAM_FRAME_HPP_
#define TCI_STREAM_FRAME_HPP_

#include <QByteArray>
#include <QtGlobal>

namespace TCIStream
{
enum StreamType : quint32
{
  IQ_STREAM = 0,
  RX_AUDIO_STREAM = 1,
  TX_AUDIO_STREAM = 2,
  TX_CHRONO = 3,
  LINEOUT_STREAM = 4
};

enum SampleType : quint32
{
  INT16 = 0,
  INT24 = 1,
  INT32 = 2,
  FLOAT32 = 3
};

struct StreamFrame
{
  bool valid = false;

  quint32 receiver = 0;
  quint32 sampleRate = 0;
  quint32 sampleType = FLOAT32;
  quint32 codec = 0;
  quint32 crc = 0;
  quint32 sampleCount = 0;
  quint32 streamType = RX_AUDIO_STREAM;
  quint32 channels = 1;

  QByteArray payload;
};

StreamFrame parseFrame(QByteArray const &frame);

QByteArray makeTxAudioFrame(QByteArray const &pcm,
                            quint32 receiver = 0,
                            quint32 sampleRate = 48000,
                            quint32 sampleType = FLOAT32,
                            quint32 channels = 1);

QByteArray makeTxAudioFrameWithSampleCount(QByteArray const &payload,
                                           quint32 sampleCount,
                                           quint32 receiver = 0,
                                           quint32 sampleRate = 48000,
                                           quint32 sampleType = FLOAT32,
                                           quint32 channels = 1);
}

#endif