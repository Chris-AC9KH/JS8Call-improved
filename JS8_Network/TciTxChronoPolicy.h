#pragma once

#include "TciProtocolHandler.h"

// Validate a TX_CHRONO request before consuming source samples or changing
// the client's audio pacing mode. Zero means unusable/unsupported.
namespace TciTxChronoPolicy
{
inline bool canAccept(bool stopping, bool started, bool suspended,
                      bool txAudioStarted, bool connected, bool ready,
                      bool sourceReadable)
{
    return !stopping && started && !suspended && txAudioStarted &&
           connected && ready && sourceReadable;
}

inline int monoSamplesRequested(const TciTxAudioPolicy &policy)
{
    if (policy.timing != TciTxAudioPolicy::Timing::ChronoDriven ||
        policy.sampleRate != 48000 ||
        (policy.channels != 1 && policy.channels != 2) ||
        policy.samplesPerFrame <= 0 ||
        policy.samplesPerFrame > 96000)
        return 0;

    switch (policy.sampleType) {
    case TCIStream::INT16:
    case TCIStream::INT24:
    case TCIStream::INT32:
    case TCIStream::FLOAT32:
        break;
    default:
        return 0;
    }

    switch (policy.sampleCountMeaning) {
    case TciTxAudioPolicy::SampleCountMeaning::TotalSampleValues:
        if (policy.samplesPerFrame % policy.channels != 0)
            return 0;
        return policy.samplesPerFrame / policy.channels;
    case TciTxAudioPolicy::SampleCountMeaning::FramesPerChannel:
        // Retained only for legacy servers that explicitly use this policy.
        return policy.samplesPerFrame;
    default:
        return 0;
    }
}
}
