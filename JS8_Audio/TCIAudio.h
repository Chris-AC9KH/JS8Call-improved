/**
 * @file TCIAudio.h
 * @brief Connect audio carried by the rig interface to the JS8 audio devices.
 */

#ifndef TCIAUDIO_H__
#define TCIAUDIO_H__

#include "JS8_Audio/AudioDevice.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>

/**
 * @brief Stands in for SoundInput and SoundOutput when the rig interface
 * carries the audio, as TCI does.
 *
 * Received frames are written to the sink, and each request for transmit
 * audio is answered from the source, or with silence while the source is
 * not open. Frames are 16-bit mono at 48 kHz. Lives in the audio thread.
 */
class TCIAudio final : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct a TCIAudio helper.
     * @param parent Optional parent QObject.
     */
    explicit TCIAudio(QObject *parent = nullptr) : QObject{parent} {}

    /**
     * @brief Start passing audio to and from the given devices.
     * @param sink Receives the rig's audio; opened here for writing.
     * @param source Supplies transmit audio while it is open.
     */
    Q_SLOT void start(AudioDevice *sink, AudioDevice *source);

    /**
     * @brief Stop passing audio and close the sink.
     */
    Q_SLOT void stop();

    /**
     * @brief Drop received audio until resume() is called.
     */
    Q_SLOT void suspend();

    /**
     * @brief Pass received audio to the sink again.
     */
    Q_SLOT void resume();

    /**
     * @brief Write audio received from the rig to the sink.
     * @param frames 16-bit mono frames at 48 kHz.
     */
    Q_SLOT void receive(QByteArray const &frames);

    /**
     * @brief Answer the rig's request for transmit audio.
     * @param frames Number of frames wanted.
     */
    Q_SLOT void request(qint64 frames);

    /**
     * @brief Set the transmit attenuation, as SoundOutput does.
     * @param dB Attenuation in dB, 0 for full scale.
     */
    Q_SLOT void setTxAttenuation(qreal dB);

    /**
     * @brief Set the receive attenuation, standing in for the sound card's
     * input level.
     * @param dB Attenuation in dB, 0 for the level the rig sends.
     */
    Q_SLOT void setRxAttenuation(qreal dB);

    /**
     * @brief Emitted with the transmit audio for one request.
     * @param frames 16-bit mono frames at 48 kHz.
     */
    Q_SIGNAL void transmit(QByteArray const &frames) const;

  private:
    QPointer<AudioDevice> m_sink;
    QPointer<AudioDevice> m_source;
    bool m_suspended = false;
    qreal m_txVolume = 1.0;
    qreal m_rxVolume = 1.0;
};

#endif
