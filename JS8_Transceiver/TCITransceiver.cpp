/**
 * @file TCITransceiver.cpp
 * @brief Implementation of the TCITransceiver class
 */

#include "TCITransceiver.h"

#include <QEventLoop>
#include <QLoggingCategory>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include "moc_TCITransceiver.cpp"

Q_DECLARE_LOGGING_CATEGORY(tcitransceiver_js8)

namespace {
char const *const tci_transceiver_name{"TCI Client"};

constexpr quint16 default_tci_port{50001};
constexpr int startup_timeout_ms{5000};

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
                               QObject *parent)
    : TransceiverBase{parent}, wrapped_{std::move(wrapped)},
      use_for_ptt_{use_for_ptt}, server_{address}, socket_{nullptr},
      startup_wait_{nullptr}, ready_{false}, vfo_b_{0} {}

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
    socket_ = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, this};
    connect(socket_, &QWebSocket::textMessageReceived, this,
            &TCITransceiver::handle_message);
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
    return 0;
}

void TCITransceiver::do_stop() {
    if (socket_) {
        disconnect(socket_, nullptr, this, nullptr);
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
        send(QString{"trx:0,%1;"}.arg(on ? "true" : "false"));
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
