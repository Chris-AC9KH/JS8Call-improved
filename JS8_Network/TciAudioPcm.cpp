#include "TciAudioPcm.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
quint32 readU32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}

qint16 readI16(const char *p)
{
    const quint16 v = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
    return static_cast<qint16>(v);
}

void appendU16(QByteArray &out, quint16 v)
{
    out.append(char(v & 0xff));
    out.append(char((v >> 8) & 0xff));
}

void appendU32(QByteArray &out, quint32 v)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.append(char((v >> shift) & 0xff));
}

double readNormalized(const char *p, quint32 type)
{
    switch (type) {
    case TCIStream::INT16:
        return double(readI16(p)) / 32768.0;
    case TCIStream::INT24: {
        const quint32 bits = quint32(quint8(p[0])) |
                             (quint32(quint8(p[1])) << 8) |
                             (quint32(quint8(p[2])) << 16);
        const qint32 signed24 = static_cast<qint32>(bits ^ 0x800000u) - 0x800000;
        return double(signed24) / 8388608.0;
    }
    case TCIStream::INT32: {
        const quint32 bits = readU32(p);
        const qint64 signed32 = bits < 0x80000000u ? qint64(bits) : qint64(bits) - 4294967296LL;
        return double(signed32) / 2147483648.0;
    }
    case TCIStream::FLOAT32: {
        const quint32 bits = readU32(p);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value) ? double(value) : 0.0;
    }
    default:
        return 0.0;
    }
}

qint16 toI16(double sample)
{
    // Match the full signed PCM range; silence NaNs/Infs and saturate inputs.
    if (!std::isfinite(sample))
        return 0;
    const double scaled = std::clamp(sample, -1.0, 1.0) * 32768.0;
    const auto v = static_cast<qint64>(std::llround(scaled));
    return static_cast<qint16>(std::clamp<qint64>(v, -32768, 32767));
}

void appendNormalized(QByteArray &out, qint16 input, quint32 type)
{
    const qint32 sample = input;
    switch (type) {
    case TCIStream::INT16:
        appendU16(out, static_cast<quint16>(input));
        return;
    case TCIStream::INT24: {
        const quint32 bits = static_cast<quint32>(sample * 256);
        for (unsigned shift = 0; shift != 24; shift += 8)
            out.append(char((bits >> shift) & 0xff));
        return;
    }
    case TCIStream::INT32:
        appendU32(out, static_cast<quint32>(qint64(sample) * 65536));
        return;
    case TCIStream::FLOAT32: {
        const float value = float(sample) / 32768.0f;
        quint32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(out, bits);
        return;
    }
    }
}
}

int TciAudioPcm::bytesPerSample(quint32 type)
{
    switch (type) {
    case TCIStream::INT16: return 2;
    case TCIStream::INT24: return 3;
    case TCIStream::INT32:
    case TCIStream::FLOAT32: return 4;
    default: return 0;
    }
}

QByteArray TciAudioPcm::decodeToMonoInt16(TCIStream::StreamFrame const &frame)
{
    const int bps = bytesPerSample(frame.sampleType);
    if (!bps || (frame.channels != 1 && frame.channels != 2) ||
        frame.sampleCount == 0 || frame.sampleCount % frame.channels != 0 ||
        quint64(frame.payload.size()) != quint64(frame.sampleCount) * quint64(bps))
        return {};

    const quint64 monoCount = frame.sampleCount / frame.channels;
    if (monoCount > quint64(std::numeric_limits<int>::max() / 2))
        return {};
    QByteArray result;
    result.reserve(int(monoCount * 2));
    const char *p = frame.payload.constData();
    for (quint64 i = 0; i < monoCount; ++i) {
        double sample = readNormalized(p, frame.sampleType);
        p += bps;
        if (frame.channels == 2) {
            sample = (sample + readNormalized(p, frame.sampleType)) * 0.5;
            p += bps;
        }
        appendU16(result, static_cast<quint16>(toI16(sample)));
    }
    return result;
}

QByteArray TciAudioPcm::encodeFromMonoInt16(QByteArray const &mono,
                                             quint32 type, quint32 channels,
                                             quint32 *sampleCount)
{
    if (sampleCount)
        *sampleCount = 0;
    const int bps = bytesPerSample(type);
    if (!bps || (channels != 1 && channels != 2) ||
        mono.isEmpty() || mono.size() % 2)
        return {};
    const quint64 count = quint64(mono.size() / 2) * channels;
    if (count > std::numeric_limits<quint32>::max() ||
        count * quint64(bps) > quint64(std::numeric_limits<int>::max()))
        return {};
    QByteArray out;
    out.reserve(int(count * bps));
    for (qsizetype i = 0; i < mono.size(); i += 2) {
        const qint16 sample = readI16(mono.constData() + i);
        for (quint32 channel = 0; channel < channels; ++channel)
            appendNormalized(out, sample, type);
    }
    if (sampleCount)
        *sampleCount = static_cast<quint32>(count);
    return out;
}
