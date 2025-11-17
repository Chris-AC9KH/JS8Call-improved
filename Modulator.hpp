#ifndef MODULATOR_HPP__
#define MODULATOR_HPP__

#include <QAudio>
#include <QPointer>
#include "AudioDevice.hpp"

class SoundOutput;

/**
 * Audio device that generates PCM audio frames that encode a message.
 *
 * Output can be muted while underway, preserving waveform timing when
 * transmission is resumed.
 *
 * This is intended to run in a thread different from the GUI thread.
 * It is **not** generally thread-safe, see remarks below.
*/
class Modulator final
  : public AudioDevice
{
  Q_OBJECT;

public:

  enum class State
  {
    Synchronizing,
    Active,
    Idle
  };

  // Constructor

  explicit Modulator(QObject * parent = nullptr)
  : AudioDevice {parent}
  {}

  // Inline accessors

  /**
   * Whether the device is idle.
   *
   * This method is thread-safe, i.e., can be called from a different thread.
   */
  bool isIdle() const { return m_state.load() == State::Idle; }

  // Manipulators

  void close() override;

  /**
   * Sets the audio frequency.
   *
   * This is **not** by itself thread-safe, but ok if fed
   * via the Qt signalling mechanism.
   */
  Q_SLOT void setAudioFrequency(double const audioFrequency)
  {
    m_audioFrequency = audioFrequency;
  }

  // Slots

  Q_SLOT void start(double        audioFrequency,
                    int           submode,
                    double        tx_delay,
                    SoundOutput * stream,
                    Channel       channel);
  Q_SLOT void stop(bool quick = false);
  Q_SLOT void tune(bool state = true);

protected:

  // QIODevice protocol

  qint64 readData (char       *, qint64) override;
  qint64 writeData(char const *, qint64) override
  {
    return -1;			// we don't consume data
  }

  // In current Qt versions, bytesAvailable() must return a size
  // that exceeds some threshold in order for the AudioSink to go
  // into Active state and start pulling data. This behavior began
  // on Windows with the 6.4 release, on Mac with 6.8, and on Linux
  // with 6.9.
  //
  // See: https://bugreports.qt.io/browse/QTBUG-108672

  qint64 bytesAvailable() const override
  {
    return 8000;
  }

private:

  // Data members

  QPointer<SoundOutput> m_stream;
  std::atomic<State>    m_state      = State::Idle;
  bool                  m_quickClose = false;
  bool                  m_tuning     = false;
  double                m_audioFrequency;
  double                m_audioFrequency0;
  double                m_toneSpacing;
  double                m_phi;
  double                m_dphi;
  double                m_amp;
  double                m_nsps;
  qint64                m_silentFrames;
  unsigned              m_ic;
  unsigned              m_isym0;
};

#endif
