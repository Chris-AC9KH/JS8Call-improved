/**
 * @file TCIAudio.cpp
 * @brief Implementation of the TCIAudio class
 */

#include "TCIAudio.h"

#include <QtMath>

#include "moc_TCIAudio.cpp"

void TCIAudio::start(AudioDevice *sink, AudioDevice *source) {
    stop();
    if (sink->initialize(QIODevice::WriteOnly, AudioDevice::Mono))
        m_sink = sink;
    m_source = source;
}

void TCIAudio::stop() {
    if (m_sink)
        m_sink->close();
    m_sink = nullptr;
    m_source = nullptr;
}

void TCIAudio::suspend() { m_suspended = true; }

void TCIAudio::resume() {
    if (m_sink)
        m_sink->reset();
    m_suspended = false;
}

namespace {
void scale(QByteArray &frames, qreal volume) {
    auto *sample = reinterpret_cast<qint16 *>(frames.data());
    for (qsizetype i = 0; i < frames.size() / qsizetype(sizeof(qint16)); ++i)
        sample[i] = static_cast<qint16>(sample[i] * volume);
}
} // namespace

void TCIAudio::setTxAttenuation(qreal dB) {
    m_txVolume = qPow(10.0, -dB / 20.0);
}

void TCIAudio::setRxAttenuation(qreal dB) {
    m_rxVolume = qPow(10.0, -dB / 20.0);
}

void TCIAudio::receive(QByteArray const &frames) {
    if (m_sink && !m_suspended) {
        QByteArray audio{frames};
        scale(audio, m_rxVolume);
        m_sink->write(audio);
    }
}

void TCIAudio::request(qint64 frames) {
    QByteArray audio(frames * qint64(sizeof(qint16)), '\0');
    if (m_source && m_source->isOpen()) {
        m_source->read(audio.data(), audio.size());
        scale(audio, m_txVolume);
    }
    Q_EMIT transmit(audio);
}
