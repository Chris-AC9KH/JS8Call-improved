#include "TCIClient.h"
#include "TCIStreamFrame.h"
#include "TciAudioPcm.h"

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QVersionNumber>


#include <qloggingcategory.h>

Q_DECLARE_LOGGING_CATEGORY(tcinetworking_js8)

TCIClient::TCIClient(QObject *parent)
    : QObject(parent)
{
}

void TCIClient::createSocket()
{
    if (socket_)
        return;

    socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    connect(socket_, &QWebSocket::connected,
            this, &TCIClient::onConnected);

    connect(socket_, &QWebSocket::disconnected,
            this, &TCIClient::onDisconnected);

    connect(socket_, &QWebSocket::textMessageReceived,
            this, &TCIClient::onTextMessageReceived);

    connect(socket_, &QWebSocket::binaryMessageReceived,
            this, &TCIClient::onBinaryMessageReceived);

    connect(socket_,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this,
            &TCIClient::onErrorOccurred);
}

void TCIClient::destroySocket()
{
    if (!socket_)
        return;

    QWebSocket *socket = socket_;
    socket_ = nullptr;

    bool disconnected = socket->disconnect(this);
    Q_UNUSED(disconnected);

    socket->deleteLater();
}

void TCIClient::connectToServer(QUrl const &url)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, url]() {
                connectToServer(url);
            },
            Qt::QueuedConnection);
        return;
    }

    if (url_ == url && (connected_ || connecting_)) {
        qCInfo(tcinetworking_js8) << "TCI connection already active or pending:"
                << "client=" << this
                << "url=" << url;

        emitReadyIfReady();
        return;
    }

    connected_ = false;
    connecting_ = true;
    ready_ = false;
    url_ = url;

    resetProtocolState();
    push_policy_logged_ = false;

    destroySocket();
    createSocket();

    qCInfo(tcinetworking_js8) << "Opening TCI connection:"
            << "client=" << this
            << "url=" << url;

    socket_->open(url);
}

void TCIClient::disconnectFromServer()
{
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                disconnectFromServer();
            },
            Qt::QueuedConnection
        );
        return;
    }

    if (socket_)
        socket_->close();
}

bool TCIClient::isConnected() const
{
    return connected_;
}

bool TCIClient::isReady() const
{
    return ready_;
}

void TCIClient::setFrequency(qint64 hz)
{
    sendText(QStringLiteral("vfo:0,0,%1;").arg(hz));
}

void TCIClient::queryFrequency()
{
    sendText("vfo:0,0;");
}

void TCIClient::setTxFrequency(qint64 hz)
{
    sendText(QStringLiteral("vfo:0,1,%1;").arg(hz));
}

void TCIClient::queryTxFrequency()
{
    sendText("vfo:0,1;");
}

void TCIClient::setSplit(bool enabled)
{
    sendText(QStringLiteral("split_enable:0,%1;")
                 .arg(enabled ? "true" : "false"));
}

void TCIClient::querySplit()
{
    sendText("split_enable:0;");
}

void TCIClient::setMode(const QString &mode)
{
    sendText(QStringLiteral("modulation:0,%1;")
                 .arg(mode.trimmed().toUpper()));
}

void TCIClient::queryMode()
{
    sendText("modulation:0;");
}

void TCIClient::setPtt(bool enabled)
{
    sendText(enabled
        ? QStringLiteral("trx:0,true,tci;")
        : QStringLiteral("trx:0,false;"));
}

void TCIClient::queryPtt()
{
    sendText("trx:0;");
}

void TCIClient::configureAudio(int sampleRate, int channels, int samplesPerFrame)
{
    audio_sample_rate_ = sampleRate;
    audio_channels_ = channels;
    audio_samples_per_frame_ = samplesPerFrame;

    sendText("audio_stream_sample_type:float32;");
    sendText(QStringLiteral("audio_stream_channels:%1;").arg(audio_channels_));
    sendText(QStringLiteral("audio_stream_samples:%1;").arg(audio_samples_per_frame_));
    sendText(QStringLiteral("audio_samplerate:%1;").arg(audio_sample_rate_));
}

void TCIClient::startRxAudio()
{
    sendText("audio_start:0;");
}

void TCIClient::stopRxAudio()
{
    sendText("audio_stop:0;");
}

void TCIClient::sendTxAudio(QByteArray const &monoInt16Pcm,
                            TciTxAudioPolicy const &policy)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, monoInt16Pcm, policy]() {
                sendTxAudio(monoInt16Pcm, policy);
            },
            Qt::QueuedConnection);
        return;
    }

    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState)
        return;

    if (monoInt16Pcm.isEmpty())
        return;

    if (monoInt16Pcm.size() % qsizetype(sizeof(qint16))) {
        qWarning() << "Ignoring unaligned TCI TX PCM block:"
                   << monoInt16Pcm.size();
        return;
    }

    int const monoInputSamples =
        monoInt16Pcm.size() / int(sizeof(qint16));

    QByteArray payload;
    quint32 headerSampleCount = 0;

    // Internal modulator is 48-kHz mono int16; never relabel its rate.
    if (policy.sampleRate != 48000 ||
        (policy.channels != 1 && policy.channels != 2)) {
        qWarning() << "Unsupported TCI TX rate/channels:"
                   << policy.sampleRate << policy.channels;
        return;
    }

    // Production TX conversion uses the same PCM module exercised by CTest.
    // The header count is the total number of scalar values across channels.
    payload = TciAudioPcm::encodeFromMonoInt16(
        monoInt16Pcm,
        quint32(policy.sampleType),
        quint32(policy.channels),
        &headerSampleCount);

    if (payload.isEmpty()) {
        qWarning() << "Unsupported or invalid TCI TX audio format:"
                   << policy.sampleType << policy.channels;
        return;
    }

    if (headerSampleCount != quint32(monoInputSamples * policy.channels)) {
        qWarning() << "Unexpected TCI TX sample count:" << headerSampleCount;
        return;
    }

    QByteArray const frame =
        TCIStream::makeTxAudioFrameWithSampleCount(
            payload,
            headerSampleCount,
            0,
            quint32(policy.sampleRate),
            quint32(policy.sampleType),
            quint32(policy.channels));

    if (frame.size() <= 64) {
        qWarning() << "Refusing to send empty TCI TX audio frame:"
                   << "payloadBytes=" << payload.size()
                   << "frameBytes=" << frame.size();
        return;
    }

    if (!canSend())
        return;

    // Log the format once per TX instead of once per 512-sample frame.
    if (!tx_frame_logged_) {
        tx_frame_logged_ = true;
        qCInfo(tcinetworking_js8).noquote()
            << "TCI TX first audio frame:"
            << "stream=" << tciStreamTypeName(TCIStream::TX_AUDIO_STREAM)
            << "receiver=" << 0
            << "rate=" << policy.sampleRate
            << "type=" << tciSampleTypeName(policy.sampleType)
            << "sampleCount=" << headerSampleCount
            << "channels=" << policy.channels
            << "payloadBytes=" << payload.size()
            << "messageBytes=" << frame.size()
            << "sampleCountMeaning="
            << tciSampleCountMeaningName(policy.sampleCountMeaning);
    }

    socket_->sendBinaryMessage(frame);
}

void TCIClient::sendTxAudioMonoInt16(QByteArray const &pcm)
{
    // Derive push-mode format on the client's own thread, where the
    // protocol handler and observed capabilities are updated.  The
    // modulator supplies int16 PCM regardless of the wire format.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, pcm]() { sendTxAudioMonoInt16(pcm); },
            Qt::QueuedConnection);
        return;
    }

    if (!connected_ || !ready_ || pcm.isEmpty())
        return;

    ensureProtocolHandler();
    TciTxAudioPolicy policy =
        protocol_handler_->deriveTxAudioPolicy(server_info_, capabilities_);

    // The client-paced fallback must not accidentally adopt the format
    // from a TX_CHRONO frame. Only this function's push path is changed.
    if (policy.timing != TciTxAudioPolicy::Timing::ClientPaced)
        return;

    // Push pacing is fixed at 48 kHz / 512 mono input samples. Keep
    // the handler's version-specific wire sample type, but constrain
    // the remaining parameters to those this implementation produces.
    policy.sampleRate = 48000;
    policy.channels = 1;

    policy.samplesPerFrame = 512;

    if (!push_policy_logged_) {
        push_policy_logged_ = true;
        qCInfo(tcinetworking_js8).noquote()
            << "TCI TX push fallback:"
            << "server=" << server_info_.programName
            << "protocol=" << server_info_.protocolVersion
            << "generation=" << tciGenerationName(protocol_generation_)
            << "sampleType=" << tciSampleTypeName(policy.sampleType)
            << "sampleRate=" << policy.sampleRate
            << "channels=" << policy.channels
            << "samplesPerFrame=" << policy.samplesPerFrame
            << "sampleCountMeaning="
            << tciSampleCountMeaningName(policy.sampleCountMeaning);
        if (protocol_generation_ == TciProtocolGeneration::Unknown)
            qCWarning(tcinetworking_js8)
                << "Unknown TCI protocol; using conservative int16 push fallback";
    }

    sendTxAudio(pcm, policy);
}

void TCIClient::onConnected()
{
    connected_ = true;
    connecting_ = false;

    qCInfo(tcinetworking_js8) << "Connected to TCI server"
            << "client=" << this;

    Q_EMIT connected();
}

void TCIClient::onDisconnected()
{
    connected_ = false;
    connecting_ = false;
    ready_ = false;

    qCInfo(tcinetworking_js8) << "Disconnected from TCI server"
            << "client=" << this;

    destroySocket();

    Q_EMIT disconnected();
}

void TCIClient::onTextMessageReceived(const QString &message)
{
    const QString name = commandName(message);

    // Routine CAT polling is high-volume; retain it at debug level.
    if (name == "vfo" || name == "dds" ||
        name == "modulation" || name == "split_enable" ||
        name == "trx" || name == "ptt")
        qCDebug(tcinetworking_js8).noquote() << "TCI RX text:" << message;
    else
        qCInfo(tcinetworking_js8).noquote() << "TCI RX text:" << message;

    parseTextMessage(message);

    ensureProtocolHandler();

    const QStringList args = commandArgs(message);

    if (protocol_handler_)
        protocol_handler_->handleTextCommand(name,
                                             args,
                                             server_info_,
                                             capabilities_);

    if (name == "protocol")
        selectProtocolHandler();

    // Capture a complete server identity after the initial announcements.
    // Capture one negotiation snapshot rather than every polling reply.
    if (name == "ready") {
        if (!server_info_logged_) {
            logServerInfo();
            server_info_logged_ = true;
        }
        logCapabilities();
        logTxPolicy();
    }
}

void TCIClient::onBinaryMessageReceived(QByteArray const &message)
{
    TCIStream::StreamFrame const frame = TCIStream::parseFrame(message);

    if (!frame.valid) {
        qWarning() << "Invalid TCI binary frame:" << message.size();
        return;
    }

    // TX_CHRONO is a repetitive pacing request, not a one-time event.
    if (frame.streamType != TCIStream::RX_AUDIO_STREAM &&
        frame.streamType != TCIStream::TX_CHRONO)
        logBinaryFrame(QStringLiteral("RX"), frame, message.size());

    ensureProtocolHandler();

    if (protocol_handler_)
        protocol_handler_->handleBinaryFrame(frame, capabilities_);

    if (frame.streamType == TCIStream::TX_CHRONO) {
        if (!chrono_format_logged_) {
            chrono_format_logged_ = true;
            logBinaryFrame(QStringLiteral("RX"), frame, message.size());
            logCapabilities();
            // READY may have logged a push policy before CHRONO was known.
            policy_logged_ = false;
            logTxPolicy();
        }

        if (protocol_handler_) {
            TciTxAudioPolicy const policy =
                protocol_handler_->deriveTxAudioPolicy(server_info_, capabilities_);

            Q_EMIT txChronoRequested(policy);
        }

        return;
    }

    if (frame.streamType != TCIStream::RX_AUDIO_STREAM) {
        qCInfo(tcinetworking_js8) << "Ignoring non-RX TCI stream type:"
                << tciStreamTypeName(frame.streamType);
        return;
    }

    if (!rx_format_logged_) {
        rx_format_logged_ = true;
        logBinaryFrame(QStringLiteral("RX"), frame, message.size());
        logCapabilities();
    }

    if (frame.sampleRate != 48000) {
        qWarning() << "Unsupported TCI RX sample rate:" << frame.sampleRate;
        return;
    }

    QByteArray const monoInt16 =
    TciAudioPcm::decodeToMonoInt16(frame);

    if (monoInt16.isEmpty())
        return;

    Q_EMIT rxAudioFrame(monoInt16,
                        static_cast<int>(frame.sampleRate));
}

void TCIClient::onErrorOccurred(QAbstractSocket::SocketError socketError)
{
    QString const message =
        socket_ ? socket_->errorString()
                : tr("Unknown TCI socket error.");

    if (socketError == QAbstractSocket::RemoteHostClosedError) {
        qInfo() << "TCI socket closed by remote host:"
                << message;

        Q_EMIT error(tr("TCI disconnected"));

        return;
    }

    qWarning() << "TCI socket error:" << message;
    // A failed initial connect may not emit disconnected(). Without
    // clearing this flag subsequent retries return as 'already connecting'.
    if (!connected_) {
        connecting_ = false;
        destroySocket();
    }
    Q_EMIT error(message);
}

void TCIClient::resetProtocolState()
{
    server_info_ = {};
    capabilities_ = {};
    protocol_handler_.reset();
    protocol_generation_ = TciProtocolGeneration::Unknown;

    server_info_logged_ = false;
    policy_logged_ = false;
    push_policy_logged_ = false;
    rx_format_logged_ = false;
    chrono_format_logged_ = false;
    tx_frame_logged_ = false;
    ptt_state_seen_ = false;
    ptt_state_ = false;
}

void TCIClient::ensureProtocolHandler()
{
    if (protocol_handler_)
        return;

    protocol_handler_ = std::make_unique<TciProtocolUnknownHandler>();
    protocol_generation_ = TciProtocolGeneration::Unknown;
}

void TCIClient::selectProtocolHandler()
{
    QVersionNumber const version =
        QVersionNumber::fromString(server_info_.protocolVersion);

    TciProtocolGeneration next_generation =
        TciProtocolGeneration::Unknown;

    if (!version.isNull()) {
        QVersionNumber const modern_boundary(1, 10);

        if (version < modern_boundary)
            next_generation = TciProtocolGeneration::LegacyV1;
        else if (version.majorVersion() <= 2)
            next_generation = TciProtocolGeneration::V2;
        else
            next_generation = TciProtocolGeneration::Unknown;
    }

    if (protocol_handler_ && protocol_generation_ == next_generation)
        return;

    switch (next_generation) {
    case TciProtocolGeneration::LegacyV1:
        protocol_handler_ = std::make_unique<TciProtocolLegacyV1Handler>();
        break;

    case TciProtocolGeneration::V2:
        protocol_handler_ = std::make_unique<TciProtocolV2Handler>();
        break;

    case TciProtocolGeneration::Unknown:
        protocol_handler_ = std::make_unique<TciProtocolUnknownHandler>();
        break;
    }

    protocol_generation_ = next_generation;
    policy_logged_ = false;

    qCInfo(tcinetworking_js8).noquote()
        << "TCI protocol handler selected:"
        << tciGenerationName(protocol_generation_)
        << "program=" << server_info_.programName
        << "version=" << server_info_.protocolVersion;
}

void TCIClient::emitReadyIfReady()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                emitReadyIfReady();
            },
            Qt::QueuedConnection);
        return;
    }

    if (ready_)
        Q_EMIT ready();
}

void TCIClient::logServerInfo() const
{
    qCInfo(tcinetworking_js8).noquote()
        << "TCI server info:"
        << "program=" << server_info_.programName
        << "protocol=" << server_info_.protocolVersion
        << "device=" << server_info_.device;
}

void TCIClient::logCapabilities() const
{
    qCInfo(tcinetworking_js8).noquote()
        << "TCI capabilities:"
        << "ready=" << capabilities_.readySeen
        << "start=" << capabilities_.startSeen
        << "receiveOnlySeen=" << capabilities_.receiveOnlySeen
        << "receiveOnly=" << capabilities_.receiveOnly
        << "txEnableSeen=" << capabilities_.txEnableSeen
        << "txEnabled=" << capabilities_.txEnabled
        << "txChronoSeen=" << capabilities_.txChronoSeen
        << "textAudioType="
        << (capabilities_.textAudio.sampleTypeSeen
                ? tciSampleTypeName(capabilities_.textAudio.sampleType)
                : QStringLiteral("unknown"))
        << "textAudioChannels="
        << (capabilities_.textAudio.channelsSeen
                ? QString::number(capabilities_.textAudio.channels)
                : QStringLiteral("unknown"))
        << "textAudioSamples="
        << (capabilities_.textAudio.samplesSeen
                ? QString::number(capabilities_.textAudio.samplesPerFrame)
                : QStringLiteral("unknown"))
        << "textAudioRate="
        << (capabilities_.textAudio.sampleRateSeen
                ? QString::number(capabilities_.textAudio.sampleRate)
                : QStringLiteral("unknown"))
        << "rxAudioType="
        << (capabilities_.rxAudio.sampleTypeSeen
                ? tciSampleTypeName(capabilities_.rxAudio.sampleType)
                : QStringLiteral("unknown"))
        << "rxAudioChannels="
        << (capabilities_.rxAudio.channelsSeen
                ? QString::number(capabilities_.rxAudio.channels)
                : QStringLiteral("unknown"))
        << "rxAudioSamples="
        << (capabilities_.rxAudio.samplesSeen
                ? QString::number(capabilities_.rxAudio.samplesPerFrame)
                : QStringLiteral("unknown"))
        << "rxAudioRate="
        << (capabilities_.rxAudio.sampleRateSeen
                ? QString::number(capabilities_.rxAudio.sampleRate)
                : QStringLiteral("unknown"))
        << "txChronoType="
        << (capabilities_.txChronoAudio.sampleTypeSeen
                ? tciSampleTypeName(capabilities_.txChronoAudio.sampleType)
                : QStringLiteral("unknown"))
        << "txChronoChannels="
        << (capabilities_.txChronoAudio.channelsSeen
                ? QString::number(capabilities_.txChronoAudio.channels)
                : QStringLiteral("unknown"))
        << "txChronoSamples="
        << (capabilities_.txChronoAudio.samplesSeen
                ? QString::number(capabilities_.txChronoAudio.samplesPerFrame)
                : QStringLiteral("unknown"))
        << "txChronoRate="
        << (capabilities_.txChronoAudio.sampleRateSeen
                ? QString::number(capabilities_.txChronoAudio.sampleRate)
                : QStringLiteral("unknown"));
}

void TCIClient::logTxPolicy()
{
    if (!protocol_handler_)
        return;

    TciTxAudioPolicy const policy =
        protocol_handler_->deriveTxAudioPolicy(server_info_, capabilities_);

    /*
     * Log repeatedly until something meaningful has been observed. After that,
     * only log when the policy is first derived after handler selection.
     */
    if (policy_logged_)
        return;

    qCInfo(tcinetworking_js8).noquote()
        << "TCI derived TX policy:"
        << "handler=" << tciGenerationName(protocol_generation_)
        << "timing=" << tciTxTimingName(policy.timing)
        << "sampleType=" << tciSampleTypeName(policy.sampleType)
        << "channels=" << policy.channels
        << "samplesPerFrame=" << policy.samplesPerFrame
        << "sampleRate=" << policy.sampleRate
        << "sampleCountMeaning="
        << tciSampleCountMeaningName(policy.sampleCountMeaning);

    policy_logged_ = true;
}

void TCIClient::logBinaryFrame(QString const &direction,
                               TCIStream::StreamFrame const &frame,
                               qsizetype messageBytes) const
{
    qCInfo(tcinetworking_js8).noquote()
        << "TCI" << direction << "binary:"
        << "stream=" << tciStreamTypeName(frame.streamType)
        << "receiver=" << frame.receiver
        << "rate=" << frame.sampleRate
        << "type=" << tciSampleTypeName(frame.sampleType)
        << "sampleCount=" << frame.sampleCount
        << "channels=" << frame.channels
        << "payloadBytes=" << frame.payload.size()
        << "messageBytes=" << messageBytes;
}

void TCIClient::sendText(QString const &message)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, message]() {
                sendText(message);
            },
            Qt::QueuedConnection);
        return;
    }

    if (!canSend())
        return;

    socket_->sendTextMessage(message);
}

bool TCIClient::canSend() const
{
    return socket_
        && socket_->state() == QAbstractSocket::ConnectedState;
}

void TCIClient::parseTextMessage(const QString &message)
{
    const QString name = commandName(message);
    const QStringList args = commandArgs(message);

    if (name == "ready") {
        if (!ready_) {
            ready_ = true;
            emit ready();
        }

        return;
    }

    if (name == "vfo") {
        if (args.size() == 3 && args.at(0) == "0") {
            bool channelOk = false;
            const int channel = args.at(1).toInt(&channelOk);

            bool freqOk = false;
            const qint64 hz = args.at(2).toLongLong(&freqOk);

            if (channelOk && freqOk && hz > 0) {
                if (channel == 0)
                    emit frequencyChanged(hz);
                else if (channel == 1)
                    emit txFrequencyChanged(hz);
            }
        }

        return;
    }

    if (name == "dds")
    {
        // Receiver center frequency, not the tuned VFO frequency.
        // JS8Call does not currently consume this information.
        return;
    }

    if (name == "split_enable") {
        if (args.size() >= 2) {
            const QString value = args.at(1).trimmed().toLower();

            emit splitChanged(
                value == "true" ||
                value == "1" ||
                value == "on" ||
                value == "tx"
            );
        }

        return;
    }

    if (name == "modulation") {
        if (args.size() == 2 && args.at(0) == "0") {
            const QString mode = args.at(1).trimmed().toUpper();

            if (!mode.isEmpty())
                emit modeChanged(mode);
        }

        return;
    }

    if (name == "trx" || name == "ptt") {
        if (args.size() >= 2 && args.at(0) == "0") {
            const QString value = args.at(1).trimmed().toLower();

            const bool transmitting =
                value == "true" ||
                value == "1" ||
                value == "on" ||
                value == "tx";

            if (!ptt_state_seen_ || ptt_state_ != transmitting) {
                qCInfo(tcinetworking_js8)
                    << "TCI PTT state:" << (transmitting ? "TX" : "RX");
                ptt_state_seen_ = true;
                ptt_state_ = transmitting;
            }

            // A completed transmission permits one format log on next TX.
            if (!transmitting)
                tx_frame_logged_ = false;

            emit pttChanged(transmitting);
        }

        return;
    }
}

QString TCIClient::commandName(const QString &message)
{
    QString s = message.trimmed();

    if (s.endsWith(';'))
        s.chop(1);

    const int colon = s.indexOf(':');

    if (colon < 0)
        return s.trimmed().toLower();

    return s.left(colon).trimmed().toLower();
}

QStringList TCIClient::commandArgs(const QString &message)
{
    QString s = message.trimmed();

    if (s.endsWith(';'))
        s.chop(1);

    const int colon = s.indexOf(':');

    if (colon < 0)
        return {};

    const QString argString = s.mid(colon + 1).trimmed();

    if (argString.isEmpty())
        return {};

    QStringList out;

    const auto parts = argString.split(',', Qt::KeepEmptyParts);

    for (const QString &part : parts)
        out << part.trimmed();

    return out;
}

QString TCIClient::semicolon(QString message)
{
    message = message.trimmed();

    if (!message.endsWith(';'))
        message.append(';');

    return message;
}

Q_LOGGING_CATEGORY(tcinetworking_js8, "tcinetworking.js8", QtInfoMsg)
