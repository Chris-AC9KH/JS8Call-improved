/**
 * @file Modulator.h
 * @brief Audio modulator device that generates PCM frames for transmission.
 */

#ifndef MODULATOR_HPP__
#define MODULATOR_HPP__

#include "JS8_Audio/AudioDevice.h"

#include <QAudio>
#include <QPointer>

class AudioOutputStream;

/**
 * @class Modulator
 * @brief Audio device which synthesizes JS8 waveform samples for TX.
 *
 * The `Modulator` implements a `QIODevice` audio source that produces
 * PCM samples encoding an outgoing message. Output may be muted while
 * keeping timing intact to allow resuming without disrupting the
 * transmission schedule. The device is intended to run in a worker
 * thread and is not generally safe to call from arbitrary threads.
 */
class Modulator final : public AudioDevice {
    Q_OBJECT;

  public:
    enum class State { Synchronizing, Active, Idle };

    /**
     * @brief Construct a modulator instance.
     * @param parent Optional parent QObject.
     */
    explicit Modulator(QObject *parent = nullptr) : AudioDevice{parent} {}

    /**
     * @brief Whether the device is currently idle.
     * @return true when idle.
     *
     * This accessor is thread-safe and can be queried from other threads.
     */
    bool isIdle() const { return m_state.load() == State::Idle; }

    // Manipulators

    /**
     * @brief Close the device and stop generating samples.
     */
    void close() override;

    /**
     * @brief Set the base audio frequency used for modulation.
     * @param audioFrequency Frequency in Hz.
     *
     * Not thread-safe by itself; use Qt signals to invoke from other threads.
     */
    Q_SLOT void setAudioFrequency(double const audioFrequency) {
        m_audioFrequency = audioFrequency;
    }

    // Slots

  /**
     * @brief Start transmission with the given parameters.
     */
    Q_SLOT void start(double audioFrequency, int submode, double tx_delay,
                AudioOutputStream *stream, Channel channel);

    /**
     * @brief Stop transmission.
     * @param quick If true, perform a quick stop.
     */
    Q_SLOT void stop(bool quick = false);

    /**
     * @brief Enter or leave tuning state.
     */
    Q_SLOT void tune(bool state = true);

  protected:
    // QIODevice protocol

    qint64 readData(char *, qint64) override;
    qint64 writeData(char const *, qint64) override { return -1; }

    /**
     * @brief Bytes available hint for QAudioSink; ensures Active state.
     *
     * Qt audio backends require that a source report a minimum number of
     * bytes available in order to transition the sink into the Active
     * state and begin pulling data. Different Qt releases/platforms
     * changed this behavior; to remain compatible we provide a
     * conservative bytes-available hint here.
     *
     * @note Observed version/platform behavior:
     * - Windows: behavior change observed starting in Qt 6.4
     * - macOS: behavior change observed starting in Qt 6.8
     * - Linux: behavior change observed starting in Qt 6.9
     *
     * @see https://bugreports.qt.io/browse/QTBUG-108672
     */
    qint64 bytesAvailable() const override { return 8000; }

  private:
    // Data members

    QPointer<AudioOutputStream> m_stream;
    std::atomic<State> m_state = State::Idle;
    bool m_quickClose = false;
    bool m_tuning = false;
    double m_audioFrequency;
    double m_audioFrequency0;
    double m_toneSpacing;
    double m_phi;
    double m_dphi;
    double m_amp;
    double m_nsps;
    qint64 m_silentFrames;
    unsigned m_ic;
    unsigned m_isym0;
};

#endif
