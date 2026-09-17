/**
 * @file decoder_aided_redemod.h
 * @brief Decoder-aided re-demodulation helpers for JS8 near-miss rescue.
 *
 * When normal decoding fails but LDPC gets CLOSE (small syndrome), the best
 * tentative 174-bit codeword is used as a waveform hypothesis: its 58 data
 * tones plus the 21 known Costas tones refine timing/residual-frequency/drift
 * against the candidate's actual received samples. The refined sync then
 * drives ONE fresh 8-tone re-demodulation producing new LLRs for a final
 * LDPC attempt. CRC remains the only acceptance gate.
 *
 * This header holds the pure, deterministic algorithmic core (no Qt, no FFTW,
 * no decoder state) so it can be unit-tested standalone:
 * - tentative codeword -> 79 expected tones (exact decoder bit/tone mapping)
 * - per-data-symbol confidence weights from LLR magnitudes
 * - bounded local sync refinement with a tone-CONTRAST metric
 * - baseline comparison / safety gate
 *
 * Bit/tone mapping authority (derived from the decoder, not memory):
 * - Decoder LLR formation (whitening_processor.h): data symbol j emits
 *   llr[3j] (tone bit 2, value 4), llr[3j+1] (tone bit 1, value 2),
 *   llr[3j+2] (tone bit 0, value 1); llr > 0 means bit 1.
 * - bpdecode174 (JS8.cpp): hard slice `cw[i] = zn[i] > 0 ? 1 : 0`, positions
 *   preserved, so tone(j) = 4*cw[3j] + 2*cw[3j+1] + cw[3j+2].
 * - Encoder (JS8::encode, JS8.cpp): parity words -> tones+7 (data symbols
 *   j=0..28, globals 7..35), message words -> tones+43 (j=29..57, globals
 *   43..71); MSB-first accumulation matches the tone formula above.
 * - Symbol layout (JS8.cpp demod loop): Costas globals 0-6, 36-42, 72-78;
 *   data globals 7-35 (j=0..28) and 43-71 (j=29..57).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>

namespace js8 {
namespace aided {

/** Coded bits per frame (matches decoder N). */
inline constexpr int kCodeBits = 174;
/** Data symbols per frame (matches decoder ND). */
inline constexpr int kDataSymbols = 58;
/** Total channel symbols per frame (matches decoder NN). */
inline constexpr int kTotalSymbols = 79;
/** FSK tones (matches decoder NROWS). */
inline constexpr int kTones = 8;

/** Near-miss trigger: best syndrome must be in (0, kMaxSyndrome]. */
inline constexpr int kMaxSyndrome = 4;

/** Timing search offsets (downsampled samples) around the first-pass start. */
inline constexpr int kTimingDeltaMin = -2;
inline constexpr int kTimingDeltaMax = 2;

/** Residual-frequency search: start/step/count covering about +/-0.5 Hz. */
inline constexpr double kFreqStartHz = -0.5;
inline constexpr double kFreqStepHz = 0.1;
inline constexpr int kFreqSteps = 11;

/** Linear-drift search values (Hz/s). */
inline constexpr double kDriftValues[] = {-0.02, 0.0, 0.02};
inline constexpr int kDriftSteps =
    static_cast<int>(sizeof(kDriftValues) / sizeof(kDriftValues[0]));

/**
 * Minimum refined-vs-baseline metric gain required to re-demodulate
 * (0.5%: small but real; rejects floating-point dust).
 */
inline constexpr double kMinMetricGain = 1.005;

/** Symbols whose weakest bit |LLR| is below this get zero weight. */
inline constexpr double kConfidenceFloor = 0.5;
/** min|LLR| is scaled by this to reach full weight 1.0 (clamped). */
inline constexpr double kConfidenceScale = 3.0;

/** 3x7 Costas tone layout, matching JS8::Costas::Array. */
using CostasTones = std::array<std::array<int, 7>, 3>;

/**
 * @brief Kill switch for decoder-aided re-demodulation.
 *
 * When the `JS8_DISABLE_REDEMOD` environment variable is present (any value),
 * the aided pass is completely disabled and decoder behavior matches the
 * implementation without this feature.
 */
inline bool redemodEnabled() {
    return std::getenv("JS8_DISABLE_REDEMOD") == nullptr;
}

/**
 * @brief Whether global symbol k is a known Costas pilot.
 *
 * Layout mirrors the decoder demod loop: pilots at globals 0-6, 36-42,
 * 72-78; data at 7-35 and 43-71.
 */
constexpr bool isCostasSymbol(int k) {
    return (k >= 0 && k < 7) || (k >= 36 && k < 43) || (k >= 72 && k < 79);
}

/** Global channel-symbol index for data symbol j (j=0..57). */
constexpr int dataSymbolIndex(int j) {
    return j < 29 ? j + 7 : j + 14;
}

/**
 * @brief Convert a tentative 174-bit codeword into 79 expected tones.
 *
 * Costas positions come from the exact known array; data tones use the exact
 * decoder mapping tone(j) = 4*b0 + 2*b1 + b2 with bits cw[3j..3j+2]
 * (non-zero int8 means bit 1, matching the bpdecode174 hard slice).
 */
inline void codewordToTones(std::array<int8_t, kCodeBits> const &cw,
                            CostasTones const &costas,
                            std::array<int, kTotalSymbols> &tones) {
    for (int block = 0; block < 3; ++block)
        for (int column = 0; column < 7; ++column)
            tones[static_cast<std::size_t>(block * 36 + column)] =
                costas[static_cast<std::size_t>(block)]
                      [static_cast<std::size_t>(column)];
    for (int j = 0; j < kDataSymbols; ++j) {
        int const b0 = cw[static_cast<std::size_t>(3 * j)] != 0 ? 1 : 0;
        int const b1 = cw[static_cast<std::size_t>(3 * j + 1)] != 0 ? 1 : 0;
        int const b2 = cw[static_cast<std::size_t>(3 * j + 2)] != 0 ? 1 : 0;
        tones[static_cast<std::size_t>(dataSymbolIndex(j))] =
            (b0 << 2) | (b1 << 1) | b2;
    }
}

/** Per-data-symbol confidence summary. */
struct ConfidenceSummary {
    std::array<float, kDataSymbols> weights{}; ///< Per-symbol weight in [0,1].
    int used = 0;            ///< Symbols with weight > 0.
    double mean = 0.0;       ///< Mean weight over USED symbols (0 if none).
    double minAbsMin = 0.0;  ///< Smallest min|LLR| observed (diagnostic).
};

/**
 * @brief Confidence weight per tentative data symbol from its 3 bit LLRs.
 *
 * weight = clamp((minAbs - floor) / (scale - floor), 0, 1), i.e. zero below
 * kConfidenceFloor, ramping to 1.0 at min|LLR| = kConfidenceScale. Non-finite
 * LLRs force zero weight so NaN/Inf can never steer the fit. One huge LLR
 * cannot dominate: each symbol contributes at most 1.0.
 */
inline ConfidenceSummary
tentativeConfidences(std::array<float, kCodeBits> const &llr) {
    ConfidenceSummary out{};
    double sum = 0.0;
    bool haveMin = false;
    for (int j = 0; j < kDataSymbols; ++j) {
        float const v0 = llr[static_cast<std::size_t>(3 * j)];
        float const v1 = llr[static_cast<std::size_t>(3 * j + 1)];
        float const v2 = llr[static_cast<std::size_t>(3 * j + 2)];
        double weight = 0.0;
        if (std::isfinite(v0) && std::isfinite(v1) && std::isfinite(v2)) {
            double const minAbs = std::min(
                {std::abs(static_cast<double>(v0)),
                 std::abs(static_cast<double>(v1)),
                 std::abs(static_cast<double>(v2))});
            if (!haveMin || minAbs < out.minAbsMin) {
                out.minAbsMin = minAbs;
                haveMin = true;
            }
            if (minAbs >= kConfidenceFloor) {
                weight = (minAbs - kConfidenceFloor) /
                         (kConfidenceScale - kConfidenceFloor);
                weight = std::clamp(weight, 0.0, 1.0);
            }
        }
        out.weights[static_cast<std::size_t>(j)] =
            static_cast<float>(weight);
        if (weight > 0.0) {
            ++out.used;
            sum += weight;
        }
    }
    out.mean = out.used > 0 ? sum / out.used : 0.0;
    return out;
}

/**
 * @brief Expand 58 data confidences to 79 per-symbol weights.
 *
 * Known Costas symbols get weight 1.0; data symbols get their confidence.
 */
inline void
buildSymbolWeights(ConfidenceSummary const &conf,
                    std::array<float, kTotalSymbols> &weights) {
    for (int k = 0; k < kTotalSymbols; ++k) {
        if (isCostasSymbol(k)) {
            weights[static_cast<std::size_t>(k)] = 1.0f;
            continue;
        }
        // Data globals 7..35 -> j=k-7; globals 43..71 -> j=k-14.
        int const j = k < 36 ? k - 7 : k - 14;
        weights[static_cast<std::size_t>(k)] =
            (j >= 0 && j < kDataSymbols)
                ? conf.weights[static_cast<std::size_t>(j)]
                : 0.0f;
    }
}

/**
 * @brief First-pass per-symbol extraction metadata for the refinement baseline.
 *
 * `startSamples` is the ACTUAL first-pass window start (nominal base plus the
 * recorded TimingTracker integer shift, exactly as extracted), and
 * `trackerHz` is the recorded FrequencyTracker estimate applied to that
 * symbol. The refinement baseline replays this tracked extraction, so the
 * gate compares refined hypotheses against the real first-pass
 * synchronization rather than an untracked approximation.
 */
struct SymbolBaseline {
    int startSamples = 0;  ///< First-pass window start (base + shift).
    float trackerHz = 0.0f; ///< Recorded tracker estimate for the symbol.
};

/**
 * @brief Validate candidate sample bounds for the aided search/extraction.
 *
 * Every baseline start plus/minus the maximum hypothesis delta must lie in
 * [0, numSamples - window] WITHOUT clamping, so no hypothesis reads out of
 * bounds and no symbol is degenerately clamped onto its neighbor.
 */
inline bool
symbolBoundsValid(std::array<SymbolBaseline, kTotalSymbols> const &baselines,
                  int window, int numSamples, int maxAbsDelta) {
    if (window <= 0 || numSamples <= 0 || maxAbsDelta < 0)
        return false;
    for (auto const &base : baselines) {
        long const first = static_cast<long>(base.startSamples) - maxAbsDelta;
        long const last =
            static_cast<long>(base.startSamples) + maxAbsDelta;
        if (first < 0 || last < 0)
            return false;
        if (last + window > static_cast<long>(numSamples))
            return false;
    }
    return true;
}

/**
 * @brief Replay one symbol's recorded FrequencyTracker correction.
 *
 * This mirrors FrequencyTracker::apply() operation-for-operation (positive
 * rotation `wstep^(n+1)` with `wstep = exp(j*2*pi*trackerHz/sampleRateHz)`,
 * fresh phase every window, float arithmetic) so the aided baseline
 * reproduces the first-pass per-symbol extraction bit-faithfully for finite
 * inputs. The added finiteness guard only rejects corrupt input the decoder
 * would discard downstream anyway.
 *
 * @return False on any invalid/non-finite input (caller must skip the
 *         symbol); true with `window` rotated in place otherwise.
 */
inline bool replayTrackerCorrection(std::complex<float> *window, int count,
                                    double trackerHz, double sampleRateHz) {
    if (window == nullptr || count <= 0 || !std::isfinite(trackerHz) ||
        !std::isfinite(sampleRateHz) || !(sampleRateHz > 0.0))
        return false;
    double const dphi = 2.0 * std::numbers::pi * (trackerHz / sampleRateHz);
    auto const wstep = std::polar(1.0f, static_cast<float>(dphi));
    auto w = std::complex<float>{1.0f, 0.0f};
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(window[i].real()) ||
            !std::isfinite(window[i].imag()))
            return false;
        w *= wstep;
        window[i] *= w;
    }
    return true;
}

/**
 * @brief Tone-CONTRAST metric from the actual 8 JS8 FSK bins.
 *
 * contrast = P[expected] - mean(P[other 7 valid tones]). Broadband energy
 * raises all eight bins together and scores ~0, so the metric answers "is
 * the tentative waveform present?" rather than "is there more RF energy?".
 * Unused FFT bins outside the 8 valid tones never enter the estimate.
 * Returns NaN on invalid input.
 */
inline double contrastEight(double expectedPower,
                            double const *otherPowers) {
    if (otherPowers == nullptr || !std::isfinite(expectedPower) ||
        expectedPower < 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    for (int t = 0; t < kTones - 1; ++t) {
        if (!std::isfinite(otherPowers[t]) || otherPowers[t] < 0.0)
            return std::numeric_limits<double>::quiet_NaN();
        sum += otherPowers[t];
    }
    return expectedPower - sum / (kTones - 1);
}

/** Best sync hypothesis from the bounded search. */
struct Hypothesis {
    int deltaSamples = 0;      ///< Timing offset (downsampled samples).
    double deltaHz = 0.0;      ///< Residual frequency correction (Hz).
    double driftHzPerSec = 0.0; ///< Linear drift correction (Hz/s).
    double metric = std::numeric_limits<double>::quiet_NaN();
};

/** Outcome of the bounded refinement search. */
struct Refinement {
    Hypothesis best; ///< Best hypothesis (baseline unless beaten strictly).
    double baselineMetric = std::numeric_limits<double>::quiet_NaN();
    bool baselineFinite = false;
    bool searched = false;   ///< False when inputs were rejected up front.
    bool atBoundary = false; ///< Best sits on the search box boundary.
};

/**
 * @brief Bounded local sync refinement around the first-pass solution.
 *
 * The baseline (delta 0,0,0) replays the ACTUAL first-pass per-symbol
 * extraction: recorded window starts plus the recorded FrequencyTracker
 * correction for each symbol. Search hypotheses are refinements RELATIVE to
 * that baseline (trial start = recorded start + deltaSamples, recorded
 * tracker rotation followed by the candidate residual freq/drift
 * derotation), so the gate can never claim a fake gain by rediscovering a
 * correction the first pass already applied.
 *
 * Each hypothesis scores every useful symbol with the tone contrast of the
 * actual 8 JS8 FSK bins: the window is derotated once, all 8 bins are
 * computed together with a tiny direct DFT (no FFTW plan per hypothesis),
 * and contrast = P[expected] - mean(P[other 7]). Unused FFT bins never
 * enter the estimate. Ties keep the earliest (baseline wins all ties: grid
 * entries must STRICTLY beat it). Any invalid input yields searched=false
 * rather than partial garbage.
 */
inline Refinement refineSync(std::complex<float> const *samples,
                             int numSamples,
                             std::array<SymbolBaseline, kTotalSymbols> const
                                 &baselines,
                             int window, double sampleRateHz,
                             std::array<int, kTotalSymbols> const &tones,
                             std::array<float, kTotalSymbols> const &weights) {
    Refinement out{};
    if (samples == nullptr || numSamples <= 0 || window <= 0 || window > 32 ||
        !std::isfinite(sampleRateHz) || !(sampleRateHz > 0.0))
        return out;
    if (!symbolBoundsValid(baselines, window, numSamples, kTimingDeltaMax))
        return out;
    for (int k = 0; k < kTotalSymbols; ++k) {
        if (tones[static_cast<std::size_t>(k)] < 0 ||
            tones[static_cast<std::size_t>(k)] >= kTones)
            return out;
        float const w = weights[static_cast<std::size_t>(k)];
        if (!std::isfinite(w) || w < 0.0f)
            return out;
        // Used symbols need usable baseline metadata; unused ones are never
        // touched, so their metadata may be anything.
        if (w > 0.0f &&
            !std::isfinite(
                static_cast<double>(baselines[static_cast<std::size_t>(k)]
                                        .trackerHz)))
            return out;
    }

    // Forward-DFT twiddles for the 8 valid tones, computed once per call so
    // the inner loop performs no per-tone trigonometry.
    constexpr double twoPi = 2.0 * std::numbers::pi;
    double twRe[kTones][32];
    double twIm[kTones][32];
    for (int tone = 0; tone < kTones; ++tone) {
        for (int n = 0; n < window; ++n) {
            double const angle =
                -twoPi * static_cast<double>(tone * n) / window;
            twRe[tone][n] = std::cos(angle);
            twIm[tone][n] = std::sin(angle);
        }
    }

    // Score one (timing, freq, drift) hypothesis with the weighted 8-tone
    // contrast. The recorded tracker rotation is replayed first (baseline),
    // then the candidate refinement derotation (absolute-time, matching the
    // decoder's coarse correction sign convention).
    auto const scoreHypothesis = [&](int dt, double df, double dd) {
        double metric = 0.0;
        std::complex<float> replayed[32];
        for (int k = 0; k < kTotalSymbols; ++k) {
            float const w = weights[static_cast<std::size_t>(k)];
            if (!(w > 0.0f))
                continue;
            int const start =
                baselines[static_cast<std::size_t>(k)].startSamples + dt;
            for (int n = 0; n < window; ++n)
                replayed[n] = samples[start + n];
            if (!replayTrackerCorrection(
                    replayed, window,
                    baselines[static_cast<std::size_t>(k)].trackerHz,
                    sampleRateHz))
                continue;
            double binRe[kTones] = {};
            double binIm[kTones] = {};
            for (int n = 0; n < window; ++n) {
                double const t =
                    static_cast<double>(start + n) / sampleRateHz;
                double const angle =
                    twoPi * df * t + std::numbers::pi * dd * t * t;
                double const c = std::cos(angle);
                double const s = std::sin(angle);
                double const yr = static_cast<double>(replayed[n].real()) * c +
                                  static_cast<double>(replayed[n].imag()) * s;
                double const yi = static_cast<double>(replayed[n].imag()) * c -
                                  static_cast<double>(replayed[n].real()) * s;
                for (int tone = 0; tone < kTones; ++tone) {
                    binRe[tone] += yr * twRe[tone][n] - yi * twIm[tone][n];
                    binIm[tone] += yi * twRe[tone][n] + yr * twIm[tone][n];
                }
            }
            int const expected = tones[static_cast<std::size_t>(k)];
            double const expPower =
                binRe[expected] * binRe[expected] +
                binIm[expected] * binIm[expected];
            double others[kTones - 1];
            int oi = 0;
            for (int tone = 0; tone < kTones; ++tone) {
                if (tone == expected)
                    continue;
                others[oi++] =
                    binRe[tone] * binRe[tone] + binIm[tone] * binIm[tone];
            }
            double const cont = contrastEight(expPower, others);
            if (!std::isfinite(cont))
                continue;
            metric += static_cast<double>(w) * cont;
        }
        return metric;
    };

    out.baselineMetric = scoreHypothesis(0, 0.0, 0.0);
    out.baselineFinite = std::isfinite(out.baselineMetric);
    if (!out.baselineFinite)
        return out;
    out.searched = true;
    out.best.metric = out.baselineMetric;

    for (int dd = 0; dd < kDriftSteps; ++dd) {
        for (int f = 0; f < kFreqSteps; ++f) {
            double const df = kFreqStartHz + f * kFreqStepHz;
            for (int d = kTimingDeltaMin; d <= kTimingDeltaMax; ++d) {
                if (d == 0 && df == 0.0 && kDriftValues[dd] == 0.0)
                    continue; // Baseline already scored.
                double const metric =
                    scoreHypothesis(d, df, kDriftValues[dd]);
                if (std::isfinite(metric) && metric > out.best.metric) {
                    out.best.deltaSamples = d;
                    out.best.deltaHz = df;
                    out.best.driftHzPerSec = kDriftValues[dd];
                    out.best.metric = metric;
                }
            }
        }
    }

    // Boundary check by grid index (exact; avoids float comparison). Only the
    // timing and frequency dimensions participate: the drift grid is nearly
    // unobservable in a per-symbol power metric (its within-symbol curvature
    // is <= 6e-3 rad), so a drift edge pick usually just fine-tunes frequency
    // between grid steps and must not veto an otherwise interior optimum. A
    // wrong drift derotation is harmless downstream (negligible
    // within-symbol smear; the Costas-only coherent fit absorbs the residual
    // linear/quadratic trend), while timing/frequency edge picks indicate a
    // runaway optimum outside the trusted box.
    {
        int const d = out.best.deltaSamples - kTimingDeltaMin;
        int f = -1;
        for (int i = 0; i < kFreqSteps; ++i)
            if (kFreqStartHz + i * kFreqStepHz == out.best.deltaHz)
                f = i;
        out.atBoundary =
            (d == 0 || d == (kTimingDeltaMax - kTimingDeltaMin) || f == 0 ||
             f == kFreqSteps - 1);
    }
    return out;
}

/**
 * @brief Safety gate: accept the refined sync only when it is finite, off the
 * search boundary, and measurably better than baseline (>= 0.5% gain, or any
 * positive metric over a degenerate zero baseline).
 */
inline bool refinementAccepted(Refinement const &r) {
    if (!r.searched || !std::isfinite(r.best.metric) || !r.baselineFinite)
        return false;
    if (r.atBoundary)
        return false;
    if (!(r.best.metric > r.baselineMetric))
        return false;
    if (r.baselineMetric > 0.0)
        return r.best.metric >= r.baselineMetric * kMinMetricGain;
    return r.best.metric > 0.0;
}

/** Metric gain in dB for telemetry (0 when baseline is not positive). */
inline double metricGainDb(double refined, double baseline) {
    if (!(refined > 0.0) || !(baseline > 0.0) ||
        !std::isfinite(refined) || !std::isfinite(baseline))
        return 0.0;
    return 10.0 * std::log10(refined / baseline);
}

/** L2 norm of an LLR vector, skipping non-finite entries (telemetry). */
template <std::size_t NBlades>
inline double llrNorm(std::array<float, NBlades> const &llr) {
    double sum = 0.0;
    for (float v : llr) {
        if (std::isfinite(v))
            sum += static_cast<double>(v) * v;
    }
    return std::sqrt(sum);
}

} // namespace aided
} // namespace js8
