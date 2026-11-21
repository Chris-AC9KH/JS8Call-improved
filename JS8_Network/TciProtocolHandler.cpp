#include "TciProtocolHandler.h"

#include <QDebug>

namespace
{
    bool parse_bool(QString const &value)
    {
        QString const v = value.trimmed().toLower();

        return v == "true" ||
               v == "1" ||
               v == "on" ||
               v == "yes" ||
               v == "tx";
    }

    bool parse_int(QString const &value, int &out)
    {
        bool ok = false;
        int const parsed = value.trimmed().toInt(&ok);

        if (!ok)
            return false;

        out = parsed;
        return true;
    }

    bool parse_sample_type(QString const &value,
                           TCIStream::SampleType &out)
    {
        QString const v = value.trimmed().toLower();

        if (v == "int16" || v == "i16" || v == "0") {
            out = TCIStream::INT16;
            return true;
        }

        if (v == "int24" || v == "i24" || v == "1") {
            out = TCIStream::INT24;
            return true;
        }

        if (v == "int32" || v == "i32" || v == "2") {
            out = TCIStream::INT32;
            return true;
        }

        if (v == "float32" || v == "float" || v == "f32" || v == "3") {
            out = TCIStream::FLOAT32;
            return true;
        }

        return false;
    }

    void observe_text_audio_command(QString const &name,
                                    QStringList const &args,
                                    TciCapabilities &capabilities)
    {
        if (args.isEmpty())
            return;

        QString const value = args.last();

        if (name == "audio_stream_sample_type") {
            TCIStream::SampleType sampleType = TCIStream::INT16;

            if (parse_sample_type(value, sampleType)) {
                capabilities.textAudio.sampleType = sampleType;
                capabilities.textAudio.sampleTypeSeen = true;
            }

            return;
        }

        if (name == "audio_stream_channels") {
            int channels = 0;

            if (parse_int(value, channels) && channels > 0) {
                capabilities.textAudio.channels = channels;
                capabilities.textAudio.channelsSeen = true;
            }

            return;
        }

        if (name == "audio_stream_samples") {
            int samples = 0;

            if (parse_int(value, samples) && samples > 0) {
                capabilities.textAudio.samplesPerFrame = samples;
                capabilities.textAudio.samplesSeen = true;
            }

            return;
        }

        if (name == "audio_samplerate" || name == "audio_sample_rate") {
            int sampleRate = 0;

            if (parse_int(value, sampleRate) && sampleRate > 0) {
                capabilities.textAudio.sampleRate = sampleRate;
                capabilities.textAudio.sampleRateSeen = true;
            }

            return;
        }
    }

    void handle_common_text_command(QString const &name,
                                    QStringList const &args,
                                    TciServerInfo &serverInfo,
                                    TciCapabilities &capabilities)
    {
        if (name == "protocol") {
            if (args.size() >= 1)
                serverInfo.programName = args.at(0).trimmed();

            if (args.size() >= 2)
                serverInfo.protocolVersion = args.at(1).trimmed();

            return;
        }

        if (name == "device") {
            if (!args.isEmpty())
                serverInfo.device = args.last().trimmed();

            return;
        }

        if (name == "ready") {
            capabilities.readySeen = true;
            return;
        }

        if (name == "start") {
            capabilities.startSeen = true;
            return;
        }

        if (name == "receive_only") {
            if (!args.isEmpty()) {
                capabilities.receiveOnlySeen = true;
                capabilities.receiveOnly = parse_bool(args.last());
            }

            return;
        }

        if (name == "tx_enable") {
            if (!args.isEmpty()) {
                capabilities.txEnableSeen = true;
                capabilities.txEnabled = parse_bool(args.last());
            }

            return;
        }

        observe_text_audio_command(name, args, capabilities);
    }

void handle_common_binary_frame(TCIStream::StreamFrame const &frame,
                            TciCapabilities &capabilities)
    {
        if (frame.streamType == TCIStream::TX_CHRONO) {
            capabilities.txChronoSeen = true;

            capabilities.txChronoAudio.sampleType =
                static_cast<TCIStream::SampleType>(frame.sampleType);
            capabilities.txChronoAudio.sampleTypeSeen = true;

            capabilities.txChronoAudio.channels =
                static_cast<int>(frame.channels);
            capabilities.txChronoAudio.channelsSeen = frame.channels > 0;

            capabilities.txChronoAudio.samplesPerFrame =
                static_cast<int>(frame.sampleCount);
            capabilities.txChronoAudio.samplesSeen = frame.sampleCount > 0;

            capabilities.txChronoAudio.sampleRate =
                static_cast<int>(frame.sampleRate);
            capabilities.txChronoAudio.sampleRateSeen = frame.sampleRate > 0;

            return;
        }

        if (frame.streamType == TCIStream::RX_AUDIO_STREAM) {
            capabilities.rxAudio.sampleType =
                static_cast<TCIStream::SampleType>(frame.sampleType);
            capabilities.rxAudio.sampleTypeSeen = true;

            capabilities.rxAudio.channels =
                static_cast<int>(frame.channels);
            capabilities.rxAudio.channelsSeen = frame.channels > 0;

            capabilities.rxAudio.samplesPerFrame =
                static_cast<int>(frame.sampleCount);
            capabilities.rxAudio.samplesSeen = frame.sampleCount > 0;

            capabilities.rxAudio.sampleRate =
                static_cast<int>(frame.sampleRate);
            capabilities.rxAudio.sampleRateSeen = frame.sampleRate > 0;
        }
    }

    TciTxAudioPolicy default_push_int16_policy()
    {
        TciTxAudioPolicy policy;
        policy.timing = TciTxAudioPolicy::Timing::ClientPaced;
        policy.sampleRate = 48000;
        policy.channels = 1;
        policy.sampleType = TCIStream::INT16;
        policy.samplesPerFrame = 512;
        policy.sampleCountMeaning =
            TciTxAudioPolicy::SampleCountMeaning::FramesPerChannel;
        return policy;
    }

    TciTxAudioPolicy chrono_policy_from_observed_audio(TciCapabilities const &capabilities)
    {
        TciTxAudioPolicy policy = default_push_int16_policy();

        policy.timing = TciTxAudioPolicy::Timing::ChronoDriven;

        if (capabilities.txChronoAudio.sampleRateSeen)
            policy.sampleRate = capabilities.txChronoAudio.sampleRate;
        else if (capabilities.textAudio.sampleRateSeen)
            policy.sampleRate = capabilities.textAudio.sampleRate;
        else if (capabilities.rxAudio.sampleRateSeen)
            policy.sampleRate = capabilities.rxAudio.sampleRate;

        if (capabilities.txChronoAudio.samplesSeen)
            policy.samplesPerFrame = capabilities.txChronoAudio.samplesPerFrame;
        else if (capabilities.textAudio.samplesSeen)
            policy.samplesPerFrame = capabilities.textAudio.samplesPerFrame;
        else if (capabilities.rxAudio.samplesSeen)
            policy.samplesPerFrame = capabilities.rxAudio.samplesPerFrame;

        if (capabilities.txChronoAudio.sampleTypeSeen)
            policy.sampleType = capabilities.txChronoAudio.sampleType;
        else if (capabilities.textAudio.sampleTypeSeen)
            policy.sampleType = capabilities.textAudio.sampleType;
        else if (capabilities.rxAudio.sampleTypeSeen)
            policy.sampleType = capabilities.rxAudio.sampleType;

        if (capabilities.txChronoAudio.channelsSeen)
            policy.channels = capabilities.txChronoAudio.channels;
        else if (capabilities.textAudio.channelsSeen)
            policy.channels = capabilities.textAudio.channels;
        else if (capabilities.rxAudio.channelsSeen)
            policy.channels = capabilities.rxAudio.channels;

        return policy;
    }
}

void TciProtocolLegacyV1Handler::handleTextCommand(QString const &name,
                                                   QStringList const &args,
                                                   TciServerInfo &serverInfo,
                                                   TciCapabilities &capabilities)
{
    handle_common_text_command(name, args, serverInfo, capabilities);
}

void TciProtocolLegacyV1Handler::handleBinaryFrame(TCIStream::StreamFrame const &frame,
                                                   TciCapabilities &capabilities)
{
    handle_common_binary_frame(frame, capabilities);
}

TciTxAudioPolicy TciProtocolLegacyV1Handler::deriveTxAudioPolicy(
    TciServerInfo const &serverInfo,
    TciCapabilities const &capabilities) const
{
    Q_UNUSED(serverInfo);

    if (!capabilities.txChronoSeen)
        return default_push_int16_policy();

    TciTxAudioPolicy policy = chrono_policy_from_observed_audio(capabilities);

    policy.sampleCountMeaning =
        TciTxAudioPolicy::SampleCountMeaning::TotalSampleValues;

    return policy;
}

void TciProtocolV2Handler::handleTextCommand(QString const &name,
                                             QStringList const &args,
                                             TciServerInfo &serverInfo,
                                             TciCapabilities &capabilities)
{
    handle_common_text_command(name, args, serverInfo, capabilities);
}

void TciProtocolV2Handler::handleBinaryFrame(TCIStream::StreamFrame const &frame,
                                             TciCapabilities &capabilities)
{
    handle_common_binary_frame(frame, capabilities);
}

TciTxAudioPolicy TciProtocolV2Handler::deriveTxAudioPolicy(TciServerInfo const &serverInfo,
                                                           TciCapabilities const &capabilities) const
{
    Q_UNUSED(serverInfo);

    if (!capabilities.txChronoSeen) {
        TciTxAudioPolicy policy = default_push_int16_policy();
        policy.sampleType = TCIStream::FLOAT32;
        policy.sampleCountMeaning =
            TciTxAudioPolicy::SampleCountMeaning::TotalSampleValues;
        return policy;
    }

    TciTxAudioPolicy policy = chrono_policy_from_observed_audio(capabilities);
    policy.sampleCountMeaning =
        TciTxAudioPolicy::SampleCountMeaning::TotalSampleValues;

    return policy;
}

void TciProtocolUnknownHandler::handleTextCommand(QString const &name,
                                                  QStringList const &args,
                                                  TciServerInfo &serverInfo,
                                                  TciCapabilities &capabilities)
{
    handle_common_text_command(name, args, serverInfo, capabilities);
}

void TciProtocolUnknownHandler::handleBinaryFrame(TCIStream::StreamFrame const &frame,
                                                  TciCapabilities &capabilities)
{
    handle_common_binary_frame(frame, capabilities);
}

TciTxAudioPolicy TciProtocolUnknownHandler::deriveTxAudioPolicy(TciServerInfo const &serverInfo,
                                                                TciCapabilities const &capabilities) const
{
    Q_UNUSED(serverInfo);
    Q_UNUSED(capabilities);

    /*
     * Unknown future protocols should not silently get new TX behavior.
     * For this observe-only pass, leave the existing client-paced path alone.
     */
    return default_push_int16_policy();
}

QString tciSampleTypeName(quint32 sampleType)
{
    switch (sampleType) {
    case TCIStream::INT16:
        return QStringLiteral("int16");
    case TCIStream::INT24:
        return QStringLiteral("int24");
    case TCIStream::INT32:
        return QStringLiteral("int32");
    case TCIStream::FLOAT32:
        return QStringLiteral("float32");
    default:
        return QStringLiteral("unknown(%1)").arg(sampleType);
    }
}

QString tciStreamTypeName(quint32 streamType)
{
    switch (streamType) {
    case TCIStream::IQ_STREAM:
        return QStringLiteral("IQ_STREAM");
    case TCIStream::RX_AUDIO_STREAM:
        return QStringLiteral("RX_AUDIO_STREAM");
    case TCIStream::TX_AUDIO_STREAM:
        return QStringLiteral("TX_AUDIO_STREAM");
    case TCIStream::TX_CHRONO:
        return QStringLiteral("TX_CHRONO");
    case TCIStream::LINEOUT_STREAM:
        return QStringLiteral("LINEOUT_STREAM");
    default:
        return QStringLiteral("unknown(%1)").arg(streamType);
    }
}

QString tciGenerationName(TciProtocolGeneration generation)
{
    switch (generation) {
    case TciProtocolGeneration::LegacyV1:
        return QStringLiteral("legacy-v1");
    case TciProtocolGeneration::V2:
        return QStringLiteral("v2");
    case TciProtocolGeneration::Unknown:
        return QStringLiteral("unknown");
    }

    return QStringLiteral("unknown");
}

QString tciTxTimingName(TciTxAudioPolicy::Timing timing)
{
    switch (timing) {
    case TciTxAudioPolicy::Timing::ClientPaced:
        return QStringLiteral("client-paced");
    case TciTxAudioPolicy::Timing::ChronoDriven:
        return QStringLiteral("chrono-driven");
    }

    return QStringLiteral("unknown");
}

QString tciSampleCountMeaningName(TciTxAudioPolicy::SampleCountMeaning meaning)
{
    switch (meaning) {
    case TciTxAudioPolicy::SampleCountMeaning::FramesPerChannel:
        return QStringLiteral("frames-per-channel");
    case TciTxAudioPolicy::SampleCountMeaning::TotalSampleValues:
        return QStringLiteral("total-sample-values");
    case TciTxAudioPolicy::SampleCountMeaning::Unknown:
        return QStringLiteral("unknown");
    }

    return QStringLiteral("unknown");
}