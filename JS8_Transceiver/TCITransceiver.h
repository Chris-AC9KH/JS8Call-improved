/**
 * @file TCITransceiver.h
 * @brief Declares the TCI rig control interface.
 */

#ifndef TCI_TRANSCEIVER_HPP__
#define TCI_TRANSCEIVER_HPP__

#include "TransceiverBase.h"
#include "TransceiverFactory.h"

#include <QStringList>

#include <memory>

class QEventLoop;
class QWebSocket;

/**
 * @brief Rig control over the TCI WebSocket protocol.
 *
 * Talks to a TCI server such as ExpertSDR3, Thetis or 590Ctrl. The server
 * pushes every state change, so there is no polling. Command, mode and
 * boolean names are matched without regard to letter case, as the TCI
 * specification requires. Only receiver 0 is used: channel 0 (VFO A) for
 * receive and channel 1 (VFO B) for a split transmit frequency.
 *
 * A wrapped transceiver, when given, supplies PTT by a method other than
 * CAT, as for the other network rig interfaces.
 *
 * With TCI audio the receiver audio stream and transmit audio travel over
 * the same connection, so no sound card is needed. The stream is set up
 * after READY at 48 kHz, and transmit audio is sent in whatever format and
 * channel count each TX_CHRONO request asks for.
 */
class TCITransceiver final : public TransceiverBase {
    Q_OBJECT; // for translation context

  public:
    /**
     * @brief Add the TCI interface to the factory's list of rigs.
     * @param registry Factory list to add to.
     * @param id Model number to register it under.
     */
    static void register_transceivers(TransceiverFactory::Transceivers *registry,
                                      int id);

    /**
     * @brief Construct a TCI transceiver.
     * @param wrapped Transceiver for non-CAT PTT, or null.
     * @param address Server as host, host:port or ws://host:port.
     * @param use_for_ptt Key the radio with TCI TRX commands.
     * @param tci_audio Carry receive and transmit audio over TCI.
     * @param parent Optional parent QObject.
     */
    explicit TCITransceiver(std::unique_ptr<TransceiverBase> wrapped,
                            QString const &address, bool use_for_ptt,
                            bool tci_audio, QObject *parent = nullptr);

    void send_audio(QByteArray const &frames) noexcept override;

  private:
    int do_start() override;
    void do_stop() override;
    void do_frequency(Frequency, MODE, bool no_ignore) override;
    void do_tx_frequency(Frequency, MODE, bool no_ignore) override;
    void do_mode(MODE) override;
    void do_ptt(bool on) override;
    void do_sync(bool force_signal, bool no_poll) override;

    void send(QString const &command);
    void handle_message(QString const &message);
    void handle_command(QString const &name, QStringList const &args);
    void handle_binary(QByteArray const &data);
    void handle_disconnected();
    void update_vfo_b();

    std::unique_ptr<TransceiverBase> wrapped_; // may be null
    bool use_for_ptt_;
    bool tci_audio_;
    QString server_;
    QWebSocket *socket_;
    QEventLoop *startup_wait_; // non-null while do_start() waits for READY
    bool ready_;
    Frequency vfo_b_;
    quint32 tx_rate_;     // from the last TX_CHRONO
    quint32 tx_format_;   // from the last TX_CHRONO
    quint32 tx_channels_; // from the last TX_CHRONO, 0 before the first
    quint32 tx_count_;
};

#endif
