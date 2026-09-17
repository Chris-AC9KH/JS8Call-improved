/**
 * @file sic_refinement.h
 * @brief Post-decode waveform refinement for successive interference cancellation.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace js8 {
struct SicRefinementResult {
    std::vector<std::complex<float>> reference;
    int timingAdjustSamples = 0;
    float frequencyOffsetHz = 0.0f;
    float driftHzPerSecond = 0.0f;
    double nominalMetric = 0.0;
    double refinedMetric = 0.0;
    double phaseFitRmsHz = 0.0;
    int phasePairs = 0;
    bool refined = false;

    // Part #5c: continuous linear timing drift. slope is sample-clock drift in
    // samples per second; endToEnd is that slope integrated over the frame in
    // samples (bounded to ~2); fitRMS is the regional-optimum residual in
    // samples; regions is the count of useful regions feeding the fit.
    double timingDriftSamplesPerSecond = 0.0;
    double timingEndToEndSamples = 0.0;
    double timingFitRmsSamples = 0.0;
    int timingRegions = 0;
    bool timingRefined = false;
};

// Continuous, phase-preserving fractional delay of a complex JS8 reference.
//
// A sample-clock drift is a time warp, not merely a waveform interpolation
// problem. Linear Cartesian interpolation would systematically attenuate a
// nonzero-carrier waveform, so unwrap the reference phase once and linearly
// interpolate that phase (and magnitude). The returned vector has the same
// length; source samples outside the frame become zero, matching the existing
// #5b shift/edge convention.
inline std::vector<std::complex<float>> applySicTimingWarp(
    std::vector<std::complex<float>> const &reference,
    double interceptSamples, double slopeSamplesPerSecond,
    double sampleRate) {
    std::size_t const length = reference.size();
    if (length < 2 || !(sampleRate > 0.0) ||
        !std::isfinite(interceptSamples) ||
        !std::isfinite(slopeSamplesPerSecond))
        return reference;

    std::vector<double> unwrapped;
    unwrapped.reserve(length);
    double phase = std::arg(reference.front());
    if (!std::isfinite(phase))
        return reference;
    unwrapped.push_back(phase);
    for (std::size_t i = 1; i < length; ++i) {
        if (!std::isfinite(reference[i].real()) ||
            !std::isfinite(reference[i].imag()))
            return reference;
        double const step =
            std::arg(reference[i] * std::conj(reference[i - 1]));
        if (!std::isfinite(step))
            return reference;
        phase += step;
        unwrapped.push_back(phase);
    }

    double const last = static_cast<double>(length - 1);
    std::vector<std::complex<float>> warped(length, {0.0F, 0.0F});
    for (std::size_t p = 0; p < length; ++p) {
        double const time = static_cast<double>(p) / sampleRate;
        double const source =
            static_cast<double>(p) - (interceptSamples + slopeSamplesPerSecond * time);
        if (!(source >= 0.0) || !(source <= last))
            continue;
        std::size_t const i0 = static_cast<std::size_t>(source);
        std::size_t const i1 = std::min(i0 + 1, length - 1);
        double const fraction = source - static_cast<double>(i0);
        if (fraction == 0.0) {
            // Preserve the original sample exactly at integer source indices.
            warped[p] = reference[i0];
            continue;
        }
        double const sourcePhase = unwrapped[i0] * (1.0 - fraction) +
                                   unwrapped[i1] * fraction;
        double const sourceMagnitude =
            std::abs(reference[i0]) * (1.0 - fraction) +
            std::abs(reference[i1]) * fraction;
        warped[p] = std::polar(static_cast<float>(sourceMagnitude),
                               static_cast<float>(sourcePhase));
    }
    return warped;
}

// Wrap an angle to [-pi, pi] so phase residuals stay on the principal branch.
inline double wrapSicPhase(double angle) {
    constexpr double twoPi = 6.283185307179586476925286766559;
    double wrapped = std::fmod(angle + 0.5 * twoPi, twoPi);
    if (wrapped < 0.0)
        wrapped += twoPi;
    return wrapped - 0.5 * twoPi;
}

// Solve a 3x3 linear system by Gaussian elimination with partial pivoting.
// Returns false for singular/degenerate systems. The tolerance is relative to
// the matrix scale so both small and large correlation magnitudes are handled.
inline bool solveSicLinearSystem3(double const normal[3][3],
                                  double const right[3], double solution[3]) {
    double augmented[3][4] = {};
    double scale = 0.0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            augmented[row][column] = normal[row][column];
            scale = std::max(scale, std::abs(normal[row][column]));
        }
        augmented[row][3] = right[row];
    }
    if (!(scale > 0.0) || !std::isfinite(scale))
        return false;

    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(augmented[row][column]) >
                std::abs(augmented[pivot][column]))
                pivot = row;
        }
        for (int column2 = column; column2 < 4; ++column2)
            std::swap(augmented[column][column2], augmented[pivot][column2]);
        if (!(std::abs(augmented[column][column]) > 1.0e-12 * scale))
            return false;
        for (int row = 0; row < 3; ++row) {
            if (row == column)
                continue;
            double const factor =
                augmented[row][column] / augmented[column][column];
            for (int column2 = column; column2 < 4; ++column2)
                augmented[row][column2] -=
                    factor * augmented[column][column2];
        }
    }

    for (int row = 0; row < 3; ++row) {
        solution[row] = augmented[row][3] / augmented[row][row];
        if (!std::isfinite(solution[row]))
            return false;
    }
    return true;
}

template <int SymbolSamples, std::size_t Symbols, typename Samples, typename Tones>
SicRefinementResult refineSicReference(
    std::vector<std::complex<float>> nominal, Samples const &samples,
    Tones const &tones, float const dt, bool const allowTimingDrift) {
    constexpr double sampleRate = 12000.0;
    constexpr double twoPi = 6.283185307179586476925286766559;

    SicRefinementResult result;
    result.reference = std::move(nominal);
    auto const originalReference = result.reference;

    auto const correlationsFor =
        [&](std::vector<std::complex<float>> const &reference,
            int const nstart, int const symbolStride) {
            std::array<std::complex<double>, Symbols> correlations{};
            int const stride = std::max(symbolStride, 1);

            for (std::size_t symbol = 0; symbol < Symbols;
                 symbol += static_cast<std::size_t>(stride)) {
                std::size_t const referenceBegin = symbol * SymbolSamples;
                std::size_t const referenceEnd = std::min(
                    referenceBegin + static_cast<std::size_t>(SymbolSamples),
                    reference.size());

                for (std::size_t referenceIndex = referenceBegin;
                     referenceIndex < referenceEnd; ++referenceIndex) {
                    long long const sampleIndex =
                        static_cast<long long>(nstart) +
                        static_cast<long long>(referenceIndex);
                    if (sampleIndex < 0 ||
                        sampleIndex >= static_cast<long long>(samples.size()))
                        continue;

                    auto const r = std::complex<double>{
                        reference[referenceIndex].real(),
                        reference[referenceIndex].imag()};
                    correlations[symbol] +=
                        static_cast<double>(samples[static_cast<std::size_t>(sampleIndex)]) *
                        std::conj(r);
                }
            }

            return correlations;
        };

    auto const correlationPower = [](auto const &correlations) {
        return std::accumulate(
            correlations.begin(), correlations.end(), 0.0,
            [](double const total, auto const &value) {
                return total + std::norm(value);
            });
    };

    auto const metricFor = [&](auto const &reference, int const nstart,
                               int const symbolStride) {
        return correlationPower(
            correlationsFor(reference, nstart, symbolStride));
    };

    auto const applyPhaseModel = [&](auto &reference,
                                     double const frequencyOffsetHz,
                                     double const driftHzPerSecond) {
        for (std::size_t i = 0; i < reference.size(); ++i) {
            double const t = static_cast<double>(i) / sampleRate;
            double const phase =
                twoPi * (frequencyOffsetHz * t +
                         0.5 * driftHzPerSecond * t * t);
            reference[i] *= std::complex<float>{
                static_cast<float>(std::cos(phase)),
                static_cast<float>(std::sin(phase))};
        }
    };

    int const nominalStart =
        static_cast<int>(std::round(static_cast<double>(dt) * sampleRate));
    result.nominalMetric = metricFor(result.reference, nominalStart, 1);
    result.refinedMetric = result.nominalMetric;

    if (!(result.nominalMetric > std::numeric_limits<double>::epsilon()) ||
        !std::isfinite(result.nominalMetric))
        return result;

    // The complete decoded message gives us all transmitted tones. Refine the
    // start time against short coherent windows around known tone transitions;
    // those boundaries contain far more timing information than the interiors
    // of constant-frequency symbols, where a sample shift mostly looks like a
    // harmless phase rotation.
    int bestStart = nominalStart;
    int bestDelta = 0;
    int const searchRadius = std::clamp(SymbolSamples / 16, 8, 120);
    int const transitionHalfWindow = std::clamp(SymbolSamples / 8, 16, 160);

    auto const transitionMetricOf =
        [&](std::vector<std::complex<float>> const &ref, int const nstart) {
            double metric = 0.0;
            int transitions = 0;
            for (std::size_t symbol = 1; symbol < Symbols; ++symbol) {
                if (tones[symbol] == tones[symbol - 1])
                    continue;

                long long const center =
                    static_cast<long long>(symbol) * SymbolSamples;
                std::complex<double> correlation{};
                for (int offset = -transitionHalfWindow;
                     offset < transitionHalfWindow; ++offset) {
                    long long const referenceIndex = center + offset;
                    long long const sampleIndex =
                        static_cast<long long>(nstart) + referenceIndex;
                    if (referenceIndex < 0 ||
                        referenceIndex >=
                            static_cast<long long>(ref.size()) ||
                        sampleIndex < 0 ||
                        sampleIndex >= static_cast<long long>(samples.size()))
                        continue;

                    auto const r = std::complex<double>{
                        ref[static_cast<std::size_t>(referenceIndex)].real(),
                        ref[static_cast<std::size_t>(referenceIndex)].imag()};
                    correlation +=
                        static_cast<double>(samples[static_cast<std::size_t>(sampleIndex)]) *
                        std::conj(r);
                }
                metric += std::norm(correlation);
                ++transitions;
            }
            return std::pair<double, int>{metric, transitions};
        };

    // Convenience wrapper evaluating the transition correlation for the current
    // (in-out) reference.
    auto const transitionMetric = [&](int const nstart) {
        return transitionMetricOf(result.reference, nstart);
    };

    auto const [nominalTransitionMetric, transitionCount] =
        transitionMetric(nominalStart);
    double bestTransitionMetric = nominalTransitionMetric;

    if (transitionCount >= 6 && nominalTransitionMetric > 0.0) {
        for (int delta = -searchRadius; delta <= searchRadius; delta += 4) {
            auto const [metric, ignored] =
                transitionMetric(nominalStart + delta);
            (void)ignored;
            if (metric > bestTransitionMetric) {
                bestTransitionMetric = metric;
                bestDelta = delta;
            }
        }

        int const localStart = std::max(-searchRadius, bestDelta - 3);
        int const localEnd = std::min(searchRadius, bestDelta + 3);
        for (int delta = localStart; delta <= localEnd; ++delta) {
            auto const [metric, ignored] =
                transitionMetric(nominalStart + delta);
            (void)ignored;
            if (metric > bestTransitionMetric) {
                bestTransitionMetric = metric;
                bestDelta = delta;
            }
        }

        // Require a visible transition-score gain and ensure that the ordinary
        // full-frame metric is not materially degraded. #5a still performs the
        // final before/after subtraction guard.
        double const timingMetric =
            metricFor(result.reference, nominalStart + bestDelta, 1);
        bool const transitionImproved =
            bestTransitionMetric >= nominalTransitionMetric * 1.005;
        bool const fullFrameSafe =
            std::isfinite(timingMetric) &&
            timingMetric >= result.nominalMetric * 0.999;
        if (transitionImproved && fullFrameSafe) {
            bestStart = nominalStart + bestDelta;
            result.timingAdjustSamples = bestDelta;
            result.refinedMetric = timingMetric;
        }
    }

    // With timing aligned, the per-symbol complex correlations expose the
    // phase trajectory of the decoded signal. Adjacent phase differences give
    // a residual-frequency measurement without needing an absolute channel
    // phase. Fit a straight line in frequency, i.e. a quadratic phase model,
    // to capture a constant offset plus linear drift.
    auto const correlations = correlationsFor(result.reference, bestStart, 1);
    std::array<double, Symbols> magnitudes{};
    for (std::size_t i = 0; i < Symbols; ++i)
        magnitudes[i] = std::abs(correlations[i]);

    auto sortedMagnitudes = magnitudes;
    std::nth_element(sortedMagnitudes.begin(),
                     sortedMagnitudes.begin() + sortedMagnitudes.size() / 2,
                     sortedMagnitudes.end());
    double const medianMagnitude =
        sortedMagnitudes[sortedMagnitudes.size() / 2];

    if (medianMagnitude > std::numeric_limits<double>::epsilon() &&
        std::isfinite(medianMagnitude)) {
        double const symbolSeconds =
            static_cast<double>(SymbolSamples) / sampleRate;
        double const minimumMagnitude = 0.25 * medianMagnitude;
        double const maximumWeight = 4.0 * medianMagnitude;
        double sw = 0.0;
        double swt = 0.0;
        double swtt = 0.0;
        double swy = 0.0;
        double swty = 0.0;

        for (std::size_t k = 0; k + 1 < Symbols; ++k) {
            double const m0 = magnitudes[k];
            double const m1 = magnitudes[k + 1];
            if (m0 < minimumMagnitude || m1 < minimumMagnitude)
                continue;

            double const phaseStep =
                std::arg(correlations[k + 1] * std::conj(correlations[k]));
            double const residualHz = phaseStep / (twoPi * symbolSeconds);
            double const t = static_cast<double>(k + 1) * symbolSeconds;
            double const weight = std::min({m0, m1, maximumWeight});

            sw += weight;
            swt += weight * t;
            swtt += weight * t * t;
            swy += weight * residualHz;
            swty += weight * t * residualHz;
            ++result.phasePairs;
        }

        if (result.phasePairs >= 12 && sw > 0.0) {
            double const meanFrequency = swy / sw;
            double frequencyOffsetHz = meanFrequency;
            double driftHzPerSecond = 0.0;
            double const determinant = sw * swtt - swt * swt;

            if (std::abs(determinant) >
                std::numeric_limits<double>::epsilon()) {
                frequencyOffsetHz =
                    (swy * swtt - swt * swty) / determinant;
                driftHzPerSecond =
                    (sw * swty - swt * swy) / determinant;
            }

            double residualSum = 0.0;
            for (std::size_t k = 0; k + 1 < Symbols; ++k) {
                double const m0 = magnitudes[k];
                double const m1 = magnitudes[k + 1];
                if (m0 < minimumMagnitude || m1 < minimumMagnitude)
                    continue;

                double const phaseStep = std::arg(
                    correlations[k + 1] * std::conj(correlations[k]));
                double const residualHz =
                    phaseStep / (twoPi * symbolSeconds);
                double const t = static_cast<double>(k + 1) * symbolSeconds;
                double const weight = std::min({m0, m1, maximumWeight});
                double const error =
                    residualHz - (frequencyOffsetHz + driftHzPerSecond * t);
                residualSum += weight * error * error;
            }

            result.phaseFitRmsHz = std::sqrt(residualSum / sw);

            bool const frequencyValid =
                std::isfinite(meanFrequency) && std::abs(meanFrequency) <= 1.0;
            bool const driftValid =
                std::isfinite(frequencyOffsetHz) &&
                std::isfinite(driftHzPerSecond) &&
                std::isfinite(result.phaseFitRmsHz) &&
                std::abs(frequencyOffsetHz) <= 1.0 &&
                std::abs(driftHzPerSecond) <= 0.15 &&
                std::abs(driftHzPerSecond) *
                        (static_cast<double>(Symbols) * symbolSeconds) <=
                    1.0 &&
                result.phaseFitRmsHz <= 1.0;

            auto bestReference = result.reference;
            double bestMetric = result.refinedMetric;
            double chosenFrequency = 0.0;
            double chosenDrift = 0.0;

            if (frequencyValid) {
                auto candidate = result.reference;
                applyPhaseModel(candidate, meanFrequency, 0.0);
                double const metric = metricFor(candidate, bestStart, 1);
                if (std::isfinite(metric) &&
                    metric >= bestMetric * 1.0001) {
                    bestReference = std::move(candidate);
                    bestMetric = metric;
                    chosenFrequency = meanFrequency;
                }
            }

            if (driftValid) {
                auto candidate = result.reference;
                applyPhaseModel(candidate, frequencyOffsetHz, driftHzPerSecond);
                double const metric = metricFor(candidate, bestStart, 1);
                if (std::isfinite(metric) &&
                    metric >= bestMetric * 1.0001) {
                    bestReference = std::move(candidate);
                    bestMetric = metric;
                    chosenFrequency = frequencyOffsetHz;
                    chosenDrift = driftHzPerSecond;
                }
            }

            result.reference = std::move(bestReference);
            result.frequencyOffsetHz = static_cast<float>(chosenFrequency);
            result.driftHzPerSecond = static_cast<float>(chosenDrift);
            result.refinedMetric = bestMetric;
        }
    }

    // Part #5c: sample-clock/timing drift across the frame. #5b aligns one
    // global timing offset; the decoded 79-symbol waveform also lets us fit how
    // that offset drifts linearly, timing(t) = tau0 + tau_dot * t.
    //
    // Slow (1-2 samples, end-to-end) drift is distributed sub-sample, so it is
    // recovered from the phase trajectory across the whole frame rather than
    // from the amplitude peak of a single broad transition window. Adjacent
    // symbols with different tones expose the drift: their phase difference
    // depends on the known instantaneous frequencies at the two centers. Those
    // tone-transition pairs are grouped into six temporal regions. A bounded
    // line is fit through the useful regional timing corrections and then
    // applied as a continuous, phase-preserving warp. The warp is baked into
    // result.reference, so subtractjs8 sees no change to its timing path and
    // needs no new parameters. Skipped when disabled.
    if (allowTimingDrift) {
        constexpr std::size_t regionCount = 6;
        constexpr std::size_t requiredRegions = 4;
        constexpr std::size_t minRegionPairs = 2;
        std::size_t const frameSamples =
            Symbols * static_cast<std::size_t>(SymbolSamples);
        std::size_t const refLen = result.reference.size();
        if (refLen == frameSamples && frameSamples > 0) {
            double const frameSeconds =
                static_cast<double>(frameSamples) / sampleRate;

            // End-to-end bound: at most ~2 samples across the whole frame, but
            // also a small fraction of one symbol so short-symbol modes stay
            // bounded (2.0 samples for every current mode).
            double const maxEndToEndDrift =
                std::min(2.0, 0.5 * static_cast<double>(SymbolSamples));

            // Baseline whole-frame metric (non-degradation guard) and the #5b
            // single global integer offset. bestStart already folds bestDelta.
            double const baseWhole =
                metricFor(result.reference, bestStart, 1);
            auto const [baseTWM, baseTransitions] =
                transitionMetricOf(result.reference, bestStart);
            if (std::isfinite(baseWhole) && baseWhole > 0.0 &&
                std::isfinite(baseTWM) && baseTWM > 0.0 &&
                baseTransitions >= 6) {
                // Instantaneous reference frequency within each symbol. Averaging
                // the complex phasor over the central half of a symbol avoids the
                // phase-slope discontinuity at its edges while retaining the
                // baseband, data-tone, and accepted #5b frequency corrections.
                std::array<double, Symbols> symbolFrequency{};
                std::array<bool, Symbols> symbolFrequencyValid{};
                for (std::size_t symbol = 0; symbol < Symbols; ++symbol) {
                    std::size_t const begin =
                        symbol * static_cast<std::size_t>(SymbolSamples);
                    std::size_t const end =
                        std::min(begin + static_cast<std::size_t>(SymbolSamples),
                                 refLen);
                    if (end <= begin)
                        continue;

                    std::size_t const usable = end - begin;
                    std::size_t margin = usable / 4;
                    if (margin == 0)
                        margin = usable;
                    std::size_t first = begin + margin;
                    std::size_t last = (end > margin) ? end - margin : end;
                    if (last <= first + 1) {
                        first = begin;
                        last = end;
                    }
                    if (last <= first + 1)
                        continue;

                    std::complex<double> phasorSum{};
                    std::size_t phasorCount = 0;
                    for (std::size_t i = first; i + 1 < last; ++i) {
                        phasorSum += static_cast<std::complex<double>>(
                                         result.reference[i + 1]) *
                                     std::conj(static_cast<std::complex<double>>(
                                         result.reference[i]));
                        ++phasorCount;
                    }
                    if (phasorCount == 0 ||
                        !(std::abs(phasorSum) > 0.0) ||
                        !std::isfinite(phasorSum.real()) ||
                        !std::isfinite(phasorSum.imag()))
                        continue;
                    double const omega = std::arg(phasorSum);
                    if (!std::isfinite(omega))
                        continue;
                    symbolFrequency[symbol] = omega;
                    symbolFrequencyValid[symbol] = true;
                }

                // Per-symbol complex correlations aligned with the accepted
                // integer #5b offset. Their phases expose timing drift through
                // the known frequency of each symbol.
                auto const timingCorrelations =
                    correlationsFor(result.reference, bestStart, 1);
                std::array<double, Symbols> timingMagnitudes{};
                for (std::size_t i = 0; i < Symbols; ++i)
                    timingMagnitudes[i] = std::abs(timingCorrelations[i]);
                auto sortedMagnitudes = timingMagnitudes;
                std::nth_element(sortedMagnitudes.begin(),
                                 sortedMagnitudes.begin() +
                                     sortedMagnitudes.size() / 2,
                                 sortedMagnitudes.end());
                double const medianMagnitude =
                    sortedMagnitudes[sortedMagnitudes.size() / 2];
                if (std::isfinite(medianMagnitude) &&
                    medianMagnitude >
                        std::numeric_limits<double>::epsilon()) {
                    double const minimumMagnitude = 0.25 * medianMagnitude;
                    double const maximumWeight = 4.0 * medianMagnitude;

                    struct TimingPhasePair {
                        double deltaTime;
                        double interceptGain;
                        double slopeGain;
                        double phase;
                        double weight;
                        double centerTime;
                        std::size_t region;
                    };
                    std::vector<TimingPhasePair> pairs;
                    pairs.reserve(Symbols);
                    std::array<std::size_t, regionCount> regionTransitionCounts{};

                    for (std::size_t symbol = 0; symbol + 1 < Symbols; ++symbol) {
                        if (tones[symbol + 1] == tones[symbol])
                            continue;
                        if (!symbolFrequencyValid[symbol] ||
                            !symbolFrequencyValid[symbol + 1])
                            continue;

                        double const magnitude0 =
                            timingMagnitudes[symbol];
                        double const magnitude1 =
                            timingMagnitudes[symbol + 1];
                        if (magnitude0 < minimumMagnitude ||
                            magnitude1 < minimumMagnitude)
                            continue;
                        double const weight =
                            std::min({magnitude0, magnitude1, maximumWeight});
                        if (!(weight > 0.0) || !std::isfinite(weight))
                            continue;

                        double const center0 =
                            (static_cast<double>(symbol) *
                                 static_cast<double>(SymbolSamples) +
                             0.5 * static_cast<double>(SymbolSamples)) /
                            sampleRate;
                        double const center1 =
                            (static_cast<double>(symbol + 1) *
                                 static_cast<double>(SymbolSamples) +
                             0.5 * static_cast<double>(SymbolSamples)) /
                            sampleRate;
                        double const interceptGain =
                            -(symbolFrequency[symbol + 1] -
                              symbolFrequency[symbol]);
                        if (!(std::abs(interceptGain) > 1.0e-9))
                            continue;
                        double const slopeGain =
                            -(symbolFrequency[symbol + 1] * center1 -
                              symbolFrequency[symbol] * center0);
                        std::complex<double> const pairProduct =
                            timingCorrelations[symbol + 1] *
                            std::conj(timingCorrelations[symbol]);
                        double const phase = std::arg(pairProduct);
                        if (!std::isfinite(phase))
                            continue;

                        std::size_t const region = std::min(
                            regionCount - 1,
                            (regionCount * (2 * symbol + 1)) / (2 * Symbols));
                        pairs.push_back(TimingPhasePair{
                            center1 - center0, interceptGain, slopeGain, phase,
                            weight, 0.5 * (center0 + center1), region});
                        ++regionTransitionCounts[region];
                    }

                // Global seed line from the tone-transition pair phases. Absolute
                // channel phase cancels in adjacent-symbol differences. A small
                // residual carrier-frequency nuisance is retained diagnostically
                // but never applied; only the timing intercept/slope below can
                // alter the returned reference.
                double residualFrequency = 0.0;
                double seedIntercept = 0.0;
                double seedSlope = 0.0;
                bool phaseFit = false;
                for (int iteration = 0; iteration < 8; ++iteration) {
                    double normal[3][3] = {};
                    double right[3] = {};
                    for (auto const &pair : pairs) {
                        double const coordinates[3] = {pair.deltaTime,
                                                       pair.interceptGain,
                                                       pair.slopeGain};
                        double const error = wrapSicPhase(
                            pair.phase -
                            (residualFrequency * pair.deltaTime +
                             seedIntercept * pair.interceptGain +
                             seedSlope * pair.slopeGain));
                        for (int row = 0; row < 3; ++row) {
                            right[row] += pair.weight * coordinates[row] * error;
                            for (int column = 0; column < 3; ++column)
                                normal[row][column] += pair.weight *
                                                       coordinates[row] *
                                                       coordinates[column];
                        }
                    }

                    double step[3] = {};
                    if (!solveSicLinearSystem3(normal, right, step)) {
                        phaseFit = false;
                        break;
                    }
                    residualFrequency += step[0];
                    seedIntercept += step[1];
                    seedSlope += step[2];
                    phaseFit = true;
                }

                if (phaseFit) {
                    // Regional intercepts around the seed line. Each useful bin
                    // supplies one locally preferred timing offset, weighted by
                    // its tone-difference timing information. Low-informativeness
                    // bins cannot steer the result.
                    struct RegionTimingAccumulator {
                        std::size_t pairs = 0;
                        double offsetNumerator = 0.0;
                        double offsetDenominator = 0.0;
                        double timeNumerator = 0.0;
                    };
                    std::array<RegionTimingAccumulator, regionCount>
                        regionAccumulators{};
                    for (auto const &pair : pairs) {
                        double const error = wrapSicPhase(
                            pair.phase -
                            (residualFrequency * pair.deltaTime +
                             seedIntercept * pair.interceptGain +
                             seedSlope * pair.slopeGain));
                        auto &accumulator =
                            regionAccumulators[pair.region];
                        ++accumulator.pairs;
                        accumulator.offsetNumerator +=
                            pair.weight * pair.interceptGain * error;
                        accumulator.offsetDenominator +=
                            pair.weight * pair.interceptGain * pair.interceptGain;
                        accumulator.timeNumerator += pair.weight *
                                                     pair.interceptGain *
                                                     pair.interceptGain *
                                                     pair.centerTime;
                    }

                    struct RegionTimingPoint {
                        double time;
                        double offset;
                        double weight;
                    };
                    std::vector<RegionTimingPoint> points;
                    points.reserve(regionCount);
                    for (std::size_t region = 0; region < regionCount; ++region) {
                        if (regionTransitionCounts[region] < minRegionPairs)
                            continue;
                        auto const &accumulator = regionAccumulators[region];
                        if (accumulator.pairs < minRegionPairs ||
                            !(accumulator.offsetDenominator > 0.0) ||
                            !std::isfinite(accumulator.offsetNumerator) ||
                            !std::isfinite(accumulator.offsetDenominator) ||
                            !std::isfinite(accumulator.timeNumerator))
                            continue;

                        double const regionalCorrection =
                            accumulator.offsetNumerator /
                            accumulator.offsetDenominator;
                        double const regionalTime =
                            accumulator.timeNumerator /
                            accumulator.offsetDenominator;
                        points.push_back(RegionTimingPoint{
                            regionalTime,
                            seedIntercept + seedSlope * regionalTime +
                                regionalCorrection,
                            accumulator.offsetDenominator});
                    }

                    // Final weighted straight line through the regional timing
                    // offsets. This is the reported #5c timing model.
                    if (points.size() >= requiredRegions) {
                        double sw = 0.0;
                        double swt = 0.0;
                        double swtt = 0.0;
                        double swy = 0.0;
                        double swty = 0.0;
                        for (auto const &point : points) {
                            sw += point.weight;
                            swt += point.weight * point.time;
                            swtt += point.weight * point.time * point.time;
                            swy += point.weight * point.offset;
                            swty += point.weight * point.time * point.offset;
                        }
                        double const determinant = sw * swtt - swt * swt;
                        double const solutionScale =
                            std::max({1.0, std::abs(sw), std::abs(swt),
                                      std::abs(swtt)});
                        if (std::isfinite(determinant) &&
                            std::abs(determinant) >
                                1.0e-12 * solutionScale * solutionScale) {
                            double const finalIntercept =
                                (swy * swtt - swt * swty) / determinant;
                            double const finalSlope =
                                (sw * swty - swt * swy) / determinant;
                            double const endToEnd =
                                finalSlope * frameSeconds;

                            double residualSum = 0.0;
                            for (auto const &point : points) {
                                double const prediction =
                                    finalIntercept + finalSlope * point.time;
                                double const error = point.offset - prediction;
                                residualSum += point.weight * error * error;
                            }
                            double const fitRMS = std::sqrt(residualSum / sw);
                            double const timeSpread =
                                swtt - swt * swt / sw;
                            double const residualVariance =
                                residualSum / static_cast<double>(
                                                  std::max<std::size_t>(
                                                      1, points.size() - 2));
                            double const slopeVariance =
                                (timeSpread > 0.0 &&
                                 std::isfinite(residualVariance))
                                    ? residualVariance / timeSpread
                                    : std::numeric_limits<double>::quiet_NaN();
                            double const slopeStd =
                                std::sqrt(slopeVariance);
                            bool const slopeSignificant =
                                (slopeStd > 0.0 &&
                                 std::abs(finalSlope) >=
                                     4.0 * slopeStd) ||
                                (slopeStd == 0.0 && finalSlope != 0.0);

                            // Keep every fitted degree of the warp tightly
                            // bounded: residual intercept near the accepted #5b
                            // integer alignment, drift within the end-to-end
                            // budget, a coherent regional trend, a plausible
                            // one-Hz-equivalent residual-frequency nuisance,
                            // and a significant slope. All of these thresholds
                            // deserve later benchmarking against captured
                            // signals.
                            double const totalIntercept =
                                static_cast<double>(
                                    result.timingAdjustSamples) +
                                finalIntercept;
                            // A continuous correction can revisit the accepted
                            // #5b integer alignment, but it must stay inside the
                            // #5b integer search envelope. Without an accepted
                            // integer move, only a sub-sample correction is
                            // allowed.
                            double const interceptBound =
                                (result.timingAdjustSamples == 0)
                                    ? 1.0
                                    : static_cast<double>(searchRadius);
                            bool const acceptedFit =
                                std::isfinite(finalIntercept) &&
                                std::isfinite(finalSlope) &&
                                std::isfinite(fitRMS) &&
                                std::abs(totalIntercept) <= interceptBound &&
                                std::abs(endToEnd) <= maxEndToEndDrift &&
                                fitRMS <= 0.05 &&
                                std::abs(residualFrequency) <= twoPi &&
                                slopeSignificant;
                            if (acceptedFit) {
                                auto warped = applySicTimingWarp(
                                    result.reference, totalIntercept,
                                    finalSlope, sampleRate);

                                // Keep the warp only when it improves the summed
                                // transition correlation without materially
                                // degrading the whole-frame #5a metric. The
                                // strict transition improvement avoids replacing
                                // a better #5b reference over numerical noise.
                                double const warpedTransition =
                                    transitionMetricOf(warped, nominalStart)
                                        .first;
                                double const warpedWhole =
                                    metricFor(warped, nominalStart, 1);
                                if (std::isfinite(warpedTransition) &&
                                    warpedTransition > baseTWM &&
                                    std::isfinite(warpedWhole) &&
                                    warpedWhole >= baseWhole * 0.999) {
                                    result.reference = std::move(warped);
                                    result.timingDriftSamplesPerSecond =
                                        finalSlope;
                                    result.timingEndToEndSamples = endToEnd;
                                    result.timingFitRmsSamples = fitRMS;
                                    result.timingRegions =
                                        static_cast<int>(points.size());
                                    result.timingRefined = true;
                                }
                            }
                        }
                    }
                }
                }
            }
        }
    }

    // subtractjs8() still receives the decoder's nominal dt. Encode the small
    // accepted timing correction into the returned reference itself, keeping
    // the vector length unchanged so the existing #5a symbol accounting stays
    // bounded. Samples shifted off either edge are simply zeroed. Skip this
    // integer shift entirely once a continuous warp has been baked above, since
    // that warp already encodes the global integer optimum.
    if (!result.timingRefined && result.timingAdjustSamples > 0) {
        std::size_t const shift = std::min(
            static_cast<std::size_t>(result.timingAdjustSamples),
            result.reference.size());
        std::move_backward(result.reference.begin(),
                           result.reference.end() - shift,
                           result.reference.end());
        std::fill(result.reference.begin(), result.reference.begin() + shift,
                  std::complex<float>{0.0f, 0.0f});
    } else if (!result.timingRefined && result.timingAdjustSamples < 0) {
        std::size_t const shift = std::min(
            static_cast<std::size_t>(-result.timingAdjustSamples),
            result.reference.size());
        std::move(result.reference.begin() + shift, result.reference.end(),
                  result.reference.begin());
        std::fill(result.reference.end() - shift, result.reference.end(),
                  std::complex<float>{0.0f, 0.0f});
    }

    double const finalMetric = metricFor(result.reference, nominalStart, 1);
    if (!std::isfinite(finalMetric) ||
        finalMetric < result.nominalMetric * 0.999) {
        // Keep #5b strictly additive: discard a refinement whose final shifted
        // waveform measurably degrades the decoded-signal correlation metric.
        result.reference = originalReference;
        result.timingAdjustSamples = 0;
        result.frequencyOffsetHz = 0.0f;
        result.driftHzPerSecond = 0.0f;
        result.refinedMetric = result.nominalMetric;
        result.timingDriftSamplesPerSecond = 0.0;
        result.timingEndToEndSamples = 0.0;
        result.timingFitRmsSamples = 0.0;
        result.timingRegions = 0;
        result.timingRefined = false;
        result.refined = false;
        return result;
    }

    result.refinedMetric = metricFor(result.reference, nominalStart, 1);
    result.refined = result.timingAdjustSamples != 0 ||
                     result.frequencyOffsetHz != 0.0f ||
                     result.driftHzPerSecond != 0.0f ||
                     result.timingRefined;
    return result;
}
} // namespace js8

// JS8.cpp currently defines genjs8refsig() and invokes it inline as the first
// argument to subtractjs8(). Dispatch on macro argument count lets us leave the
// member definition structurally unchanged (its std::array<int, NN> parameter
// is seen as three preprocessor arguments because of the template comma) while
// wrapping the ordinary two-argument call with post-decode refinement.
#define JS8_SIC_GENREF_SELECT(_1, _2, _3, NAME, ...) NAME
#define JS8_SIC_GENREF_DEFINITION(_1, _2, _3) \
    genjs8refsigRaw(_1, _2, _3)
#define JS8_SIC_GENREF_CALL(_itone, _f0)                                      \
    ([&]() {                                                                  \
        auto sicReference = genjs8refsigRaw(_itone, _f0);                    \
        if (std::getenv("JS8_DISABLE_SIC_REFINEMENT") != nullptr)            \
            return sicReference;                                              \
        auto const allowTimingDrift =                                         \
            std::getenv("JS8_DISABLE_SIC_TIMING_DRIFT") == nullptr;          \
        auto sicRefinement =                                                  \
            ::js8::refineSicReference<Mode::NSPS, NN>(                       \
                std::move(sicReference), dd, _itone, xdt2, allowTimingDrift);\
        if (decoder_js8().isDebugEnabled()) {                                \
            double const metricGainDb =                                      \
                sicRefinement.nominalMetric > 0.0 &&                         \
                        sicRefinement.refinedMetric > 0.0                    \
                    ? 10.0 * std::log10(sicRefinement.refinedMetric /        \
                                        sicRefinement.nominalMetric)          \
                    : 0.0;                                                   \
            qCDebug(decoder_js8)                                             \
                << "SIC refinement"                                         \
                << "timingSamples"                                          \
                << sicRefinement.timingAdjustSamples                         \
                << "frequencyOffsetHz"                                      \
                << sicRefinement.frequencyOffsetHz                           \
                << "driftHzPerSecond"                                       \
                << sicRefinement.driftHzPerSecond                            \
                << "phasePairs" << sicRefinement.phasePairs                 \
                << "phaseFitRmsHz" << sicRefinement.phaseFitRmsHz           \
                << "timingDriftSamplesPerSecond"                            \
                << sicRefinement.timingDriftSamplesPerSecond               \
                << "timingEndToEndSamples"                                 \
                << sicRefinement.timingEndToEndSamples                     \
                << "timingFitRmsSamples"                                   \
                << sicRefinement.timingFitRmsSamples                       \
                << "timingRegions" << sicRefinement.timingRegions          \
                << "metricGainDb" << metricGainDb                           \
                << "accepted" << sicRefinement.refined;                     \
        }                                                                    \
        return std::move(sicRefinement.reference);                           \
    }())
#define genjs8refsig(...)                                                     \
    JS8_SIC_GENREF_SELECT(__VA_ARGS__, JS8_SIC_GENREF_DEFINITION,            \
                          JS8_SIC_GENREF_CALL, JS8_SIC_GENREF_UNUSED)          \
    (__VA_ARGS__)
