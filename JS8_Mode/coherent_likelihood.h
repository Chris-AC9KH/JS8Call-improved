/**
 * @file coherent_likelihood.h
 * @brief Conservative coherent 8-FSK likelihood support for the JS8 decoder.
 *
 * The JS8 waveform is phase-continuous: over one symbol the tone-dependent
 * phase term accumulates exactly `2*pi*tone` radians, so every symbol-start
 * FFT bin for the transmitted tone shares a common carrier phase (mod 2*pi).
 * This header fits that carrier phase trajectory from the known Costas pilot
 * symbols and converts complex data-symbol tone bins into coherent tone
 * scores.
 *
 * Design constraints (see decoder integration):
 * - Only Costas/pilot symbols feed the phase fit; data symbols never do.
 * - The fit is a low-order carrier model
 *       phase(t) = phi0 + 2*pi*delta_f*(t-tc) + pi*fdot*(t-tc)^2
 *   with a bounded residual-frequency/drift grid plus local refinement.
 * - A measurable quality gate (pilot count, span, residual RMS) decides the
 *   blend weight. Poor, ambiguous, or non-finite fits fall back to the
 *   existing noncoherent likelihoods (alpha = 0).
 * - Coherent tone scores are plain rotated real parts in the same amplitude
 *   units as the decoder's magnitude matrix. Absolute amplitude and noise
 *   scales cancel in the decoder's per-symbol moment matching before
 *   blending, so no per-data-symbol amplitude is fitted here.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace js8 {

/// One known-tone pilot observation for the carrier-phase fit.
struct CoherentPilot {
    double timeSeconds = 0.0;          ///< Symbol-start time in seconds.
    std::complex<double> value{};      ///< Complex tone-bin value.
    int tone = 0;                      ///< Expected Costas tone index.
    int symbolIndex = 0;               ///< Global symbol index (debug only).
};

/// Result of the low-order carrier-phase fit over pilot symbols.
struct CarrierPhaseFit {
    bool fitted = false;               ///< True when the model is trusted.
    bool atBound = false;              ///< True when the optimum hit a bound.
    int pilotCount = 0;                ///< Valid pilot observations used.
    double refTimeSeconds = 0.0;       ///< Weighted mean pilot time.
    double phi0 = 0.0;                 ///< Carrier phase at refTime (radians).
    double deltaF = 0.0;               ///< Residual frequency offset (Hz).
    double fdot = 0.0;                 ///< Linear frequency drift (Hz/s).
    double rmsRad = std::numeric_limits<double>::quiet_NaN();
    double coherence = 0.0;            ///< Weighted mean resultant length.
};

/// Debug/benchmark telemetry for one candidate frame.
struct CoherentLikelihoodTelemetry {
    bool enabled = false;              ///< Coherent scores actually blended.
    bool fitted = false;               ///< Phase model passed quality gates.
    int pilotCount = 0;                ///< Valid pilot observations.
    double phaseRmsRad = std::numeric_limits<double>::quiet_NaN();
    double phaseRmsDeg = std::numeric_limits<double>::quiet_NaN();
    double alpha = 0.0;                ///< Blend weight in [0,1].
    double phi0 = std::numeric_limits<double>::quiet_NaN();
    double deltaF = std::numeric_limits<double>::quiet_NaN();
    double fdot = std::numeric_limits<double>::quiet_NaN();
    std::optional<double> avgCoherentToneMargin;
    std::optional<double> avgNoncoherentToneMargin;
};

namespace detail {
// Fit search bounds and quality thresholds. These are fixed, inspectable
// starting points; benchmark them against captured signals before tuning.
constexpr double kMaxDeltaFHz = 1.0;
constexpr double kDeltaFStepHz = 0.05;
constexpr double kMaxDriftHzPerSec = 0.05;
constexpr double kDriftStepHzPerSec = 0.0025;
constexpr double kGoodPhaseRmsRad = 0.12; // ~7 deg: full coherent weight
constexpr double kPoorPhaseRmsRad = 0.35; // ~20 deg: fall back entirely

inline double wrapPhase(double angle) {
    constexpr double twoPi = 2.0 * std::numbers::pi;
    double wrapped = std::fmod(angle + std::numbers::pi, twoPi);
    if (wrapped < 0.0)
        wrapped += twoPi;
    return wrapped - std::numbers::pi;
}
} // namespace detail

/// Fit the carrier-phase model to pilot observations.
///
/// @param pilots Known-tone pilot observations (complex bin + time).
/// @param minPilots Minimum valid pilots required before trusting the fit.
/// @param minSpanSeconds Minimum time span across valid pilots.
/// @return Fit result; `fitted` is false on any degenerate/ambiguous input.
inline CarrierPhaseFit
fitCarrierPhase(std::vector<CoherentPilot> const &pilots, int minPilots,
                double minSpanSeconds) {
    CarrierPhaseFit fit;

    struct ValidPilot {
        double timeSeconds;
        std::complex<double> value;
    };
    std::vector<ValidPilot> valid;
    valid.reserve(pilots.size());
    for (auto const &pilot : pilots) {
        if (!std::isfinite(pilot.timeSeconds) ||
            !std::isfinite(pilot.value.real()) ||
            !std::isfinite(pilot.value.imag()))
            continue;
        if (!(std::abs(pilot.value) > 0.0))
            continue;
        valid.push_back({pilot.timeSeconds, pilot.value});
    }
    fit.pilotCount = static_cast<int>(valid.size());
    if (static_cast<int>(valid.size()) < minPilots || minPilots <= 0)
        return fit;

    double minTime = valid.front().timeSeconds;
    double maxTime = valid.front().timeSeconds;
    std::vector<double> magnitudes;
    magnitudes.reserve(valid.size());
    for (auto const &pilot : valid) {
        minTime = std::min(minTime, pilot.timeSeconds);
        maxTime = std::max(maxTime, pilot.timeSeconds);
        magnitudes.push_back(std::abs(pilot.value));
    }
    if (!(maxTime - minTime >= minSpanSeconds) ||
        !std::isfinite(maxTime - minTime))
        return fit;

    std::nth_element(magnitudes.begin(),
                     magnitudes.begin() + magnitudes.size() / 2,
                     magnitudes.end());
    double const medianMagnitude = magnitudes[magnitudes.size() / 2];
    if (!(medianMagnitude > 0.0) || !std::isfinite(medianMagnitude))
        return fit;

    std::vector<double> weights;
    weights.reserve(valid.size());
    double weightSum = 0.0;
    double weightedTime = 0.0;
    for (auto const &pilot : valid) {
        double const weight =
            std::min(std::abs(pilot.value) / medianMagnitude, 1.0);
        if (!(weight > 0.0) || !std::isfinite(weight))
            return fit;
        weights.push_back(weight);
        weightSum += weight;
        weightedTime += weight * pilot.timeSeconds;
    }
    if (!(weightSum > 0.0) || !std::isfinite(weightSum))
        return fit;
    double const refTime = weightedTime / weightSum;
    fit.refTimeSeconds = refTime;

    auto const coherenceAt = [&](double deltaF, double fdot) {
        std::complex<double> sum{};
        for (std::size_t i = 0; i < valid.size(); ++i) {
            double const tau = valid[i].timeSeconds - refTime;
            double const model =
                2.0 * std::numbers::pi * deltaF * tau +
                std::numbers::pi * fdot * tau * tau;
            sum += weights[i] * valid[i].value *
                   std::exp(std::complex<double>{0.0, -model});
        }
        return sum;
    };

    // Bounded coarse grid over residual frequency/drift, then two rounds of
    // local refinement. This is a small per-candidate fit over 21 pilots, not
    // a per-data-symbol search.
    double bestDeltaF = 0.0;
    double bestDrift = 0.0;
    double bestScore = -1.0;
    double stepF = detail::kDeltaFStepHz;
    double stepD = detail::kDriftStepHzPerSec;
    for (double deltaF = -detail::kMaxDeltaFHz;
         deltaF <= detail::kMaxDeltaFHz + 0.5 * stepF; deltaF += stepF) {
        for (double fdot = -detail::kMaxDriftHzPerSec;
             fdot <= detail::kMaxDriftHzPerSec + 0.5 * stepD;
             fdot += stepD) {
            double const score = std::abs(coherenceAt(deltaF, fdot));
            if (std::isfinite(score) && score > bestScore) {
                bestScore = score;
                bestDeltaF = deltaF;
                bestDrift = fdot;
            }
        }
    }
    // Among near-tied optima (e.g. exact frequency aliases in noiseless data),
    // prefer the smallest residual: the fine-sync and tracking stages ahead of
    // this fit have already removed coarse frequency error.
    for (double deltaF = -detail::kMaxDeltaFHz;
         deltaF <= detail::kMaxDeltaFHz + 0.5 * stepF; deltaF += stepF) {
        for (double fdot = -detail::kMaxDriftHzPerSec;
             fdot <= detail::kMaxDriftHzPerSec + 0.5 * stepD;
             fdot += stepD) {
            double const score = std::abs(coherenceAt(deltaF, fdot));
            if (!std::isfinite(score) ||
                score < bestScore * (1.0 - 1.0e-9))
                continue;
            double const currentMag =
                std::abs(bestDeltaF) + std::abs(bestDrift);
            double const candidateMag = std::abs(deltaF) + std::abs(fdot);
            if (candidateMag < currentMag - 1.0e-12) {
                bestDeltaF = deltaF;
                bestDrift = fdot;
            }
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        stepF *= 0.25;
        stepD *= 0.25;
        for (int di = -2; di <= 2; ++di) {
            for (int dj = -2; dj <= 2; ++dj) {
                double const deltaF = bestDeltaF + di * stepF;
                double const fdot = bestDrift + dj * stepD;
                if (std::abs(deltaF) > detail::kMaxDeltaFHz ||
                    std::abs(fdot) > detail::kMaxDriftHzPerSec)
                    continue;
                double const score = std::abs(coherenceAt(deltaF, fdot));
                if (std::isfinite(score) && score > bestScore) {
                    bestScore = score;
                    bestDeltaF = deltaF;
                    bestDrift = fdot;
                }
            }
        }
    }

    constexpr double boundEps = 1.0e-12;
    if (std::abs(bestDeltaF) >= detail::kMaxDeltaFHz - boundEps ||
        std::abs(bestDrift) >= detail::kMaxDriftHzPerSec - boundEps) {
        fit.atBound = true;
        return fit;
    }

    std::complex<double> const sum = coherenceAt(bestDeltaF, bestDrift);
    double magnitudeSum = 0.0;
    for (std::size_t i = 0; i < valid.size(); ++i)
        magnitudeSum += weights[i] * std::abs(valid[i].value);
    if (!(magnitudeSum > 0.0) || !std::isfinite(magnitudeSum) ||
        !std::isfinite(sum.real()) || !std::isfinite(sum.imag()))
        return fit;

    double residualSum = 0.0;
    for (std::size_t i = 0; i < valid.size(); ++i) {
        double const tau = valid[i].timeSeconds - refTime;
        double const predicted = std::arg(sum) +
                                 2.0 * std::numbers::pi * bestDeltaF * tau +
                                 std::numbers::pi * bestDrift * tau * tau;
        double const residual =
            detail::wrapPhase(std::arg(valid[i].value) - predicted);
        if (!std::isfinite(residual))
            return fit;
        residualSum += weights[i] * residual * residual;
    }

    fit.fitted = true;
    fit.phi0 = std::arg(sum);
    fit.deltaF = bestDeltaF;
    fit.fdot = bestDrift;
    fit.rmsRad = std::sqrt(residualSum / weightSum);
    fit.coherence = std::abs(sum) / magnitudeSum;
    return fit;
}

/// Blend weight in [0,1] from pilot phase-fit quality.
///
/// Full weight below the good RMS threshold, zero above the poor threshold,
/// and a simple linear ramp between them. Returns exactly 0 whenever the fit
/// is untrusted or non-finite.
inline double coherentBlendWeight(CarrierPhaseFit const &fit) {
    if (!fit.fitted || fit.pilotCount <= 0)
        return 0.0;
    if (!std::isfinite(fit.rmsRad))
        return 0.0;
    if (fit.rmsRad <= detail::kGoodPhaseRmsRad)
        return 1.0;
    if (fit.rmsRad >= detail::kPoorPhaseRmsRad)
        return 0.0;
    double const alpha = (detail::kPoorPhaseRmsRad - fit.rmsRad) /
                         (detail::kPoorPhaseRmsRad - detail::kGoodPhaseRmsRad);
    return std::clamp(alpha, 0.0, 1.0);
}

/// Predicted carrier phase at an absolute symbol-start time.
inline double predictCarrierPhase(CarrierPhaseFit const &fit, double timeSeconds) {
    double const tau = timeSeconds - fit.refTimeSeconds;
    return fit.phi0 + 2.0 * std::numbers::pi * fit.deltaF * tau +
           std::numbers::pi * fit.fdot * tau * tau;
}

/// Normalize one symbol's tone scores to zero mean and unit variance.
///
/// Returns false (leaving `out` untouched) when the scores are degenerate or
/// non-finite. Amplitude and noise scales cancel in this mapping, which keeps
/// coherent and noncoherent families comparable before blending.
template <std::size_t Tones>
bool normalizeToneScores(std::array<float, Tones> const &scores,
                         std::array<float, Tones> &out) {
    double mean = 0.0;
    for (float const value : scores) {
        if (!std::isfinite(value))
            return false;
        mean += value;
    }
    mean /= static_cast<double>(scores.size());

    double variance = 0.0;
    for (float const value : scores) {
        double const centered = value - mean;
        variance += centered * centered;
    }
    variance /= static_cast<double>(scores.size());
    if (!std::isfinite(variance) ||
        !(variance > 1.0e-12 * (1.0 + mean * mean)))
        return false;

    double const stddev = std::sqrt(variance);
    for (std::size_t i = 0; i < scores.size(); ++i)
        out[i] = static_cast<float>((scores[i] - mean) / stddev);
    return true;
}

/// Blend noncoherent and coherent tone scores for one symbol.
///
/// With `alpha <= 0` (or any invalid input) this copies `noncoherent` exactly,
/// so the fallback path is bit-identical to the legacy likelihood path.
template <std::size_t Tones>
bool blendToneScores(std::array<float, Tones> const &noncoherent,
                     std::array<float, Tones> const &coherent, float alpha,
                     std::array<float, Tones> &out) {
    if (!(alpha > 0.0f) || !std::isfinite(alpha)) {
        out = noncoherent;
        return false;
    }
    float const clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);
    std::array<float, Tones> normNoncoherent{};
    std::array<float, Tones> normCoherent{};
    if (!normalizeToneScores(noncoherent, normNoncoherent) ||
        !normalizeToneScores(coherent, normCoherent)) {
        out = noncoherent;
        return false;
    }
    for (std::size_t i = 0; i < Tones; ++i)
        out[i] = clampedAlpha * normCoherent[i] +
                 (1.0f - clampedAlpha) * normNoncoherent[i];
    return true;
}

/// Margin between the largest and second-largest tone scores, if well-defined.
template <std::size_t Tones>
std::optional<double> topTwoMargin(std::array<float, Tones> const &scores) {
    float best = -std::numeric_limits<float>::infinity();
    float second = -std::numeric_limits<float>::infinity();
    for (float const value : scores) {
        if (!std::isfinite(value))
            return std::nullopt;
        if (value > best) {
            second = best;
            best = value;
        } else if (value > second) {
            second = value;
        }
    }
    if (!std::isfinite(second))
        return std::nullopt;
    return static_cast<double>(best - second);
}

/// Coherent tone scores for one candidate frame, plus benchmark telemetry.
///
/// `dataBins` holds the scaled complex tone bins (same amplitude units as the
/// decoder's magnitude matrix) and `dataTimes` holds the corresponding
/// symbol-start times. Raw coherent scores are the rotated real parts:
///
///     score = real(bin * exp(-j * predictedPhase))
///
/// Moment matching in the decoder maps these onto the noncoherent score
/// family before blending, so no per-symbol amplitude is fitted here.
struct CoherentToneResult {
    double alpha = 0.0;
    std::vector<std::array<float, 8>> coherentScores;
    std::vector<double> predictedPhase;
    CoherentLikelihoodTelemetry telemetry;
};

inline CoherentToneResult computeCoherentToneScores(
    std::vector<CoherentPilot> const &pilots,
    std::vector<std::array<std::complex<float>, 8>> const &dataBins,
    std::vector<double> const &dataTimes, int minPilots,
    double minSpanSeconds) {
    CoherentToneResult result;
    if (dataBins.size() != dataTimes.size())
        return result;

    CarrierPhaseFit const fit =
        fitCarrierPhase(pilots, minPilots, minSpanSeconds);
    result.telemetry.fitted = fit.fitted;
    result.telemetry.pilotCount = fit.pilotCount;
    result.telemetry.phaseRmsRad = fit.rmsRad;
    result.telemetry.phaseRmsDeg = fit.rmsRad * 180.0 / std::numbers::pi;
    result.telemetry.phi0 = fit.phi0;
    result.telemetry.deltaF = fit.deltaF;
    result.telemetry.fdot = fit.fdot;
    result.telemetry.alpha = coherentBlendWeight(fit);
    if (!fit.fitted || !(result.telemetry.alpha > 0.0))
        return result;

    result.coherentScores.reserve(dataBins.size());
    result.predictedPhase.reserve(dataTimes.size());
    double coherentMarginSum = 0.0;
    double noncoherentMarginSum = 0.0;
    std::size_t marginCount = 0;
    for (std::size_t symbol = 0; symbol < dataBins.size(); ++symbol) {
        if (!std::isfinite(dataTimes[symbol]))
            return CoherentToneResult{};
        double const predicted = predictCarrierPhase(fit, dataTimes[symbol]);
        if (!std::isfinite(predicted))
            return CoherentToneResult{};
        result.predictedPhase.push_back(predicted);
        std::complex<double> const rotation =
            std::exp(std::complex<double>{0.0, -predicted});
        std::array<float, 8> coherent{};
        std::array<float, 8> magnitudes{};
        for (std::size_t tone = 0; tone < 8; ++tone) {
            std::complex<double> const bin{dataBins[symbol][tone].real(),
                                           dataBins[symbol][tone].imag()};
            if (!std::isfinite(bin.real()) || !std::isfinite(bin.imag()))
                return CoherentToneResult{};
            coherent[tone] = static_cast<float>((bin * rotation).real());
            magnitudes[tone] = static_cast<float>(std::abs(bin));
        }
        result.coherentScores.push_back(coherent);
        auto const coherentMargin = topTwoMargin(coherent);
        auto const noncoherentMargin = topTwoMargin(magnitudes);
        if (coherentMargin && noncoherentMargin) {
            coherentMarginSum += *coherentMargin;
            noncoherentMarginSum += *noncoherentMargin;
            ++marginCount;
        }
    }

    result.alpha = result.telemetry.alpha;
    result.telemetry.enabled = true;
    if (marginCount > 0) {
        result.telemetry.avgCoherentToneMargin =
            coherentMarginSum / static_cast<double>(marginCount);
        result.telemetry.avgNoncoherentToneMargin =
            noncoherentMarginSum / static_cast<double>(marginCount);
    }
    return result;
}

} // namespace js8
