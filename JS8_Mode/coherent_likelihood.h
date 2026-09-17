/**
 * @file coherent_likelihood.h
 * @brief Conservative coherent 8-FSK likelihood support for the JS8 decoder.
 *
 * The JS8 waveform is phase-continuous: over one symbol the tone-dependent
 * phase term accumulates exactly `2*pi*tone` radians, so every symbol-start
 * FFT bin for the transmitted tone shares a common carrier phase (mod 2*pi).
 * This header fits that carrier phase trajectory from the known Costas pilot
 * symbols and converts complex data-symbol tone bins into coherent tone
 * numerators.
 *
 * Phase frame: the decoder's FrequencyTracker and TimingTracker perturb the
 * complex bins deterministically (see normalizeFrequencyTrackerPhase() and
 * normalizeTimingPhase()). Every observation is divided back into one
 * consistent nominal-boundary phase frame before fitting or scoring. The
 * trackers themselves are never modified.
 *
 * Likelihood units: with complex noise variance
 *     sigma^2 = toneNoise * symbolNoise
 * (the WhiteningProcessor convention), the tone-dependent part of the
 * coherent log likelihood for a known amplitude A is
 *     (A*projection - 0.5*A*A) * invSigma2,
 * where projection = Re(bin * exp(-j*phase)). The decoder multiplies the
 * numerators produced here by its own per-symbol invSigma2 and blends them
 * with the legacy `0.5*power*invSigma2` scores. No symbol is ever normalized
 * independently, so symbol reliability information is preserved.
 *
 * Design constraints (see decoder integration):
 * - Only Costas/pilot symbols feed the phase fit; data symbols never do.
 * - The fit is a low-order carrier model
 *       phase(t) = phi0 + 2*pi*delta_f*(t-tc) + pi*fdot*(t-tc)^2
 *   solved in closed form from adjacent pilot phase differences plus one
 *   absolute-phase refinement pass: O(pilots), no frequency/drift grid.
 * - A measurable quality gate (pilot count, span, residual RMS) decides the
 *   blend weight. Poor, ambiguous, or non-finite fits fall back to the
 *   existing noncoherent likelihoods (alpha = 0).
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

// One known-tone pilot observation for the carrier-phase fit.
struct CoherentPilot {
    double baseTimeSeconds = 0.0; ///< Nominal symbol-start time in seconds.
    std::complex<double> value{}; ///< Raw complex tone bin.
    int tone = 0;                 ///< Expected tone index.
    int symbolIndex = 0;          ///< Global symbol index (adjacency + debug).
    double timingShiftSamples = 0.0; ///< Effective extraction displacement.
    double trackerHz = 0.0;          ///< FrequencyTracker estimate applied.
};

// One data symbol's raw complex tone bins plus phase metadata.
struct CoherentDataSymbol {
    double baseTimeSeconds = 0.0; ///< Nominal symbol-start time in seconds.
    double timingShiftSamples = 0.0; ///< Effective extraction displacement.
    double trackerHz = 0.0;          ///< FrequencyTracker estimate applied.
    std::array<std::complex<float>, 8> bins{};
};

/// Result of the low-order carrier-phase fit over pilot symbols.
struct CarrierPhaseFit {
    bool fitted = false;               ///< True when the model is trusted.
    int pilotCount = 0;                ///< Valid pilot observations used.
    double refTimeSeconds = 0.0;       ///< Weighted mean pilot time.
    double phi0 = 0.0;                 ///< Carrier phase at refTime (radians).
    double deltaF = 0.0;               ///< Residual frequency offset (Hz).
    double fdot = 0.0;                 ///< Linear frequency drift (Hz/s).
    double amplitude = std::numeric_limits<double>::quiet_NaN();
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
    double pilotAmplitude = std::numeric_limits<double>::quiet_NaN();
    std::optional<double> avgCoherentToneMargin;
    std::optional<double> avgNoncoherentToneMargin;
};

namespace detail {
// Fit bounds and quality thresholds. These are fixed, inspectable starting
// points; benchmark them against captured signals before tuning.
constexpr double kMaxDeltaFHz = 1.0;
constexpr double kMaxDriftHzPerSec = 0.05;
constexpr double kGoodPhaseRmsRad = 0.12; // ~7 deg: full coherent weight
constexpr double kPoorPhaseRmsRad = 0.35; // ~20 deg: fall back entirely

inline double wrapPhase(double angle) {
    constexpr double twoPi = 2.0 * std::numbers::pi;
    double wrapped = std::fmod(angle + std::numbers::pi, twoPi);
    if (wrapped < 0.0)
        wrapped += twoPi;
    return wrapped - std::numbers::pi;
}

// Closed-form weighted least squares for y = a*x0 + b*x1. Returns false when
// the system is singular, degenerate, or non-finite.
inline bool solveWeightedLine2(const double *x0, const double *x1,
                               const double *y, const double *w, std::size_t n,
                               double &a, double &b) {
    double s00 = 0.0, s01 = 0.0, s11 = 0.0, s0y = 0.0, s1y = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x0[i]) || !std::isfinite(x1[i]) ||
            !std::isfinite(y[i]) || !(w[i] > 0.0) || !std::isfinite(w[i]))
            return false;
        s00 += w[i] * x0[i] * x0[i];
        s01 += w[i] * x0[i] * x1[i];
        s11 += w[i] * x1[i] * x1[i];
        s0y += w[i] * x0[i] * y[i];
        s1y += w[i] * x1[i] * y[i];
    }
    double const det = s00 * s11 - s01 * s01;
    double const scale = std::max({1.0, std::abs(s00), std::abs(s01),
                                   std::abs(s11)});
    if (!std::isfinite(det) || !(std::abs(det) > 1.0e-12 * scale * scale))
        return false;
    a = (s11 * s0y - s01 * s1y) / det;
    b = (s00 * s1y - s01 * s0y) / det;
    return std::isfinite(a) && std::isfinite(b);
}
} // namespace detail

/// Effective carrier-model timestamp for an extracted symbol.
///
/// The carrier trajectory must be evaluated where the FFT window actually
/// starts, not at the nominal symbol boundary. Timing-dependent FSK phase is
/// handled separately by normalizeTimingPhase(), so adding the displacement
/// here does not double-count it.
inline double effectiveSymbolTimeSeconds(double baseTimeSeconds,
                                         double shiftSamples,
                                         double sampleRateHz) {
    if (!std::isfinite(baseTimeSeconds) || !std::isfinite(shiftSamples) ||
        !(sampleRateHz > 0.0))
        return std::numeric_limits<double>::quiet_NaN();
    return baseTimeSeconds + shiftSamples / sampleRateHz;
}

/// Remove the deterministic bulk phase of one FrequencyTracker::apply() call.
///
/// apply() multiplies window sample n by `wstep^(n+1)` with
/// `wstep = exp(j*2*pi*trackerHz/sampleRateHz)` and resets every symbol. For
/// a bin-centered tone that contributes exactly
/// `exp(j*dphi*(windowSamples+1)/2)` to the matched FFT bin, so dividing it
/// out leaves the physical carrier trajectory plus only the residual error.
inline std::complex<double>
normalizeFrequencyTrackerPhase(std::complex<double> bin, double trackerHz,
                               double sampleRateHz, int windowSamples) {
    if (!std::isfinite(bin.real()) || !std::isfinite(bin.imag()) ||
        !std::isfinite(trackerHz) || !(sampleRateHz > 0.0) ||
        windowSamples <= 0)
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    double const dphi =
        2.0 * std::numbers::pi * trackerHz / sampleRateHz;
    return bin *
           std::exp(std::complex<double>{
               0.0, -dphi * (static_cast<double>(windowSamples) + 1.0) * 0.5});
}

/// Remove the known tone-dependent phase of a TimingTracker displacement.
///
/// A window starting `shiftSamples` late sees the tone component advanced by
/// `2*pi*tone*shiftSamples/windowSamples`; dividing it out normalizes every
/// symbol to the nominal-boundary phase frame used by the carrier model.
inline std::complex<double>
normalizeTimingPhase(std::complex<double> bin, int tone, double shiftSamples,
                     int windowSamples) {
    if (!std::isfinite(bin.real()) || !std::isfinite(bin.imag()) ||
        !std::isfinite(shiftSamples) || windowSamples <= 0 || tone < 0 ||
        tone >= windowSamples)
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    double const tonePhase = 2.0 * std::numbers::pi *
                             static_cast<double>(tone) * shiftSamples /
                             static_cast<double>(windowSamples);
    return bin * std::exp(std::complex<double>{0.0, -tonePhase});
}

/// Fit the carrier-phase model to pilot observations.
///
/// Pilot bins are first normalized into the nominal phase frame (tracker and
/// timing corrections above). Adjacent same-block phase differences then give
/// a closed-form frequency/drift seed, followed by one absolute-phase
/// refinement pass. Cost is O(pilots): a few dozen trig evaluations, no grid.
///
/// @param pilots Known-tone pilot observations (complex bin + metadata).
/// @param minPilots Minimum valid pilots required before trusting the fit.
/// @param minSpanSeconds Minimum time span across valid pilots.
/// @param windowSamples Decoder symbol FFT length (Mode::NDOWNSPS).
/// @param sampleRateHz Decoder downsampled rate (post-downsample Hz).
/// @return Fit result; `fitted` is false on any degenerate/ambiguous input.
inline CarrierPhaseFit
fitCarrierPhase(std::vector<CoherentPilot> const &pilots, int minPilots,
                double minSpanSeconds, int windowSamples,
                double sampleRateHz) {
    CarrierPhaseFit fit;

    struct NormalizedPilot {
        double timeSeconds;
        std::complex<double> value;
        int symbolIndex;
    };
    std::vector<NormalizedPilot> valid;
    valid.reserve(pilots.size());
    for (auto const &pilot : pilots) {
        double const effectiveTime = effectiveSymbolTimeSeconds(
            pilot.baseTimeSeconds, pilot.timingShiftSamples, sampleRateHz);
        if (!std::isfinite(effectiveTime))
            continue;
        std::complex<double> value = normalizeFrequencyTrackerPhase(
            pilot.value, pilot.trackerHz, sampleRateHz, windowSamples);
        value = normalizeTimingPhase(value, pilot.tone,
                                     pilot.timingShiftSamples, windowSamples);
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            continue;
        if (!(std::abs(value) > 0.0))
            continue;
        valid.push_back(
            {effectiveTime, value, pilot.symbolIndex});
    }
    fit.pilotCount = static_cast<int>(valid.size());
    if (static_cast<int>(valid.size()) < minPilots || minPilots <= 0)
        return fit;
    if (!std::isfinite(minSpanSeconds) || !(minSpanSeconds > 0.0))
        return fit;
    std::sort(valid.begin(), valid.end(),
              [](auto const &a, auto const &b) {
                  return a.timeSeconds < b.timeSeconds;
              });

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

    // Seed from adjacent-symbol phase differences: absolute channel phase
    // cancels, and with tracker-corrected residuals these stay inside
    // (-pi, pi) for plausible inputs. Only truly adjacent symbols are used
    // so no unwrapping across Costas-block gaps is ever attempted here.
    std::vector<double> diffReg0, diffReg1, diffPhase, diffWeight;
    for (std::size_t i = 0; i + 1 < valid.size(); ++i) {
        if (valid[i + 1].symbolIndex != valid[i].symbolIndex + 1)
            continue;
        double const t0 = valid[i].timeSeconds - refTime;
        double const t1 = valid[i + 1].timeSeconds - refTime;
        diffReg0.push_back(t1 - t0);
        diffReg1.push_back(t1 * t1 - t0 * t0);
        diffPhase.push_back(detail::wrapPhase(
            std::arg(valid[i + 1].value * std::conj(valid[i].value))));
        diffWeight.push_back(std::min(weights[i], weights[i + 1]));
    }
    if (diffPhase.empty())
        return fit;
    // Phase model per difference: dPhase = 2*pi*deltaF*dReg0 + pi*fdot*dReg1.
    std::vector<double> scaledReg0 = diffReg0, scaledReg1 = diffReg1;
    for (auto &v : scaledReg0)
        v *= 2.0 * std::numbers::pi;
    for (auto &v : scaledReg1)
        v *= std::numbers::pi;
    double deltaF = 0.0, fdot = 0.0;
    if (!detail::solveWeightedLine2(scaledReg0.data(), scaledReg1.data(),
                                    diffPhase.data(), diffWeight.data(),
                                    diffPhase.size(), deltaF, fdot))
        return fit;

    // One absolute-phase refinement pass: derotate by the seed, unwrap
    // sequentially (residuals are small by construction), then solve the full
    // quadratic model including the absolute phase.
    double phi0 = 0.0;
    double phi0Previous = 0.0;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<double> unwrapped;
        unwrapped.reserve(valid.size());
        double previous = 0.0;
        bool first = true;
        for (std::size_t i = 0; i < valid.size(); ++i) {
            double const tau = valid[i].timeSeconds - refTime;
            double const rotated = std::arg(
                valid[i].value *
                std::exp(std::complex<double>{
                    0.0, -(2.0 * std::numbers::pi * deltaF * tau +
                           std::numbers::pi * fdot * tau * tau)}));
            if (!std::isfinite(rotated))
                return fit;
            double value = rotated;
            if (!first)
                value = previous + detail::wrapPhase(rotated - previous);
            unwrapped.push_back(value);
            previous = value;
            first = false;
        }
        std::vector<double> reg0(valid.size(), 0.0);
        std::vector<double> reg1(valid.size(), 0.0);
        for (std::size_t i = 0; i < valid.size(); ++i) {
            double const tau = valid[i].timeSeconds - refTime;
            reg0[i] = 2.0 * std::numbers::pi * tau;
            reg1[i] = std::numbers::pi * tau * tau;
        }
        // Absolute-phase estimate: with the seed derotated out, `unwrapped`
        // already holds the carrier phase at the reference time plus only the
        // residual frequency/drift error. The weighted circular mean of the
        // derotated phases therefore estimates phi0 directly; re-applying the
        // seed regressors here would double-subtract the trend and wrap the
        // residuals around the circle for any non-trivial frequency error.
        double correction = 0.0;
        {
            std::complex<double> mean{};
            for (std::size_t i = 0; i < valid.size(); ++i)
                mean += weights[i] *
                        std::exp(std::complex<double>{0.0, unwrapped[i]});
            if (!(std::abs(mean) > 0.0) || !std::isfinite(mean.real()) ||
                !std::isfinite(mean.imag()))
                return fit;
            correction = std::arg(mean);
            phi0 = correction;
        }
        // Residual trend after removing phi0: Linear/drift corrections are
        // fitted against the small wrapped residuals around phi0.
        std::vector<double> residuals(valid.size());
        for (std::size_t i = 0; i < valid.size(); ++i)
            residuals[i] =
                detail::wrapPhase(unwrapped[i] - phi0);
        double stepF = 0.0, stepD = 0.0;
        if (!detail::solveWeightedLine2(reg0.data(), reg1.data(),
                                        residuals.data(), weights.data(),
                                        valid.size(), stepF, stepD))
            return fit;
        deltaF += stepF;
        fdot += stepD;
        if (std::abs(stepF) < 1.0e-12 && std::abs(stepD) < 1.0e-12 &&
            std::abs(correction - phi0Previous) < 1.0e-12)
            break;
        phi0Previous = correction;
    }

    if (std::abs(deltaF) >= detail::kMaxDeltaFHz ||
        std::abs(fdot) >= detail::kMaxDriftHzPerSec)
        return fit;

    double residualSum = 0.0;
    std::complex<double> coherentSum{};
    double magnitudeSum = 0.0;
    for (std::size_t i = 0; i < valid.size(); ++i) {
        double const tau = valid[i].timeSeconds - refTime;
        double const predicted = phi0 +
                                 2.0 * std::numbers::pi * deltaF * tau +
                                 std::numbers::pi * fdot * tau * tau;
        double const residual =
            detail::wrapPhase(std::arg(valid[i].value) - predicted);
        if (!std::isfinite(residual))
            return fit;
        residualSum += weights[i] * residual * residual;
        coherentSum += weights[i] * valid[i].value *
                       std::exp(std::complex<double>{0.0, -predicted});
        magnitudeSum += weights[i] * std::abs(valid[i].value);
    }
    if (!(magnitudeSum > 0.0) || !std::isfinite(magnitudeSum))
        return fit;

    fit.fitted = true;
    fit.phi0 = phi0;
    fit.deltaF = deltaF;
    fit.fdot = fdot;
    // Robust pilot amplitude: median normalized-bin magnitude, in the same
    // scaled units as the decoder magnitude matrix. Shared by all tones, so
    // no per-symbol amplitude is ever fitted.
    fit.amplitude = medianMagnitude;
    fit.rmsRad = std::sqrt(residualSum / weightSum);
    fit.coherence = std::abs(coherentSum) / magnitudeSum;
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
inline double predictCarrierPhase(CarrierPhaseFit const &fit,
                                  double timeSeconds) {
    double const tau = timeSeconds - fit.refTimeSeconds;
    return fit.phi0 + 2.0 * std::numbers::pi * fit.deltaF * tau +
           std::numbers::pi * fit.fdot * tau * tau;
}

/// Blend physical noncoherent and coherent tone scores for one symbol.
///
/// `noncoherent[i]` must hold the decoder's `0.5*power*invSigma2` values and
/// `coherent[i]` the `(A*projection - 0.5*A*A)*invSigma2` values sharing the
/// same `invSigma2`. With `alpha <= 0` (or any invalid input) this copies
/// `noncoherent` exactly, so the fallback path is bit-identical to the
/// legacy likelihood path. Symbols are never renormalized: reliability
/// information in the score magnitudes is preserved.
template <std::size_t Tones>
bool blendToneScores(std::array<float, Tones> const &noncoherent,
                     std::array<float, Tones> const &coherent, float alpha,
                     std::array<float, Tones> &out) {
    if (!(alpha > 0.0f) || !std::isfinite(alpha)) {
        out = noncoherent;
        return false;
    }
    float const clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);
    for (std::size_t i = 0; i < Tones; ++i) {
        if (!std::isfinite(noncoherent[i]) || !std::isfinite(coherent[i])) {
            out = noncoherent;
            return false;
        }
        out[i] = (1.0f - clampedAlpha) * noncoherent[i] +
                 clampedAlpha * coherent[i];
    }
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

/// Per-tone coherent numerators for one candidate frame, plus telemetry.
///
/// For each data symbol and candidate tone, the raw complex bin is first
/// normalized with that symbol's tracker metadata, then rotated by the
/// predicted carrier phase:
///     projection = Re(normalizedBin * exp(-j*predictedPhase))
///     numerator  = A*projection - 0.5*A*A
/// The decoder multiplies these numerators by its own per-symbol invSigma2,
/// exactly as it does the noncoherent `0.5*power` numerators.
struct CoherentToneResult {
    double alpha = 0.0;
    double amplitude = 0.0;
    std::vector<std::array<float, 8>> coherentNumerators;
    std::vector<double> predictedPhase;
    CoherentLikelihoodTelemetry telemetry;
};

inline CoherentToneResult computeCoherentToneScores(
    std::vector<CoherentPilot> const &pilots,
    std::vector<CoherentDataSymbol> const &data, int minPilots,
    double minSpanSeconds, int windowSamples, double sampleRateHz) {
    CoherentToneResult result;
    if (windowSamples <= 0 || !(sampleRateHz > 0.0))
        return result;

    CarrierPhaseFit const fit = fitCarrierPhase(
        pilots, minPilots, minSpanSeconds, windowSamples, sampleRateHz);
    result.telemetry.fitted = fit.fitted;
    result.telemetry.pilotCount = fit.pilotCount;
    result.telemetry.phaseRmsRad = fit.rmsRad;
    result.telemetry.phaseRmsDeg = fit.rmsRad * 180.0 / std::numbers::pi;
    result.telemetry.phi0 = fit.phi0;
    result.telemetry.deltaF = fit.deltaF;
    result.telemetry.fdot = fit.fdot;
    result.telemetry.pilotAmplitude = fit.amplitude;
    result.telemetry.alpha = coherentBlendWeight(fit);
    if (!fit.fitted || !(result.telemetry.alpha > 0.0))
        return result;
    if (!std::isfinite(fit.amplitude) || !(fit.amplitude > 0.0))
        return CoherentToneResult{};

    result.amplitude = fit.amplitude;
    result.coherentNumerators.reserve(data.size());
    result.predictedPhase.reserve(data.size());
    double coherentMarginSum = 0.0;
    double noncoherentMarginSum = 0.0;
    std::size_t marginCount = 0;
    for (auto const &symbol : data) {
        double const effectiveTime = effectiveSymbolTimeSeconds(
            symbol.baseTimeSeconds, symbol.timingShiftSamples, sampleRateHz);
        if (!std::isfinite(effectiveTime))
            return CoherentToneResult{};
        double const predicted =
            predictCarrierPhase(fit, effectiveTime);
        if (!std::isfinite(predicted))
            return CoherentToneResult{};
        result.predictedPhase.push_back(predicted);
        std::array<float, 8> numerators{};
        std::array<float, 8> magnitudes{};
        for (std::size_t tone = 0; tone < 8; ++tone) {
            std::complex<double> bin{symbol.bins[tone].real(),
                                     symbol.bins[tone].imag()};
            bin = normalizeFrequencyTrackerPhase(bin, symbol.trackerHz,
                                                 sampleRateHz, windowSamples);
            bin = normalizeTimingPhase(bin, static_cast<int>(tone),
                                       symbol.timingShiftSamples,
                                       windowSamples);
            if (!std::isfinite(bin.real()) || !std::isfinite(bin.imag()))
                return CoherentToneResult{};
            std::complex<double> const rotated =
                bin * std::exp(std::complex<double>{0.0, -predicted});
            double const projection = rotated.real();
            if (!std::isfinite(projection))
                return CoherentToneResult{};
            numerators[tone] = static_cast<float>(
                fit.amplitude * projection -
                0.5 * fit.amplitude * fit.amplitude);
            magnitudes[tone] = static_cast<float>(std::abs(bin));
        }
        result.coherentNumerators.push_back(numerators);
        auto const coherentMargin = topTwoMargin(numerators);
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

/// Blended coherent input for one candidate frame, in WhiteningProcessor's
/// `s1` orientation (`[tone][symbol]`).
template <int ToneRows, int DataSymbols> struct CoherentBlend {
    std::array<std::array<float, DataSymbols>, ToneRows> numerators{};
    double amplitude = 0.0;
    float alpha = 0.0f;
};

} // namespace js8
