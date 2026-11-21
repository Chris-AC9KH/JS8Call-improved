#pragma once

#include "TCIStreamFrame.h"

#include <QMetaType>
#include <QString>
#include <QStringList>

struct TciServerInfo
{
    QString programName;
    QString protocolVersion;
    QString device;
};

struct TciObservedAudio
{
    bool sampleTypeSeen = false;
    bool channelsSeen = false;
    bool samplesSeen = false;
    bool sampleRateSeen = false;

    TCIStream::SampleType sampleType = TCIStream::INT16;
    int channels = 1;
    int samplesPerFrame = 512;
    int sampleRate = 48000;
};

struct TciCapabilities
{
    bool readySeen = false;
    bool startSeen = false;
    bool receiveOnlySeen = false;
    bool receiveOnly = false;

    bool txEnableSeen = false;
    bool txEnabled = false;

    bool txChronoSeen = false;

    TciObservedAudio textAudio;
    TciObservedAudio rxAudio;
    TciObservedAudio txChronoAudio;
};

enum class TciProtocolGeneration
{
    LegacyV1,  // protocol version < 1.10
    V2,        // protocol version >= 1.10 through 2.x
    Unknown
};

struct TciTxAudioPolicy
{
    enum class Timing
    {
        ClientPaced,
        ChronoDriven
    };

    enum class SampleCountMeaning
    {
        FramesPerChannel,
        TotalSampleValues,
        Unknown
    };

    Timing timing = Timing::ClientPaced;
    int sampleRate = 48000;
    int channels = 1;
    TCIStream::SampleType sampleType = TCIStream::INT16;
    int samplesPerFrame = 512;

    SampleCountMeaning sampleCountMeaning =
        SampleCountMeaning::FramesPerChannel;
};

Q_DECLARE_METATYPE(TciTxAudioPolicy)

class TciProtocolHandler
{
public:
    virtual ~TciProtocolHandler() = default;

    virtual TciProtocolGeneration generation() const = 0;

    virtual void handleTextCommand(QString const &name,
                                   QStringList const &args,
                                   TciServerInfo &serverInfo,
                                   TciCapabilities &capabilities) = 0;

    virtual void handleBinaryFrame(TCIStream::StreamFrame const &frame,
                                   TciCapabilities &capabilities) = 0;

    virtual TciTxAudioPolicy deriveTxAudioPolicy(TciServerInfo const &serverInfo,
                                                 TciCapabilities const &capabilities) const = 0;
};

class TciProtocolLegacyV1Handler final : public TciProtocolHandler
{
public:
    TciProtocolGeneration generation() const override
    {
        return TciProtocolGeneration::LegacyV1;
    }

    void handleTextCommand(QString const &name,
                           QStringList const &args,
                           TciServerInfo &serverInfo,
                           TciCapabilities &capabilities) override;

    void handleBinaryFrame(TCIStream::StreamFrame const &frame,
                           TciCapabilities &capabilities) override;

    TciTxAudioPolicy deriveTxAudioPolicy(TciServerInfo const &serverInfo,
                                         TciCapabilities const &capabilities) const override;
};

class TciProtocolV2Handler final : public TciProtocolHandler
{
public:
    TciProtocolGeneration generation() const override
    {
        return TciProtocolGeneration::V2;
    }

    void handleTextCommand(QString const &name,
                           QStringList const &args,
                           TciServerInfo &serverInfo,
                           TciCapabilities &capabilities) override;

    void handleBinaryFrame(TCIStream::StreamFrame const &frame,
                           TciCapabilities &capabilities) override;

    TciTxAudioPolicy deriveTxAudioPolicy(TciServerInfo const &serverInfo,
                                         TciCapabilities const &capabilities) const override;
};

class TciProtocolUnknownHandler final : public TciProtocolHandler
{
public:
    TciProtocolGeneration generation() const override
    {
        return TciProtocolGeneration::Unknown;
    }

    void handleTextCommand(QString const &name,
                           QStringList const &args,
                           TciServerInfo &serverInfo,
                           TciCapabilities &capabilities) override;

    void handleBinaryFrame(TCIStream::StreamFrame const &frame,
                           TciCapabilities &capabilities) override;

    TciTxAudioPolicy deriveTxAudioPolicy(TciServerInfo const &serverInfo,
                                         TciCapabilities const &capabilities) const override;
};

QString tciSampleTypeName(quint32 sampleType);
QString tciStreamTypeName(quint32 streamType);
QString tciGenerationName(TciProtocolGeneration generation);
QString tciTxTimingName(TciTxAudioPolicy::Timing timing);
QString tciSampleCountMeaningName(TciTxAudioPolicy::SampleCountMeaning meaning);