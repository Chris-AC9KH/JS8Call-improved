// Deterministic validation for the coherent JS8 data-symbol likelihood path.
//
// Covers:
//   1. perfect phase-coherent signal (fit accurate, correct tone dominates)
//   2. known residual frequency offset (sign + magnitude)
//   3. known linear frequency drift (sign + magnitude)
//   4. random absolute carrier phase
//   5. incoherent pilot phases (mandatory noncoherent fallback)
//   6. all five JS8 symbol lengths (384/600/1200/1920/3840 samples)
//   7. NaN/Inf inputs (no propagation into scores or blend weight)
//   8. disabled path: WhiteningProcessor output bit-identical to legacy
//   9. reliability preserved: strong symbols outrank weak ones (no per-symbol
//      unit-variance normalization), alpha continuity at 0/0.01/1
//  10. tracker phase compensation using the real FrequencyTracker code and
//      decoder-style DFTs (frequency sign, timing offsets x tones 0/3/7)
//
// Build/run (Qt6Core is needed only for the whitening fallback check):
//   clang++ -std=c++20 -O2 -I.. $(pkg-config --cflags Qt6Core) \
//       tools/coherent_likelihood_test.cpp ../JS8_Mode/FrequencyTracker.cpp \
//       $(pkg-config --libs Qt6Core) \
//       -o /tmp/coherent_likelihood_test && /tmp/coherent_likelihood_test

#include "../JS8_Mode/coherent_likelihood.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8");

#include "../JS8_Mode/FrequencyTracker.h"
#include "../JS8_Mode/whitening_processor.h"

namespace {

int failures = 0;

void check(bool cond, char const *what) {
    if (cond) {
        std::printf("  PASS: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

double phaseError(double actual, double expected) {
    double const halfTurn = std::numbers::pi;
    double wrapped = std::fmod(actual - expected + halfTurn, 2.0 * halfTurn);
    if (wrapped < 0.0)
        wrapped += 2.0 * halfTurn;
    return std::abs(wrapped - halfTurn);
}

constexpr double kSampleRate = 12000.0;

// Costas pilot symbol positions in a 79-symbol frame.
std::vector<int> pilotSymbols() {
    std::vector<int> symbols;
    for (int block = 0; block < 3; ++block)
        for (int column = 0; column < 7; ++column)
            symbols.push_back(block * 36 + column);
    return symbols;
}

// Synthesize pilot observations for a known carrier model.
std::vector<js8::CoherentPilot>
makePilots(double phi0, double deltaF, double fdot, double symbolSeconds,
           double amplitude, double phaseNoiseRad, std::mt19937 &rng) {
    std::vector<js8::CoherentPilot> pilots;
    std::normal_distribution<double> noise(0.0, phaseNoiseRad);
    for (int symbol : pilotSymbols()) {
        double const t = symbol * symbolSeconds;
        double phase = phi0 + 2.0 * std::numbers::pi * deltaF * t +
                       std::numbers::pi * fdot * t * t;
        if (phaseNoiseRad > 0.0)
            phase += noise(rng);
        pilots.push_back(
            {t, amplitude * std::polar(1.0, phase), symbol % 8, symbol,
             0.0, 0.0});
    }
    return pilots;
}

// Synthesize one data symbol's 8 complex tone bins: energy on `trueTone`.
std::array<std::complex<float>, 8>
makeDataBins(int trueTone, double phase, double amplitude, double noiseSigma,
             std::mt19937 &rng) {
    std::normal_distribution<double> noise(0.0, noiseSigma / std::sqrt(2.0));
    std::array<std::complex<float>, 8> bins{};
    for (int tone = 0; tone < 8; ++tone) {
        std::complex<double> value{noise(rng), noise(rng)};
        if (tone == trueTone)
            value += amplitude * std::polar(1.0, phase);
        bins[static_cast<std::size_t>(tone)] =
            std::complex<float>{static_cast<float>(value.real()),
                                static_cast<float>(value.imag())};
    }
    return bins;
}

// Decoder-style forward DFT bin (FFTW_FORWARD sign convention).
std::complex<double> dftBin(std::vector<std::complex<float>> const &samples,
                            int tone) {
    std::complex<double> acc{};
    int const count = static_cast<int>(samples.size());
    for (int n = 0; n < count; ++n) {
        double const angle =
            -2.0 * std::numbers::pi * tone * n / count;
        acc += std::complex<double>{samples[static_cast<std::size_t>(n)].real(),
                                    samples[static_cast<std::size_t>(n)].imag()} *
               std::complex<double>{std::cos(angle), std::sin(angle)};
    }
    return acc;
}

void runPerfectSignal() {
    std::printf("[perfect coherent signal]\n");
    std::mt19937 rng(11);
    auto pilots = makePilots(0.7, 0.0, 0.0, 0.16, 2.0, 0.0, rng);
    auto fit = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
    check(fit.fitted, "perfect signal fits");
    check(std::abs(fit.deltaF) < 0.02, "residual frequency near zero");
    check(std::abs(fit.fdot) < 0.005, "residual drift near zero");
    check(phaseError(fit.phi0, 0.7) < 0.05, "absolute phase recovered");
    check(fit.pilotCount == 21, "all 21 pilots used");
    check(js8::coherentBlendWeight(fit) == 1.0, "full coherent weight");
    check(std::abs(fit.amplitude - 2.0) < 1.0e-6, "pilot amplitude recovered");

    // Correct tone dominates the coherent numerators on every data symbol.
    bool dominates = true;
    std::vector<js8::CoherentDataSymbol> data;
    for (int symbol = 0; symbol < 58; ++symbol) {
        int const trueTone = symbol % 8;
        double const t = (7 + symbol) * 0.16;
        js8::CoherentDataSymbol entry;
        entry.timeSeconds = t;
        entry.bins =
            makeDataBins(trueTone, js8::predictCarrierPhase(fit, t), 2.0,
                         0.05, rng);
        data.push_back(entry);
    }
    auto result = js8::computeCoherentToneScores(pilots, data, 12, 5.0, 32,
                                                 375.0);
    check(result.telemetry.enabled, "coherent scores enabled");
    check(result.alpha == 1.0, "telemetry reports full weight");
    for (std::size_t symbol = 0; symbol < data.size() && dominates; ++symbol) {
        int const trueTone = static_cast<int>(symbol) % 8;
        auto const &scores = result.coherentNumerators[symbol];
        auto margin = js8::topTwoMargin(scores);
        if (!margin || *margin <= 0.0)
            dominates = false;
        float best = scores[0];
        int winner = 0;
        for (int tone = 1; tone < 8; ++tone)
            if (scores[static_cast<std::size_t>(tone)] > best) {
                best = scores[static_cast<std::size_t>(tone)];
                winner = tone;
            }
        if (winner != trueTone)
            dominates = false;
    }
    check(dominates, "correct tone dominates all data symbols");
}

void runFrequencyOffset() {
    std::printf("[residual frequency offset]\n");
    std::mt19937 rng(12);
    for (double deltaF : {0.25, -0.25}) {
        auto pilots = makePilots(0.0, deltaF, 0.0, 0.16, 2.0, 0.0, rng);
        auto fit = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
        check(fit.fitted, "offset fit accepted");
        check((fit.deltaF > 0.0) == (deltaF > 0.0), "offset sign recovered");
        check(std::abs(fit.deltaF - deltaF) < 0.03,
              "offset magnitude recovered");
    }
}

void runFrequencyDrift() {
    std::printf("[linear frequency drift]\n");
    std::mt19937 rng(13);
    for (double fdot : {0.02, -0.02}) {
        auto pilots = makePilots(0.0, 0.0, fdot, 0.16, 2.0, 0.0, rng);
        auto fit = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
        check(fit.fitted, "drift fit accepted");
        check((fit.fdot > 0.0) == (fdot > 0.0), "drift sign recovered");
        check(std::abs(fit.fdot - fdot) < 0.2 * std::abs(fdot) + 0.003,
              "drift magnitude recovered");
    }
}

void runRandomPhase() {
    std::printf("[random absolute carrier phase]\n");
    std::mt19937 rng(14);
    std::uniform_real_distribution<double> uniform(-std::numbers::pi,
                                                   std::numbers::pi);
    for (int trial = 0; trial < 4; ++trial) {
        double const phi0 = uniform(rng);
        // Zero frequency offset here so the absolute phase is
        // time-independent and directly comparable.
        auto pilots = makePilots(phi0, 0.0, 0.0, 0.16, 2.0, 0.0, rng);
        auto fit = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
        check(fit.fitted, "random-phase fit accepted");
        check(phaseError(fit.phi0, phi0) < 0.05, "absolute phase recovered");
    }
}

void runIncoherentPilots() {
    std::printf("[incoherent pilot phases]\n");
    std::mt19937 rng(15);
    std::uniform_real_distribution<double> uniform(-std::numbers::pi,
                                                   std::numbers::pi);
    std::vector<js8::CoherentPilot> pilots;
    for (int symbol : pilotSymbols()) {
        double const t = symbol * 0.16;
        pilots.push_back({t, 2.0 * std::polar(1.0, uniform(rng)), symbol % 8,
                          symbol, 0.0, 0.0});
    }
    auto fit = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
    double const alpha = js8::coherentBlendWeight(fit);
    check(alpha == 0.0, "incoherent pilots force noncoherent fallback");
    check(!fit.fitted || fit.rmsRad > 0.35, "poor fit rejected or very loose");
}

void runAllSymbolLengths() {
    std::printf("[all mode symbol lengths]\n");
    std::mt19937 rng(16);
    // (symbol samples, downsampled window, downsampled rate)
    constexpr std::array<std::array<int, 3>, 5> modes = {{
        {{384, 12, 375}}, {{600, 12, 240}}, {{1200, 20, 200}},
        {{1920, 32, 200}}, {{3840, 32, 100}},
    }};
    for (auto const &mode : modes) {
        double const symbolSeconds =
            mode[0] / static_cast<double>(kSampleRate);
        auto pilots =
            makePilots(1.1, 0.05, 0.0, symbolSeconds, 2.0, 0.0, rng);
        auto fit = js8::fitCarrierPhase(pilots, 12, 1.0, mode[1],
                                        static_cast<double>(mode[2]));
        bool finite = std::isfinite(fit.phi0) && std::isfinite(fit.deltaF) &&
                      std::isfinite(fit.fdot) && std::isfinite(fit.rmsRad);
        check(fit.fitted && finite, "mode length fits without NaN/Inf");
        check(std::abs(fit.deltaF - 0.05) < 0.03, "offset recovered per mode");
    }
}

void runNonFiniteInputs() {
    std::printf("[NaN/Inf inputs]\n");
    std::mt19937 rng(17);
    // A few corrupt pilots are filtered; the fit must stay finite.
    auto pilots = makePilots(0.0, 0.0, 0.0, 0.16, 2.0, 0.0, rng);
    pilots[3].value = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    pilots[9].timeSeconds = std::numeric_limits<double>::infinity();
    auto filtered = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
    check(std::isfinite(js8::coherentBlendWeight(filtered)),
          "blend weight stays finite with corrupt pilots");
    check(filtered.pilotCount == 19, "corrupt pilots excluded from count");
    // All pilots corrupt: mandatory fallback, finite weight.
    for (auto &pilot : pilots)
        pilot.value = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    auto fit = js8::fitCarrierPhase(pilots, 12, 5.0, 32, 375.0);
    double const alpha = js8::coherentBlendWeight(fit);
    check(!fit.fitted, "all-corrupt pilots cannot fit");
    check(alpha == 0.0, "non-finite pilots force fallback");
    check(std::isfinite(alpha), "blend weight stays finite");

    std::vector<js8::CoherentDataSymbol> data(2);
    data[0].timeSeconds = 0.0;
    data[1].timeSeconds = 0.16;
    data[0].bins[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
    auto result = js8::computeCoherentToneScores(
        pilots, data, 12, 5.0, 32, 375.0);
    check(result.coherentNumerators.empty(), "no scores from invalid bins");
    check(result.alpha == 0.0, "no weight from invalid bins");
}

void runDisabledFallbackExact() {
    std::printf("[disabled path matches legacy LLRs]\n");
    std::mt19937 rng(18);
    std::uniform_real_distribution<float> uniform(0.0f, 3.0f);
    std::array<std::array<float, 58>, 8> s1{};
    for (auto &row : s1)
        for (auto &value : row)
            value = uniform(rng);
    std::array<int, 58> winners{};
    for (int j = 0; j < 58; ++j)
        winners[static_cast<std::size_t>(j)] = j % 8;

    auto const baseline =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false);
    js8::CoherentBlend<8, 58> disabled{};
    disabled.alpha = 0.0f;
    auto const disabledExplicit =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     disabled);
    check(disabledExplicit.llr0 == baseline.llr0 &&
              disabledExplicit.llr1 == baseline.llr1,
          "explicit disable matches legacy LLRs exactly");
}

// Reliability: a strong symbol must outrank a weak one after blending.
// Per-symbol unit-variance normalization would destroy this relationship.
void runReliabilityPreserved() {
    std::printf("[strong vs weak symbol reliability]\n");
    std::array<std::array<float, 58>, 8> s1{};
    for (auto &row : s1)
        row.fill(0.1f);
    s1[2][5] = 2.0f;  // strong symbol, winner tone 2
    s1[5][10] = 0.3f; // weak symbol, winner tone 5
    std::array<int, 58> winners{};
    for (int j = 0; j < 58; ++j) {
        float best = s1[0][j];
        int winner = 0;
        for (int i = 1; i < 8; ++i)
            if (s1[i][j] > best) {
                best = s1[i][j];
                winner = i;
            }
        winners[static_cast<std::size_t>(j)] = winner;
    }
    // Coherent numerators consistent with amplitude 1 and aligned phases:
    // numerator = 2*A*projection - A*A with projection = bin magnitude.
    js8::CoherentBlend<8, 58> blend;
    blend.amplitude = 1.0;
    blend.alpha = 1.0f;
    for (int tone = 0; tone < 8; ++tone)
        for (int j = 0; j < 58; ++j) {
            float const projection = s1[tone][j];
            blend.numerators[static_cast<std::size_t>(tone)]
                            [static_cast<std::size_t>(j)] =
                                2.0f * 1.0f * projection - 1.0f;
        }
    auto const result =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     blend);
    auto symbolStrength = [&](int symbol) {
        float m = 0.0f;
        for (int b = 0; b < 3; ++b)
            m = std::max(m, std::abs(result.llr0[3 * symbol + b]));
        return m;
    };
    float const strong = symbolStrength(5);
    float const weak = symbolStrength(10);
    check(strong > 3.0f * weak, "strong symbol LLR dominates weak symbol");
}

// Alpha continuity: alpha=0 reproduces noncoherent output exactly, and a tiny
// alpha stays genuinely close instead of renormalizing the whole symbol.
void runAlphaContinuity() {
    std::printf("[alpha continuity]\n");
    std::mt19937 rng(19);
    std::uniform_real_distribution<float> uniform(0.0f, 3.0f);
    std::array<std::array<float, 58>, 8> s1{};
    for (auto &row : s1)
        for (auto &value : row)
            value = uniform(rng);
    std::array<int, 58> winners{};
    for (int j = 0; j < 58; ++j)
        winners[static_cast<std::size_t>(j)] = j % 8;
    js8::CoherentBlend<8, 58> blend;
    blend.amplitude = 1.0;
    for (int tone = 0; tone < 8; ++tone)
        for (int j = 0; j < 58; ++j)
            blend.numerators[static_cast<std::size_t>(tone)]
                            [static_cast<std::size_t>(j)] =
                                8.0f - tone - 0.01f * j;

    auto const baseline =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false);
    blend.alpha = 0.0f;
    auto const atZero =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     blend);
    check(atZero.llr0 == baseline.llr0 && atZero.llr1 == baseline.llr1,
          "alpha=0 reproduces noncoherent output exactly");

    blend.alpha = 0.01f;
    auto const atTiny =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     blend);
    float maxBase = 0.0f, maxDiff = 0.0f;
    for (std::size_t i = 0; i < baseline.llr0.size(); ++i) {
        maxBase = std::max(maxBase, std::abs(baseline.llr0[i]));
        maxDiff = std::max(maxDiff,
                           std::abs(atTiny.llr0[i] - baseline.llr0[i]));
    }
    check(maxDiff < 0.05f * maxBase, "alpha=0.01 stays close to noncoherent");

    blend.alpha = 1.0f;
    auto const atOne =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     blend);
    check(atOne.llr0 != baseline.llr0, "alpha=1 uses coherent evidence");

    // NaN coherent data can never corrupt the fallback: any non-finite
    // numerator disables blending frame-wide, so the output matches the
    // legacy path exactly and no NaN reaches the LLRs.
    blend.alpha = 0.5f;
    blend.numerators[3][7] = std::numeric_limits<float>::quiet_NaN();
    auto const nanSafe =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     blend);
    check(nanSafe.llr0 == baseline.llr0 && nanSafe.llr1 == baseline.llr1,
          "NaN coherent data falls back exactly");
    bool allFinite = true;
    for (float value : nanSafe.llr0)
        allFinite = allFinite && std::isfinite(value);
    for (float value : nanSafe.llr1)
        allFinite = allFinite && std::isfinite(value);
    check(allFinite, "no NaN propagates into LLRs");
}

// Tracker phase compensation with the real FrequencyTracker implementation
// and decoder-style DFTs.
void runFrequencyTrackerCompensation() {
    std::printf("[FrequencyTracker phase compensation]\n");
    constexpr int window = 32;
    constexpr double rateHz = 375.0;
    for (double trackerHz : {2.5, -2.5}) {
        js8::FrequencyTracker tracker;
        tracker.reset(trackerHz, rateHz);
        std::vector<std::complex<float>> samples(
            static_cast<std::size_t>(window));
        double const phi0 = 0.7;
        int const tone = 3;
        for (int n = 0; n < window; ++n) {
            double const phase =
                2.0 * std::numbers::pi * tone * n / window + phi0;
            samples[static_cast<std::size_t>(n)] = std::polar(
                1.0f, static_cast<float>(phase));
        }
        // Exactly the per-symbol correction used by the decoder.
        tracker.apply(samples.data(), window);
        std::complex<double> const corrected =
            js8::normalizeFrequencyTrackerPhase(dftBin(samples, tone),
                                                trackerHz, rateHz, window);
        check(phaseError(std::arg(corrected), phi0) < 1.0e-3,
              "tracker phase removed, carrier phase recovered");
    }
}

// Timing displacement compensation across representative tones.
void runTimingTrackerCompensation() {
    std::printf("[TimingTracker phase compensation]\n");
    constexpr int window = 32;
    for (int tone : {0, 3, 7}) {
        double const phi0 = 1.1;
        std::vector<std::complex<float>> line(
            static_cast<std::size_t>(window + 8));
        for (int n = -4; n < window + 4; ++n) {
            double const phase =
                2.0 * std::numbers::pi * tone * n / window + phi0;
            line[static_cast<std::size_t>(n + 4)] = std::polar(
                1.0f, static_cast<float>(phase));
        }
        bool independent = true;
        for (int shift : {-2, -1, 0, 1, 2}) {
            std::vector<std::complex<float>> windowSamples(
                static_cast<std::size_t>(window));
            for (int n = 0; n < window; ++n)
                windowSamples[static_cast<std::size_t>(n)] =
                    line[static_cast<std::size_t>(n + shift + 4)];
            std::complex<double> const bin = dftBin(windowSamples, tone);
            std::complex<double> const corrected =
                js8::normalizeTimingPhase(bin, tone, shift, window);
            if (phaseError(std::arg(corrected), phi0) >= 1.0e-3)
                independent = false;
        }
        check(independent, "recovered phase independent of timing offset");
    }
}

} // namespace

int main() {
    runPerfectSignal();
    runFrequencyOffset();
    runFrequencyDrift();
    runRandomPhase();
    runIncoherentPilots();
    runAllSymbolLengths();
    runNonFiniteInputs();
    runDisabledFallbackExact();
    runReliabilityPreserved();
    runAlphaContinuity();
    runFrequencyTrackerCompensation();
    runTimingTrackerCompensation();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED"
                                                       : "TESTS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
