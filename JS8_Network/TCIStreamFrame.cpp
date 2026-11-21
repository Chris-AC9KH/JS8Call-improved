#include "TCIStreamFrame.h"

#include <QDebug>

namespace
{
    constexpr int TCI_STREAM_HEADER_UINT32_COUNT = 16;
    constexpr int TCI_STREAM_HEADER_BYTES = TCI_STREAM_HEADER_UINT32_COUNT * 4;

    quint32 read_u32_le(QByteArray const &in, int offset)
    {
        auto b0 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset)));
        auto b1 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 1)));
        auto b2 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 2)));
        auto b3 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 3)));

        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    void append_u32_le(QByteArray &out, quint32 value)
    {
        out.append(static_cast<char>(value & 0xff));
        out.append(static_cast<char>((value >> 8) & 0xff));
        out.append(static_cast<char>((value >> 16) & 0xff));
        out.append(static_cast<char>((value >> 24) & 0xff));
    }

    int bytes_per_sample(quint32 sample_type)
    {
        switch (sample_type) {
        case TCIStream::INT16:
            return 2;
        case TCIStream::INT24:
            return 3;
        case TCIStream::INT32:
        case TCIStream::FLOAT32:
            return 4;
        default:
            return 0;
        }
    }
}

namespace TCIStream
{
    StreamFrame parseFrame(QByteArray const &frame)
    {
        StreamFrame result;

        if (frame.size() < TCI_STREAM_HEADER_BYTES) {
            return result;
        }

        result.receiver = read_u32_le(frame, 0);
        result.sampleRate = read_u32_le(frame, 4);
        result.sampleType = read_u32_le(frame, 8);
        result.codec = read_u32_le(frame, 12);
        result.crc = read_u32_le(frame, 16);
        result.sampleCount = read_u32_le(frame, 20);
        result.streamType = read_u32_le(frame, 24);
        result.channels = read_u32_le(frame, 28);
        result.payload = frame.mid(TCI_STREAM_HEADER_BYTES);

        int const bps = bytes_per_sample(result.sampleType);

        if (bps <= 0) {
            return result;
        }

        /*
         * TX_CHRONO is a timing/request frame. Some servers send it as a
         * 64-byte header-only frame. Do not require audio payload for it.
         */
        if (result.streamType == TX_CHRONO) {
            result.valid = true;
            return result;
        }

        /*
         * Other non-audio stream types may also be metadata/control-like for
         * our purposes. Parse the header and leave payload interpretation to
         * the caller.
         */
        if (result.streamType != RX_AUDIO_STREAM &&
            result.streamType != TX_AUDIO_STREAM &&
            result.streamType != IQ_STREAM &&
            result.streamType != LINEOUT_STREAM) {
            result.valid = true;
            return result;
            }

        if (result.channels == 0) {
            return result;
        }

        // TCI length counts scalar values across channels.
        if (result.sampleCount == 0 ||
            result.sampleCount % result.channels != 0 ||
            result.codec != 0)
            return result;

        const quint64 expected_min = quint64(result.sampleCount) * quint64(bps);
        if (expected_min != quint64(result.payload.size())) {
            qWarning() << "Short TCI stream payload:"
                       << "streamType=" << result.streamType
                       << "sampleRate=" << result.sampleRate
                       << "sampleType=" << result.sampleType
                       << "sampleCount=" << result.sampleCount
                       << "channels=" << result.channels
                       << "payloadBytes=" << result.payload.size()
                       << "expectedBytes=" << expected_min;
            return result;
        }

        result.valid = true;
        return result;
    }

    QByteArray makeTxAudioFrame(QByteArray const &pcm,
                                quint32 receiver,
                                quint32 sampleRate,
                                quint32 sampleType,
                                quint32 channels)
    {
        int const bps = bytes_per_sample(sampleType);

        quint32 const sample_count =
            bps > 0 && channels > 0 && pcm.size() % bps == 0
                ? static_cast<quint32>(pcm.size() / bps)
                : 0;

        QByteArray frame;
        frame.reserve(TCI_STREAM_HEADER_BYTES + pcm.size());

        append_u32_le(frame, receiver);
        append_u32_le(frame, sampleRate);
        append_u32_le(frame, sampleType);
        append_u32_le(frame, 0); // codec
        append_u32_le(frame, 0); // crc
        append_u32_le(frame, sample_count);
        append_u32_le(frame, TX_AUDIO_STREAM);
        append_u32_le(frame, channels);

        for (int i = 0; i < 8; ++i) {
            append_u32_le(frame, 0);
        }

        frame.append(pcm);
        return frame;
    }

QByteArray makeTxAudioFrameWithSampleCount(QByteArray const &pcm,
                            quint32 sampleCount,
                            quint32 receiver,
                            quint32 sampleRate,
                            quint32 sampleType,
                            quint32 channels)
    {
        QByteArray frame;
        frame.reserve(TCI_STREAM_HEADER_BYTES + pcm.size());

        append_u32_le(frame, receiver);
        append_u32_le(frame, sampleRate);
        append_u32_le(frame, sampleType);
        append_u32_le(frame, 0); // codec
        append_u32_le(frame, 0); // crc
        append_u32_le(frame, sampleCount);
        append_u32_le(frame, TX_AUDIO_STREAM);
        append_u32_le(frame, channels);

        for (int i = 0; i < 8; ++i) {
            append_u32_le(frame, 0);
        }

        frame.append(pcm);
        return frame;
    }
}