/**
 * @file ldpc_feedback.h
 * @brief LDPC erasure threshold configuration and feedback helpers.
 *
 * This header exposes configuration points (environment-variable overrides)
 * and a small helper that refines log-likelihood ratios (LLRs) using a
 * candidate decoded codeword. The refinement routine is intended to be run
 * between LDPC decoder passes: it boosts LLR magnitudes for bits that match
 * the decoded codeword confidently and shrinks or erases uncertain bits so
 * that subsequent LDPC passes can converge more easily.
 */

#pragma once

#include <QDebug>
#include <QLoggingCategory>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

Q_DECLARE_LOGGING_CATEGORY(decoder_js8);

namespace js8 {
/**
 * @brief LDPC erasure threshold config and feedback refinement helpers.
 *
 * Inline env readers expose thresholds/pass limits; the templated
 * refineLlrsWithLdpcFeedback shrinks/boosts LLRs using the decoded
 * codeword to retry LDPC. Used inside the JS8 decode loop between
 * LDPC passes.
 */
/** Default erasure threshold (zero retains all likelihood evidence). */
constexpr float LLR_ERASURE_THRESHOLD_DEFAULT = 0.0f;

/** Fixed LLR calibration shared across all frames (one means no adjustment). */
inline float llrGlobalScale() {
    if (auto const env = std::getenv("JS8_LLR_SCALE"); env) {
        char *end = nullptr;
        float const value = std::strtof(env, &end);
        if (end != env && *end == '\0' && std::isfinite(value) && value > 0.0f)
            return value;
    }
    return 2.0f;
}
/** Minimum magnitude to consider a bit "confident" when sign matches. */
constexpr float LLR_FEEDBACK_CONFIDENT_MIN = 3.0f;
/** Maximum magnitude considered "uncertain" (will be shrunk). */
constexpr float LLR_FEEDBACK_UNCERTAIN_MAX = 1.0f;
/** Multiply confident magnitudes by this factor when boosting. */
constexpr float LLR_FEEDBACK_CONFIDENT_BOOST = 1.2f;
/** Multiply uncertain magnitudes by this factor when shrinking. */
constexpr float LLR_FEEDBACK_UNCERTAIN_SHRINK = 0.5f;
/** Maximum allowed absolute magnitude for adjusted LLRs. */
constexpr float LLR_FEEDBACK_MAX_MAG = 6.0f;
/** Default maximum number of LDPC feedback passes when unset in env. */
constexpr int LDPC_FEEDBACK_MAX_PASSES_DEFAULT = 8;

/**
 * @brief Read configured LLR erasure threshold.
 *
 * Reads the `JS8_LLR_ERASURE_THRESH` environment variable (if present) and
 * returns a positive finite threshold. If the environment variable is
 * missing, invalid, or the `JS8_DISABLE_ERASURE_THRESHOLDING` variable is
 * set, this function returns `0.0f` to indicate erasure thresholding is
 * disabled.
 *
 * @return Positive erasure threshold, or `0.0f` if disabled/invalid.
 */
inline float llrErasureThreshold() {
    float threshold = LLR_ERASURE_THRESHOLD_DEFAULT;

    if (auto const env = std::getenv("JS8_LLR_ERASURE_THRESH"); env) {
        char *end = nullptr;
        float val = std::strtof(env, &end);

        if (end != env && std::isfinite(val)) {
            threshold = val;
        }
    }

    if (threshold <= 0.0f || !std::isfinite(threshold) ||
        std::getenv("JS8_DISABLE_ERASURE_THRESHOLDING")) {
        return 0.0f;
    }

    return threshold;
}

/**
 * @brief Whether LDPC feedback is enabled.
 *
 * The `JS8_LDPC_FEEDBACK` environment variable may be set to `0` to
 * explicitly disable feedback. If the variable is not present or cannot be
 * parsed as an integer, feedback is enabled by default.
 *
 * @return `true` when LDPC feedback is enabled, otherwise `false`.
 */
inline bool ldpcFeedbackEnabled() {
    bool ok = false;
    int value = qEnvironmentVariableIntValue("JS8_LDPC_FEEDBACK", &ok);
    return ok ? value != 0 : true;
}

/**
 * @brief Maximum LDPC feedback passes to attempt.
 *
 * Reads `JS8_LDPC_MAX_PASSES` and clamps the value to the range
 * `[1, LDPC_FEEDBACK_MAX_PASSES_DEFAULT]`. Returns a sensible default when
 * the variable is not provided or invalid.
 *
 * @return Number of feedback passes to attempt.
 */
inline int ldpcFeedbackMaxPasses() {
    bool ok = false;
    int value = qEnvironmentVariableIntValue("JS8_LDPC_MAX_PASSES", &ok);

    if (!ok)
        return LDPC_FEEDBACK_MAX_PASSES_DEFAULT;

    return std::clamp(value, 1, LDPC_FEEDBACK_MAX_PASSES_DEFAULT);
}

/**
 * @brief Refine LLRs using a decoded LDPC codeword as feedback.
 *
 * The algorithm adjusts each input LLR based on whether its sign matches the
 * corresponding bit in the provided codeword `cw` and on the magnitude of the
 * LLR:
 * - If the sign matches and the magnitude is >= `LLR_FEEDBACK_CONFIDENT_MIN`,
 *   the magnitude is boosted by `LLR_FEEDBACK_CONFIDENT_BOOST` (clamped to
 *   `LLR_FEEDBACK_MAX_MAG`) and the sign preserved.
 * - If the sign does not match, or the magnitude is <= `LLR_FEEDBACK_UNCERTAIN_MAX`,
 *   the magnitude is shrunk by `LLR_FEEDBACK_UNCERTAIN_SHRINK`. When an
 *   `erasureThreshold` > 0 is provided, shrunk magnitudes below that threshold
 *   are set to zero (erased).
 * - Non-finite input LLRs are replaced with zero and counted as uncertain.
 *
 * This function writes results to `llrOut` and returns counts of confident
 * and uncertain bits via `confidentCount` and `uncertainCount` respectively.
 *
 * @tparam N Number of LLR elements (array length).
 * @param llrIn Input LLR array (unchanged).
 * @param cw Decoded codeword as an array of int8_t (non-zero means bit 1).
 * @param erasureThreshold Threshold below which shrunk magnitudes are erased
 *        (set to zero). Use `0.0f` to disable erasure thresholding.
 * @param[out] llrOut Output array receiving adjusted LLRs.
 * @param[out] confidentCount Number of bits considered confident after adjust.
 * @param[out] uncertainCount Number of bits considered uncertain after adjust.
 */
template <std::size_t N>
void refineLlrsWithLdpcFeedback(std::array<float, N> const &llrIn,
                                std::array<int8_t, N> const &cw,
                                float erasureThreshold,
                                std::array<float, N> &llrOut,
                                int &confidentCount, int &uncertainCount) {
    llrOut = llrIn;
    confidentCount = 0;
    uncertainCount = 0;

    for (std::size_t i = 0; i < llrOut.size(); ++i) {
        float &value = llrOut[i];

        if (!std::isfinite(value)) {
            value = 0.0f;
            ++uncertainCount;
            continue;
        }

        bool const bitOne = cw[i] != 0;
        float const mag = std::abs(value);
        bool const signMatch = (value >= 0.0f) == bitOne;

        if (signMatch && mag >= LLR_FEEDBACK_CONFIDENT_MIN) {
            ++confidentCount;
            float boosted = mag * LLR_FEEDBACK_CONFIDENT_BOOST;
            boosted = std::clamp(boosted, 0.0f, LLR_FEEDBACK_MAX_MAG);
            value = bitOne ? boosted : -boosted;
        } else if (!signMatch || mag <= LLR_FEEDBACK_UNCERTAIN_MAX) {
            ++uncertainCount;
            float shrunk = mag * LLR_FEEDBACK_UNCERTAIN_SHRINK;

            if (erasureThreshold > 0.0f && shrunk < erasureThreshold) {
                value = 0.0f;
            } else {
                value = bitOne ? shrunk : -shrunk;
            }
        }
    }
}
} // namespace js8
