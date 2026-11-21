#pragma once

#include "TCIStreamFrame.h"

#include <QByteArray>
#include <QtGlobal>

namespace TciAudioPcm
{
// TCI Stream.length counts individual scalar samples across all channels.
// Converts the 48-kHz mono int16 samples used by JS8Call to/from TCI audio.
// Does not resample; the caller must verify the stream's sample rate.
int bytesPerSample(quint32 sampleType);

// Invalid type, malformed payload, unsupported channel count, or invalid
// framing returns an empty byte array. Stereo is interleaved L, R and
// downmixed by arithmetic mean before conversion to mono int16.
QByteArray decodeToMonoInt16(TCIStream::StreamFrame const &frame);

// Input is little-endian mono int16. Output interleaves and duplicates mono
// into both channels when channels == 2. sampleCount is the number of scalar
// values (mono frame count * channel count), not the number of time frames.
QByteArray encodeFromMonoInt16(QByteArray const &monoInt16,
                               quint32 sampleType,
                               quint32 channels,
                               quint32 *sampleCount = nullptr);
}
