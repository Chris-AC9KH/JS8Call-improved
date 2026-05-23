/**
 * @file NotificationSoundOutput.h
 * @brief Self-contained audio output for notification sounds.
 */

#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#ifdef Q_OS_LINUX
#include <QElapsedTimer>
#endif
#include <QObject>
#include <qmath.h>

#include <memory>

class NotificationSoundOutput : public QObject
{
    Q_OBJECT

public:
    explicit NotificationSoundOutput(QObject *parent = nullptr);
    ~NotificationSoundOutput() override;

    void setDevice(QAudioDevice const &device, unsigned msBuffer);
    void setAttenuation(qreal a);
    void play(QByteArray const &data, QAudioFormat const &format);
    void stop();

signals:
    void status(QString message);
    void error(QString message);

private slots:
    void handleStateChanged(QAudio::State newState);

private:
    void release();

    QAudioDevice                  m_device;
    std::unique_ptr<QAudioSink>   m_sink;
    std::unique_ptr<QBuffer>      m_buffer;
    QAudioFormat                  m_currentFormat;
#ifdef Q_OS_LINUX
    /**
     * @brief Monotonic timer used on Linux to coordinate audio playback.
     *
     * This member exists only on Linux because the implementation in the source file
     * uses a monotonic clock to measure the elapsed time between successive play()
     * requests. Tracking the interval helps avoid rapid teardown/recreation of
     * QAudioSink and mitigates backend-specific glitches (e.g., pops or missed starts)
     * observed with Linux audio stacks when sounds are triggered in quick succession.
     *
     * Declaring the timer here ensures the class layout reflects the Linux-specific
     * behavior while keeping all timing logic in the .cpp. Other platforms do not
     * require this workaround and therefore omit the member entirely.
     */
    QElapsedTimer m_lastPlayed;
#endif
    qreal                         m_volume  = 1.0;
    unsigned                      m_msBuffer = 0;
};
