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
//
// Build/run (Qt6Core is needed only for the whitening fallback check):
//   clang++ -std=c++20 -O2 -I.. $(pkg-config --cflags Qt6Core) \
//       tools/coherent_likelihood_test.cpp $(pkg-config --libs Qt6Core) \
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

// Synthesize pilot observations for a known carrier model. `phaseNoiseRad`
// adds Gaussian phase noise; `dropFraction` randomly zeroes weights.
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
            {t, amplitude * std::polar(1.0, phase), symbol % 8, symbol});
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

void runPerfectSignal() {
    std::printf("[perfect coherent signal]\n");
    std::mt19937 rng(11);
    auto pilots = makePilots(0.7, 0.0, 0.0, 0.16, 2.0, 0.0, rng);
    auto fit = js8::fitCarrierPhase(pilots, 12, 5.0);
    check(fit.fitted, "perfect signal fits");
    check(std::abs(fit.deltaF) < 0.02, "residual frequency near zero");
    check(std::abs(fit.fdot) < 0.005, "residual drift near zero");
    check(phaseError(fit.phi0, 0.7) < 0.05, "absolute phase recovered");
    check(fit.pilotCount == 21, "all 21 pilots used");
    check(js8::coherentBlendWeight(fit) == 1.0, "full coherent weight");

    // Correct tone dominates the coherent scores on every data symbol.
    bool dominates = true;
    for (int symbol = 0; symbol < 58; ++symbol) {
        int const trueTone = symbol % 8;
        double const t = (7 + symbol) * 0.16;
        auto bins = makeDataBins(trueTone, js8::predictCarrierPhase(fit, t),
                                 2.0, 0.05, rng);
        std::array<float, 8> scores{};
        for (std::size_t tone = 0; tone < 8; ++tone) {
            std::complex<double> const bin{bins[tone].real(),
                                           bins[tone].imag()};
            scores[tone] = static_cast<float>(
                (bin * std::exp(std::complex<double>{
                           0.0, -js8::predictCarrierPhase(fit, t)}))
                    .real());
        }
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
        auto fit = js8::fitCarrierPhase(pilots, 12, 5.0);
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
        auto fit = js8::fitCarrierPhase(pilots, 12, 5.0);
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
        auto fit = js8::fitCarrierPhase(pilots, 12, 5.0);
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
                          symbol});
    }
    auto fit = js8::fitCarrierPhase(pilots, 12, 5.0);
    double const alpha = js8::coherentBlendWeight(fit);
    check(alpha == 0.0, "incoherent pilots force noncoherent fallback");
    check(!fit.fitted || fit.rmsRad > 0.35, "poor fit rejected or very loose");
}

void runAllSymbolLengths() {
    std::printf("[all mode symbol lengths]\n");
    std::mt19937 rng(16);
    for (int nsps : {384, 600, 1200, 1920, 3840}) {
        double const symbolSeconds = nsps / kSampleRate;
        auto pilots = makePilots(1.1, 0.05, 0.0, symbolSeconds, 2.0, 0.0, rng);
        auto fit = js8::fitCarrierPhase(pilots, 12, 1.0);
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
    auto filtered = js8::fitCarrierPhase(pilots, 12, 5.0);
    check(std::isfinite(js8::coherentBlendWeight(filtered)),
          "blend weight stays finite with corrupt pilots");
    check(filtered.pilotCount == 19, "corrupt pilots excluded from count");
    // All pilots corrupt: mandatory fallback, finite weight.
    for (auto &pilot : pilots)
        pilot.value = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    auto fit = js8::fitCarrierPhase(pilots, 12, 5.0);
    double const alpha = js8::coherentBlendWeight(fit);
    check(!fit.fitted, "all-corrupt pilots cannot fit");
    check(alpha == 0.0, "non-finite pilots force fallback");
    check(std::isfinite(alpha), "blend weight stays finite");

    std::vector<std::array<std::complex<float>, 8>> bins(
        2, std::array<std::complex<float>, 8>{});
    bins[0][0] = {std::numeric_limits<float>::quiet_NaN(), 0.0f};
    auto result = js8::computeCoherentToneScores(
        pilots, bins, std::vector<double>{0.0, 0.16}, 12, 5.0);
    check(result.coherentScores.empty(), "no scores from invalid bins");
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
    auto const disabledExplicit =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     std::nullopt, 0.0f);
    std::array<std::array<float, 58>, 8> dummy{};
    auto const zeroAlpha =
        js8::WhiteningProcessor<8, 58, 174>::process(s1, winners, 0.0f, false,
                                                     dummy, 0.0f);
    check(disabledExplicit.llr0 == baseline.llr0 &&
              disabledExplicit.llr1 == baseline.llr1,
          "explicit disable matches legacy LLRs exactly");
    check(zeroAlpha.llr0 == baseline.llr0 && zeroAlpha.llr1 == baseline.llr1,
          "zero blend weight matches legacy LLRs exactly");

    // Blend helper itself: alpha<=0/NaN copies input; degenerate input falls
    // back; valid input blends.
    std::array<float, 8> noncoherent{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<float, 8> coherent{8, 7, 6, 5, 4, 3, 2, 1};
    std::array<float, 8> out{};
    check(!js8::blendToneScores(noncoherent, coherent, 0.0f, out) &&
              out == noncoherent,
          "blend alpha=0 copies noncoherent exactly");
    check(!js8::blendToneScores(noncoherent, coherent,
                                std::numeric_limits<float>::quiet_NaN(),
                                out) &&
              out == noncoherent,
          "blend NaN alpha copies noncoherent exactly");
    std::array<float, 8> flat{};
    flat.fill(2.0f);
    check(!js8::blendToneScores(noncoherent, flat, 1.0f, out) &&
              out == noncoherent,
          "blend degenerate coherent falls back exactly");
    check(js8::blendToneScores(noncoherent, coherent, 1.0f, out) &&
              out != noncoherent,
          "blend alpha=1 uses coherent evidence");
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

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED"
                                                       : "TESTS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
