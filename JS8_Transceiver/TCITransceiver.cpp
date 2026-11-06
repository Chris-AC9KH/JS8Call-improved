/**
 * @file TCITransceiver.cpp
 * @brief Implementation of the TCITransceiver class
 */

#include "TCITransceiver.h"

#include <QEventLoop>
#include <QtEndian>
#include <QLoggingCategory>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <algorithm>

#include "moc_TCITransceiver.cpp"

Q_DECLARE_LOGGING_CATEGORY(tcitransceiver_js8)

namespace {
char const *const tci_transceiver_name{"TCI Client"};

constexpr quint16 default_tci_port{50001};
constexpr int startup_timeout_ms{5000};

// TCI audio blocks start with a 64-byte header of little-endian 32-bit
// words: receiver, sample_rate, format, codec, crc, length (samples across
// all channels), type, channels, count, then reserved words.
constexpr qsizetype header_bytes{64};
enum HeaderWord { receiver, sample_rate, format, length = 5, type, channels,
                  count };
enum StreamType : quint32 { rx_audio_stream = 1, tx_audio_stream, tx_chrono };
enum SampleType : quint32 { int16, int24, int32, float32 };
constexpr quint32 audio_rate{48000};

int sample_bytes(quint32 format) {
    return format == int16 ? 2 : format == int24 ? 3 : 4;
}

qint16 read_sample(char const *p, quint32 format) {
    switch (format) {
    case int16:
        return qFromLittleEndian<qint16>(p);
    case int24:
        return qFromLittleEndian<qint16>(p + 1);
    case int32:
        return static_cast<qint16>(qFromLittleEndian<qint32>(p) >> 16);
    default:
        return static_cast<qint16>(
            std::clamp(qFromLittleEndian<float>(p), -1.f, 1.f) * 32767.f);
    }
}

void write_sample(char *p, qint16 sample, quint32 format) {
    switch (format) {
    case int16:
        qToLittleEndian(sample, p);
        break;
    case int24:
        qToLittleEndian(sample, p + 1);
        break;
    case int32:
        qToLittleEndian(static_cast<qint32>(sample) * 65536, p);
        break;
    default:
        qToLittleEndian(sample / 32768.f, p);
        break;
    }
}

QString map_mode(Transceiver::MODE mode) {
    switch (mode) {
    case Transceiver::AM:
        return "am";
    case Transceiver::CW:
    case Transceiver::CW_R:
        return "cw";
    case Transceiver::USB:
        return "usb";
    case Transceiver::LSB:
        return "lsb";
    case Transceiver::DIG_U:
        return "digu";
    case Transceiver::DIG_L:
        return "digl";
    case Transceiver::FM:
    case Transceiver::DIG_FM:
        return "nfm";
    default:
        break;
    }
    return {};
}

Transceiver::MODE map_mode(QString const &name) {
    auto const mode = name.toLower();
    if (mode == "am" || mode == "sam" || mode == "dsb")
        return Transceiver::AM;
    if (mode == "cw")
        return Transceiver::CW;
    if (mode == "cwr")
        return Transceiver::CW_R;
    if (mode == "usb")
        return Transceiver::USB;
    if (mode == "lsb")
        return Transceiver::LSB;
    if (mode == "digu")
        return Transceiver::DIG_U;
    if (mode == "digl")
        return Transceiver::DIG_L;
    if (mode == "fsk")
        return Transceiver::FSK;
    if (mode == "fskr")
        return Transceiver::FSK_R;
    if (mode == "nfm" || mode == "wfm")
        return Transceiver::FM;
    return Transceiver::UNK;
}

// TCI booleans are "true" or "1" in any letter case; anything else is false.
bool to_bool(QString const &value) {
    return value.compare("true", Qt::CaseInsensitive) == 0 || value == "1";
}
} // namespace

void TCITransceiver::register_transceivers(
    TransceiverFactory::Transceivers *registry, int id) {
    (*registry)[tci_transceiver_name] = TransceiverFactory::Capabilities{
        id, TransceiverFactory::Capabilities::network, true, false, false,
        true};
}

TCITransceiver::TCITransceiver(std::unique_ptr<TransceiverBase> wrapped,
                               QString const &address, bool use_for_ptt,
                               bool tci_audio, QObject *parent)
    : TransceiverBase{parent}, wrapped_{std::move(wrapped)},
      use_for_ptt_{use_for_ptt}, tci_audio_{tci_audio}, server_{address},
      socket_{nullptr}, startup_wait_{nullptr}, ready_{false}, vfo_b_{0},
      tx_rate_{audio_rate}, tx_format_{float32}, tx_channels_{0},
      tx_count_{0} {}

int TCITransceiver::do_start() {
    TRACE_CAT("TCITransceiver", "starting");
    if (wrapped_)
        wrapped_->start(0);

    auto server = server_.trimmed();
    if (server.isEmpty())
        server = "localhost";
    if (!server.contains("://"))
        server.prepend("ws://");
    QUrl url{server};
    if (!url.isValid() || url.host().isEmpty())
        throw error{tr("Invalid TCI server address: %1").arg(server_)};
    if (url.port() < 0)
        url.setPort(default_tci_port);

    ready_ = false;
    vfo_b_ = 0;
    tx_channels_ = 0;
    socket_ = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, this};
    connect(socket_, &QWebSocket::textMessageReceived, this,
            &TCITransceiver::handle_message);
    connect(socket_, &QWebSocket::binaryMessageReceived, this,
            &TCITransceiver::handle_binary);
    connect(socket_, &QWebSocket::disconnected, this,
            &TCITransceiver::handle_disconnected);

    QEventLoop wait;
    startup_wait_ = &wait;
    QTimer::singleShot(startup_timeout_ms, &wait, &QEventLoop::quit);
    socket_->open(url);
    wait.exec();
    startup_wait_ = nullptr;

    if (!ready_) {
        auto const reason = socket_->error() == QAbstractSocket::UnknownSocketError
                                ? tr("no READY from the server")
                                : socket_->errorString();
        throw error{tr("Failed to connect to TCI server %1\n%2")
                        .arg(url.toString(), reason)};
    }

    // READY ends the greeting; ask for the state we track in case the
    // server did not include it.
    send("vfo:0,0;");
    send("vfo:0,1;");
    send("modulation:0;");
    send("split_enable:0;");
    send("trx:0;");
    if (tci_audio_) {
        // The server must be streaming before TRX asks for TCI audio.
        send(QString{"audio_samplerate:%1;"}.arg(audio_rate));
        send("audio_stream_sample_type:float32;");
        send("audio_stream_channels:1;");
        send("audio_start:0;");
    }
    return 0;
}

void TCITransceiver::do_stop() {
    if (socket_) {
        disconnect(socket_, nullptr, this, nullptr);
        if (tci_audio_ &&
            socket_->state() == QAbstractSocket::ConnectedState)
            socket_->sendTextMessage("audio_stop:0;");
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    ready_ = false;
    if (wrapped_)
        wrapped_->stop();
    TRACE_CAT("TCITransceiver", "stopped");
}

void TCITransceiver::do_frequency(Frequency f, MODE m, bool) {
    TRACE_CAT("TCITransceiver", f << m);
    send(QString{"vfo:0,0,%1;"}.arg(f));
    update_rx_frequency(f);
    if (m != UNK)
        do_mode(m);
}

void TCITransceiver::do_tx_frequency(Frequency f, MODE, bool) {
    TRACE_CAT("TCITransceiver", f);
    if (f) {
        send(QString{"vfo:0,1,%1;"}.arg(f));
        send("split_enable:0,true;");
        vfo_b_ = f;
    } else {
        send("split_enable:0,false;");
    }
    update_split(f);
    update_other_frequency(f);
}

void TCITransceiver::do_mode(MODE m) {
    TRACE_CAT("TCITransceiver", m);
    if (auto const name = map_mode(m); !name.isEmpty()) {
        send(QString{"modulation:0,%1;"}.arg(name));
        update_mode(m);
    }
}

void TCITransceiver::do_ptt(bool on) {
    TRACE_CAT("TCITransceiver", on << state());
    if (use_for_ptt_) {
        tx_count_ = 0;
        send(on && tci_audio_ ? "trx:0,true,tci;"
                              : QString{"trx:0,%1;"}.arg(on ? "true" : "false"));
    } else {
        Q_ASSERT(wrapped_);
        TransceiverState new_state{wrapped_->state()};
        new_state.ptt(on);
        wrapped_->set(new_state, 0);
    }
    update_PTT(on);
}

void TCITransceiver::do_sync(bool force_signal, bool) {
    update_complete(force_signal);
}

void TCITransceiver::send_audio(QByteArray const &frames) noexcept {
    if (!tx_channels_ || !socket_ ||
        socket_->state() != QAbstractSocket::ConnectedState)
        return;

    auto const samples = frames.size() / qsizetype(sizeof(qint16));
    auto const bytes = sample_bytes(tx_format_);
    QByteArray block(header_bytes + samples * tx_channels_ * bytes, '\0');
    auto const header = [&block](HeaderWord word, quint32 value) {
        qToLittleEndian(value, block.data() + word * 4);
    };
    header(sample_rate, tx_rate_);
    header(format, tx_format_);
    header(length, quint32(samples * tx_channels_));
    header(type, tx_audio_stream);
    header(channels, tx_channels_);
    header(count, tx_count_++);

    auto const *in = reinterpret_cast<qint16 const *>(frames.constData());
    auto *out = block.data() + header_bytes;
    for (qsizetype i = 0; i < samples; ++i) {
        for (quint32 c = 0; c < tx_channels_; ++c, out += bytes)
            write_sample(out, in[i], tx_format_);
    }
    socket_->sendBinaryMessage(block);
}

void TCITransceiver::send(QString const &command) {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState)
        throw error{tr("TCI server is not connected")};
    qCDebug(tcitransceiver_js8) << "TCI >" << command;
    socket_->sendTextMessage(command);
}

void TCITransceiver::handle_message(QString const &message) {
    qCDebug(tcitransceiver_js8) << "TCI <" << message;
    for (auto const &command : message.split(';', Qt::SkipEmptyParts)) {
        auto const colon = command.indexOf(':');
        auto const name = command.left(colon).trimmed().toLower();
        QStringList args;
        if (colon >= 0) {
            for (auto const &arg :
                 command.mid(colon + 1).split(',', Qt::SkipEmptyParts))
                args << arg.trimmed();
        }
        handle_command(name, args);
    }
    if (ready_ && !startup_wait_)
        update_complete();
}

void TCITransceiver::handle_command(QString const &name,
                                    QStringList const &args) {
    if (name == "ready") {
        ready_ = true;
        if (startup_wait_)
            startup_wait_->quit();
        return;
    }

    // Everything else we track is per receiver; we only use receiver 0.
    if (args.isEmpty() || args[0] != "0")
        return;

    if (name == "vfo" && args.size() >= 3) {
        bool ok;
        auto const f = args[2].toULongLong(&ok);
        if (!ok)
            return;
        if (args[1] == "0") {
            update_rx_frequency(f);
        } else if (args[1] == "1") {
            vfo_b_ = f;
            update_vfo_b();
        }
    } else if (name == "modulation" && args.size() >= 2) {
        if (auto const m = map_mode(args[1]); m != UNK)
            update_mode(m);
    } else if (name == "split_enable" && args.size() >= 2) {
        update_split(to_bool(args[1]));
        update_vfo_b();
    } else if (name == "trx" && args.size() >= 2 && use_for_ptt_) {
        update_PTT(to_bool(args[1]));
    }
}

void TCITransceiver::handle_binary(QByteArray const &data) {
    if (!tci_audio_ || data.size() < header_bytes)
        return;
    auto const header = [&data](HeaderWord word) {
        return qFromLittleEndian<quint32>(data.constData() + word * 4);
    };
    auto const block_format = header(format);
    auto const block_channels = std::max(header(channels), 1u);
    if (block_format > float32)
        return;

    if (header(type) == tx_chrono) {
        tx_rate_ = header(sample_rate);
        tx_format_ = block_format;
        tx_channels_ = block_channels;
        Q_EMIT audio_requested(header(length) / block_channels);
    } else if (header(type) == rx_audio_stream && header(receiver) == 0) {
        if (header(sample_rate) != audio_rate) {
            offline(tr("TCI server sends %1 Hz audio; JS8Call needs %2 Hz")
                        .arg(header(sample_rate))
                        .arg(audio_rate));
            return;
        }
        auto const bytes = sample_bytes(block_format);
        auto const samples = std::min<qsizetype>(
            header(length), (data.size() - header_bytes) / bytes);
        auto const frames = samples / block_channels;
        QByteArray audio(frames * qsizetype(sizeof(qint16)), Qt::Uninitialized);
        auto *out = reinterpret_cast<qint16 *>(audio.data());
        auto const *in = data.constData() + header_bytes;
        for (qsizetype i = 0; i < frames; ++i, in += block_channels * bytes)
            out[i] = read_sample(in, block_format);
        Q_EMIT audio_received(audio);
    }
}

void TCITransceiver::handle_disconnected() {
    if (startup_wait_) {
        startup_wait_->quit();
    } else {
        offline(tr("TCI server closed the connection"));
    }
}

void TCITransceiver::update_vfo_b() {
    update_other_frequency(state().split() ? vfo_b_ : 0);
}

Q_LOGGING_CATEGORY(tcitransceiver_js8, "tcitransceiver.js8", QtWarningMsg)
