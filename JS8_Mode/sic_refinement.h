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
};

template <int SymbolSamples, std::size_t Symbols, typename Samples, typename Tones>
SicRefinementResult refineSicReference(
    std::vector<std::complex<float>> nominal, Samples const &samples,
    Tones const &tones, float const dt) {
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

    auto const transitionMetric = [&](int const nstart) {
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
                        static_cast<long long>(result.reference.size()) ||
                    sampleIndex < 0 ||
                    sampleIndex >= static_cast<long long>(samples.size()))
                    continue;

                auto const r = std::complex<double>{
                    result.reference[static_cast<std::size_t>(referenceIndex)].real(),
                    result.reference[static_cast<std::size_t>(referenceIndex)].imag()};
                correlation +=
                    static_cast<double>(samples[static_cast<std::size_t>(sampleIndex)]) *
                    std::conj(r);
            }
            metric += std::norm(correlation);
            ++transitions;
        }
        return std::pair<double, int>{metric, transitions};
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

    // subtractjs8() still receives the decoder's nominal dt. Encode the small
    // accepted timing correction into the returned reference itself, keeping
    // the vector length unchanged so the existing #5a symbol accounting stays
    // bounded. Samples shifted off either edge are simply zeroed.
    if (result.timingAdjustSamples > 0) {
        std::size_t const shift = std::min(
            static_cast<std::size_t>(result.timingAdjustSamples),
            result.reference.size());
        std::move_backward(result.reference.begin(),
                           result.reference.end() - shift,
                           result.reference.end());
        std::fill(result.reference.begin(), result.reference.begin() + shift,
                  std::complex<float>{0.0f, 0.0f});
    } else if (result.timingAdjustSamples < 0) {
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
        result.refined = false;
        return result;
    }

    result.refinedMetric = metricFor(result.reference, nominalStart, 1);
    result.refined = result.timingAdjustSamples != 0 ||
                     result.frequencyOffsetHz != 0.0f ||
                     result.driftHzPerSecond != 0.0f;
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
        auto sicRefinement =                                                  \
            ::js8::refineSicReference<Mode::NSPS, NN>(                       \
                std::move(sicReference), dd, _itone, xdt2);                  \
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
                << "metricGainDb" << metricGainDb                           \
                << "accepted" << sicRefinement.refined;                     \
        }                                                                    \
        return std::move(sicRefinement.reference);                           \
    }())
#define genjs8refsig(...)                                                     \
    JS8_SIC_GENREF_SELECT(__VA_ARGS__, JS8_SIC_GENREF_DEFINITION,            \
                          JS8_SIC_GENREF_CALL, JS8_SIC_GENREF_UNUSED)          \
    (__VA_ARGS__)
