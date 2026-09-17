/**
 * @file whitening_processor.h
 * @brief Noise whitening and LLR normalization helper used by the JS8 decoder.
 */

#pragma once

#include "coherent_likelihood.h"

#include <QDebug>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(decoder_js8);

namespace js8 {
/**
 * @brief Compute per-tone/symbol noise medians and whiten LLRs for a JS8 frame.
 *
 * Given symbol magnitudes (sans Costas) and winners, produces normalized
 * LLR0/LLR1, optionally applying noise-based whitening and erasure. Fully
 * templated on matrix dimensions, so it stays header-only; used inside the JS8
 * decoder per candidate.
 */
template <int NROWS, int ND, int N> class WhiteningProcessor {
  public:
    /**
     * @brief Result of a whitening/LRR normalization pass.
     *
     * `llr0` and `llr1` are populated in column order (three outputs per
     * symbol) to match the decoder's expectations. The boolean flags indicate
     * whether whitening and/or erasure were applied; `erasures` counts the
     * number of individual LLR elements that were set to zero. The `avgAbs*`
     * fields contain sum-like metrics collected during processing to aid
     * debugging and tuning.
     */
    struct Result {
        std::array<float, 3 * ND> llr0;    ///< LLR values for the 0-hypothesis
        std::array<float, 3 * ND> llr1;    ///< LLR values for the 1-hypothesis
        bool whiteningApplied;             ///< True when whitening was applied
        bool erasureApplied;               ///< True when erasure was applied
        std::size_t erasures;              ///< Number of LLR elements erased
        double avgAbsPre;                   ///< Aggregate |LLR| before whitening
        double avgAbsPost;                  ///< Aggregate |LLR| after whitening
    };

    /**
     * @brief Compute normalized LLR arrays for a single candidate frame.
     *
     * The template parameters describe the matrix dimensions used by the
     * decoder: `NROWS` is the number of tones (rows), `ND` is the number of
     * symbols (columns) and `N` is a helper parameter used by the decoder
     * (kept for API parity). The input `s1` is an array of `NROWS` rows,
     * each containing `ND` magnitudes (per-symbol). For each symbol column
     * the routine computes three LLR entries (placed into contiguous slots
     * of `llr0`/`llr1`) and optionally applies noise whitening and erasure.
     *
     * @param s1 Per-tone arrays of symbol magnitudes; index as `s1[row][col]`.
     * @param symbolWinners For each symbol column, the index [0..NROWS-1]
     *        identifying the winning tone.
     * @param erasureThreshold When > 0.0, magnitudes below this threshold
     *        (after whitening) are erased (set to zero).
     * @param debug When true, emits extra debug logging about noise metrics.
     * @param coherentBlend Optional physical coherent numerators in
     *        `s1` orientation (`[tone][symbol]`), holding
     *        `A*projection - 0.5*A*A` per tone. The decoder scales them by its
     *        own per-symbol `invSigma2` exactly like the noncoherent power
     *        numerators, so no symbol is ever renormalized. When absent (or
     *        when its alpha/amplitude is not usable) the legacy noncoherent
     *        scores are used exactly.
     * @return A `Result` containing `llr0`, `llr1` and processing statistics.
     */
    static Result process(std::array<std::array<float, ND>, NROWS> const &s1,
                           std::array<int, ND> const &symbolWinners,
                           float erasureThreshold, bool debug,
                           std::optional<CoherentBlend<NROWS, ND>> const
                               &coherentBlend = std::nullopt) {
        auto const median =
            [](std::vector<float> &values) -> std::optional<float> {
            if (values.empty())
                return std::nullopt;

            auto const mid = values.size() / 2;

            std::nth_element(values.begin(), values.begin() + mid,
                             values.end());
            float med = values[mid];

            if ((values.size() % 2) == 0 && mid > 0) {
                std::nth_element(values.begin(), values.begin() + (mid - 1),
                                 values.end());
                med = 0.5f * (med + values[mid - 1]);
            }

            return med;
        };

        // Estimate per-tone noise using non-winning tone magnitudes across the
        // frame.
        auto const toneNoise =
            [&]() -> std::optional<std::array<float, NROWS>> {
            std::array<std::vector<float>, NROWS> toneSamples;
            std::array<float, NROWS> noise = {};

            // Collect non-winning magnitudes for each tone.
            for (int j = 0; j < ND; ++j) {
                int const winner = symbolWinners[j];

                for (int i = 0; i < NROWS; ++i) {
                    if (i != winner)
                        toneSamples[i].push_back(s1[i][j]);
                }
            }

            bool ok = true;

            for (int i = 0; i < NROWS; ++i) {
                if (auto m = median(toneSamples[i]); m) {
                    noise[i] = *m;
                } else {
                    ok = false;
                    break;
                }
            }

            if (!ok)
                return std::nullopt;

            return noise;
        }();

        if (toneNoise && debug) {
            std::ostringstream oss;

            oss << "toneNoise:";
            for (auto const value : *toneNoise)
                oss << ' ' << value;

            qCDebug(decoder_js8).noquote() << oss.str().c_str();
        }

        // Estimate per-symbol noise using non-winning tone magnitudes per
        // symbol.
        auto const symbolNoise = [&]() -> std::optional<std::vector<float>> {
            std::vector<float> noise;
            noise.reserve(ND);

            for (int j = 0; j < ND; ++j) {
                std::vector<float> bins;
                bins.reserve(NROWS - 1);

                int const winner = symbolWinners[j];

                for (int i = 0; i < NROWS; ++i) {
                    if (i != winner)
                        bins.push_back(s1[i][j]);
                }

                if (auto m = median(bins); m) {
                    noise.push_back(*m);
                } else {
                    return std::nullopt;
                }
            }

            return noise;
        }();

        if (symbolNoise && !symbolNoise->empty() && debug) {
            auto const [minIt, maxIt] =
                std::minmax_element(symbolNoise->begin(), symbolNoise->end());
            float const avg = std::accumulate(symbolNoise->begin(),
                                              symbolNoise->end(), 0.0f) /
                              static_cast<float>(symbolNoise->size());

            qCDebug(decoder_js8)
                << "symbolNoise avg/min/max" << avg << *minIt << *maxIt;
        }

        Result result{};

        bool const disableWhitening =
            std::getenv("JS8_DISABLE_WHITENING") != nullptr;
        // Coherent data is used only when the whole blended input validates:
        // any non-finite numerator, amplitude, or weight falls back to the
        // legacy path bit-identically (per-symbol partial blends could still
        // shift the shared downstream LLR normalization).
        bool coherentUsable = false;
        float coherentAlpha = 0.0f;
        if (coherentBlend && coherentBlend->alpha > 0.0f &&
            std::isfinite(coherentBlend->alpha) &&
            std::isfinite(coherentBlend->amplitude) &&
            coherentBlend->amplitude > 0.0f) {
            coherentUsable = true;
            for (auto const &row : coherentBlend->numerators) {
                for (float const value : row) {
                    if (!std::isfinite(value)) {
                        coherentUsable = false;
                        break;
                    }
                }
                if (!coherentUsable)
                    break;
            }
            if (coherentUsable)
                coherentAlpha =
                    std::clamp(coherentBlend->alpha, 0.0f, 1.0f);
        }
        bool const whiteningAvailable = toneNoise && symbolNoise &&
                                        !symbolNoise->empty() &&
                                        !disableWhitening;
        bool const applyErasureInWhitening =
            whiteningAvailable && erasureThreshold > 0.0f;
        double sumAbsPre = 0.0;
        double sumAbsPost = 0.0;
        std::size_t erasures = 0;

        for (int j = 0; j < ND; ++j) {
            int const i1 = 3 * j;     // First column (matches Fortran's i1)
            int const i2 = 3 * j + 1; // Second column (matches Fortran's i2)
            int const i4 = 3 * j + 2; // Third column (matches Fortran's i4)

            std::array<float, NROWS> ps;

            for (int i = 0; i < NROWS; ++i)
                ps[i] = s1[i][j];

            // Common per-symbol noise-power estimate. When whitening is
            // unavailable, fall back to unit variance so the calculation stays
            // a Gaussian power likelihood rather than degenerating.
            float const invSigma2 = whiteningAvailable ? 1.0f /
                                       ((std::max(0.0f, (*toneNoise)[symbolWinners[j]]) *
                                         std::max(0.0f, (*symbolNoise)[j])) +
                                        1e-12f)
                                                     : 1.0f;

            // Noise-normalized tone power. Under an AWGN model with per-symbol
            // noise power sigma^2 = toneNoise * symbolNoise, ps^2/(2*sigma^2)
            // is used as the Gaussian soft-likelihood metric for tone i.
            std::array<float, NROWS> w;

            for (int i = 0; i < NROWS; ++i) {
                float const power = ps[i] * ps[i];
                w[i] = 0.5f * power * invSigma2;
            }

            // Optionally blend conservatively estimated coherent numerators.
            // Both families share this symbol's invSigma2, so reliability
            // information in their magnitudes is preserved; no symbol is ever
            // renormalized. With no usable blend input, `w` above is used
            // untouched, preserving the legacy path exactly.
            if (coherentUsable) {
                std::array<float, NROWS> coherentScores;
                for (int i = 0; i < NROWS; ++i)
                    coherentScores[i] =
                        (*coherentBlend).numerators[i][j] * invSigma2;
                std::array<float, NROWS> blended{};
                if (js8::blendToneScores(w, coherentScores, coherentAlpha,
                                         blended))
                    w = blended;
            }

            // Stable log-sum-exp of the tones whose natural-binary encoding has
            // bit `bit` set (given by `shift`) or clear, for a single bit group.
            auto logSumExp = [&](int shift, int bit) -> float {
                float m = -std::numeric_limits<float>::infinity();

                for (int i = 0; i < NROWS; ++i)
                    if (((i >> shift) & 1) == bit && w[i] > m)
                        m = w[i];

                if (std::isinf(m))
                    return 0.0f; // empty group: contributes 0 to the diff

                float s = 0.0f;

                for (int i = 0; i < NROWS; ++i)
                    if (((i >> shift) & 1) == bit)
                        s += std::exp(w[i] - m);

                return m + std::log(s);
            };

            // Each symbol emits three LLRs (one per bit) as the logsum-exp
            // difference across the two bit groups, combining all tones in each
            // group rather than using only the single largest magnitude.
            result.llr0[i1] = logSumExp(2, 1) - logSumExp(2, 0);
            result.llr0[i2] = logSumExp(1, 1) - logSumExp(1, 0);
            result.llr0[i4] = logSumExp(0, 1) - logSumExp(0, 0);

            // llr0 and llr1 are unified: they carry the same, properly
            // soft-calculated symbol information (pass diversity comes from the
            // bit-range masking and LDPC feedback downstream).
            result.llr1[i1] = result.llr0[i1];
            result.llr1[i2] = result.llr0[i2];
            result.llr1[i4] = result.llr0[i4];

            if (whiteningAvailable) {
                // The LLRs are already noise-normalized through invSigma2, so
                // no further division is required here; only erasure and the
                // pre/post magnitude metrics are collected.
                auto const applyWhitening = [&](float &value) {
                    float const pre = std::abs(value);
                    sumAbsPre += pre;

                    if (applyErasureInWhitening &&
                        std::abs(value) < erasureThreshold) {
                        value = 0.0f;
                        ++erasures;
                    }

                    sumAbsPost += std::abs(value);
                };

                applyWhitening(result.llr0[i1]);
                applyWhitening(result.llr0[i2]);
                applyWhitening(result.llr0[i4]);
                applyWhitening(result.llr1[i1]);
                applyWhitening(result.llr1[i2]);
                applyWhitening(result.llr1[i4]);
            }
        }

        auto const normalizeLLR = [](auto &llr) {
            float sum = 0.0f;
            float sum_of_squares = 0.0f;

            for (auto const value : llr) {
                sum += value;
                sum_of_squares += value * value;
            }

            float const llrav = sum / llr.size();
            float const llr2av = sum_of_squares / llr.size();
            float const variance = llr2av - llrav * llrav;
            float const llrsig = std::sqrt(variance > 0.0f ? variance : llr2av);

            for (float &val : llr)
                val = (val / llrsig) * 2.83f;
        };

        // Normalize and process metrics

        normalizeLLR(result.llr0);
        normalizeLLR(result.llr1);

        if (whiteningAvailable && debug) {
            auto const total =
                static_cast<double>(result.llr0.size() + result.llr1.size());
            double const avgPre = total > 0.0 ? sumAbsPre / total : 0.0;
            double const avgPost = total > 0.0 ? sumAbsPost / total : 0.0;

            qCDebug(decoder_js8) << "LLR whitening applied"
                                 << "avg|LLR| pre/post:" << avgPre << avgPost
                                 << "erasures:" << erasures;
        }

        result.whiteningApplied = whiteningAvailable;
        result.erasureApplied = applyErasureInWhitening;
        result.erasures = erasures;
        result.avgAbsPre = sumAbsPre;
        result.avgAbsPost = sumAbsPost;
        return result;
    }
};
} // namespace js8
