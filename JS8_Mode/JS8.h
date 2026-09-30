/**
 * @file JS8.h
 * @brief Core JS8 namespace types, Costas arrays and decoder event payloads.
 *
 * Defines the `JS8` namespace, Costas array helpers, event payload
 * structures used by the decoder, and the `Decoder` QObject wrapper type.
 */

#ifndef __JS8
#define __JS8

#include <QObject>
#include <QSemaphore>
#include <QThread>
#include "optional_work.h"
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <variant>

namespace JS8 {
Q_NAMESPACE

namespace Costas {
/**
 * @brief Costas array helpers used to map symbols to tones.
 *
 * JS8 historically re-used the Costas arrays from FT8 for the "normal"
 * mode; other JS8 modes use modified arrays. Callers request the
 * appropriate array via `Costas::array(type)`.
 */
enum class Type { ORIGINAL, MODIFIED };

/** 3x7 Costas array type. */
using Array = std::array<std::array<int, 7>, 3>;

/**
 * @brief Return the Costas array for the requested `Type`.
 * @param type Selector indicating ORIGINAL or MODIFIED Costas.
 * @return Const reference to the selected Costas `Array`.
 */
constexpr auto array = [] {
    constexpr auto COSTAS =
        std::array{std::array{std::array{4, 2, 5, 6, 1, 3, 0},
                              std::array{4, 2, 5, 6, 1, 3, 0},
                              std::array{4, 2, 5, 6, 1, 3, 0}},
                   std::array{std::array{0, 6, 2, 3, 5, 4, 1},
                              std::array{1, 5, 0, 2, 3, 6, 4},
                              std::array{2, 5, 0, 6, 4, 1, 3}}};

    return [COSTAS](Type type) -> Array const & {
        return COSTAS[static_cast<std::underlying_type_t<Type>>(type)];
    };
}();
} // namespace Costas

/**
 * @brief Encode a NUL-terminated message into tone indices using a Costas array.
 * @param type Mode selector (interpreted as `Costas::Type`).
 * @param costas Costas array to use for encoding.
 * @param message NUL-terminated input message string.
 * @param tones Output buffer to receive computed tone indices (caller-owned).
 */
void encode(int type, Costas::Array const &costas, const char *message,
            int *tones);

namespace Event {
/**
 * @brief Emitted when the decode process starts with the configured submodes.
 */
struct DecodeStarted {
    int submodes; ///< Number of submodes active for decoding
};

/** @brief Indicate start of a sync window. */
struct SyncStart {
    int position; ///< Byte/bit position of the sync candidate
    int size;     ///< Size of the sync window
};

/**
 * @brief Sync state update emitted during candidate evaluation.
 *
 * If `type` is `CANDIDATE` the `sync.candidate` field is valid; if
 * `DECODED` the `sync.decoded` float holds the decoded value.
 */
struct SyncState {
    enum class Type { CANDIDATE, DECODED } type;
    int mode;       ///< Submode index
    float frequency;///< Frequency estimate (Hz)
    float dt;       ///< Timing offset (s)
    union {
        int candidate; ///< Candidate index when Type::CANDIDATE
        float decoded; ///< Decoded metric when Type::DECODED
    } sync;
};

/**
 * @brief Decoded frame payload emitted when a frame is successfully decoded.
 *
 * @note `utc` is the raw time value produced by `code_time()` in `commons.h`.
 */
struct Decoded {
    int utc;           ///< Raw UTC timestamp from decoder
    int snr;           ///< Signal-to-noise ratio (dB)
    float xdt;         ///< Fine timing offset
    float frequency;   ///< Frequency estimate in Hz
    std::string data;  ///< Decoded payload string
    int type;          ///< Frame type identifier
    float quality;     ///< Quality metric
    int mode;          ///< Submode index used for decoding
};

/** Emitted when a batch of decoded frames has completed. */
struct DecodeFinished {
    std::size_t decoded; ///< Number of decoded frames
};

/** Variant encompassing all decoder event payloads. */
using Variant =
    std::variant<DecodeStarted, SyncStart, SyncState, Decoded, DecodeFinished>;

/** Function type used to receive decoder events. */
using Emitter = std::function<void(Variant const &)>;
} // namespace Event

/**
 * @brief Forward declaration of the decoder worker type.
 *
 * The concrete `Worker` implementation is defined elsewhere; the decoder
 * control object holds a pointer to a `Worker` instance running in the
 * decoder thread.
 */
class Worker;

/**
 * @class Decoder
 * @brief High-level QObject controller that owns the decoding worker thread.
 *
 * `Decoder` provides a Qt-friendly wrapper around the decoding worker.
 * It manages a worker thread and emits `decodeEvent` notifications using the
 * `JS8::Event::Variant` payload type.
 */
class Decoder : public QObject {
        Q_OBJECT

        QSemaphore m_semaphore; ///< Internal semaphore used by the worker
        QThread m_thread;       ///< Thread running the worker
        js8::OptionalWorkSignal m_optionalPressure;
        Worker *m_worker;       ///< Non-owning pointer to the worker instance

    public:
        /** Construct a decoder controller. */
        Decoder(QObject *parent = nullptr);

        /** Publish ready work without exposing the UI queue to the worker. */
        void pendingDecode(bool pending) noexcept;
        void nextDecodeReady(std::chrono::steady_clock::time_point when) noexcept;

    signals:

        /** Emitted for decoder lifecycle and result events. */
        void decodeEvent(Event::Variant const &);

    public slots:

        /** Start the decoder thread at the given priority. */
        void start(QThread::Priority priority);

        /** Request the decoder to quit. */
        void quit();

        /** Perform a single decode cycle (queued invocation). */
        void decode();
};
} // namespace JS8

#endif
