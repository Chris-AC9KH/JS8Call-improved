/**
 * @file SoundOutput.h
 * @brief Send audio data to a configured soundcard/output device.
 */

#ifndef SOUNDOUT_H__
#define SOUNDOUT_H__

#include "JS8_Audio/AudioOutputStream.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QObject>
#include <QString>

/**
 * @class SoundOutput
 * @brief Manages audio output format, buffering and the underlying sink.
 *
 * Responsible for configuring a `QAudioSink`, managing buffering latency,
 * and applying attenuation for playback. Typical usage is to call
 * `setFormat()` or `setDeviceFormat()` and then `restart()` with a
 * prepared `QIODevice` for streaming audio.
 */
class SoundOutput : public AudioOutputStream {
    Q_OBJECT;

  public:
    /**
     * @brief Default-construct a SoundOutput helper.
     */
    explicit SoundOutput(QObject *parent = nullptr)
    : AudioOutputStream(parent) {}

    /**
     * @brief Current attenuation (volume multiplier).
     * @return Attenuation factor where 1.0 is unity gain.
     */
    qreal attenuation() const;

    /**
     * @brief Active audio format used by the output.
     * @return The `QAudioFormat` currently configured for playback.
     */
    QAudioFormat format() const;

  public Q_SLOTS:
    /**
     * @brief Configure format based on device and channel count.
     * @param device Output device to use.
     * @param channels Number of channels (1 or 2).
     * @param msBuffered Milliseconds of buffering to set up (0 = default).
     */
    void setFormat(QAudioDevice const &device, unsigned channels,
                   unsigned msBuffered = 0u);

    /**
     * @brief Set device and explicit format to use for playback.
     * @param device Output device to use.
     * @param format Exact audio format to use for the sink.
     * @param msBuffered Milliseconds of buffering to set up (0 = default).
     */
    void setDeviceFormat(QAudioDevice const &device, QAudioFormat const &format,
                         unsigned msBuffered = 0u);

    /**
     * @brief Restart output streaming from the given `QIODevice` source.
     * @param source The device providing audio samples for playback.
     */
    void restart(QIODevice *) override;

    /**
     * @brief Temporarily suspend output streaming.
     */
    void suspend() override;

    /**
     * @brief Resume output after a suspend.
     */
    void resume() override;

    /**
     * @brief Reset internal state and stop any active stream.
     */
    void reset() override;

    /**
     * @brief Stop playback immediately.
     */
    void stop() override;

    /**
     * @brief Set attenuation (volume multiplier) for playback.
     * @param value Attenuation factor where 1.0 is no attenuation.
     */
    void setAttenuation(qreal value);

    /**
     * @brief Reset attenuation to default (unity) value.
     */
    void resetAttenuation();

  Q_SIGNALS:
    /**
     * @brief Emitted when an error occurs related to audio output.
     * @param message Human-readable error description.
     */
    void error(QString message) const;

    /**
     * @brief Emitted to report informational status updates.
     * @param message Status text.
     */
    void status(QString message) const;

  private:
    /**
     * @brief Verify the configured stream/sink is healthy and ready.
     * @return true if the stream is configured and usable.
     */
    bool checkStream() const;

  private Q_SLOTS:
    /**
     * @brief Internal handler for QAudioSink state changes.
     * @param state New sink state.
     */
    void handleStateChanged(QAudio::State state) const;

  private:
    QAudioDevice m_device;                   ///< Selected output device.
    QScopedPointer<QAudioSink> m_stream;    ///< Owned sink for playback.
    QAudioFormat m_format;                  ///< Configured audio format.
    unsigned m_msBuffered = 0u;             ///< Milliseconds of buffering.
    qreal m_volume = 1.0;                   ///< Current volume multiplier.
    bool m_error = false;                   ///< Last error flag.
};

#endif
