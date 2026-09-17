// Deterministic unit tests for decoder-aided re-demodulation helpers.
//
// Covers: codeword->tones mapping (A), confidence weighting (B), baseline
// refinement (C), known timing error (D), known residual frequency (E),
// timing+frequency (F), wrong tentative codeword (G), bounds/clipping (H),
// NaN/Inf safety (I), plus a helper-level rescue demonstration.
//
// The synthetic 79-symbol waveform uses continuous FSK phase (boundary phase
// jumps are multiples of 2*pi) with absolute-time residual/drift terms, the
// same physical model the refinement search assumes.
//
// Build (no Qt/FFTW needed; header is dependency-free):
//   clang++ -std=c++20 -O2 -Wall -I. tools/decoder_aided_redemod_test.cpp \
//       -o /tmp/aided_test && /tmp/aided_test

#include "JS8_Mode/decoder_aided_redemod.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, char const *name) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s\n", name);
    } else {
        std::printf("  PASS: %s\n", name);
    }
}

// Costas arrays copied from JS8::Costas::array (JS8_Mode/JS8.h): ORIGINAL for
// Mode A, MODIFIED for the other modes.
constexpr js8::aided::CostasTones kOriginal = {{
    {{4, 2, 5, 6, 1, 3, 0}},
    {{4, 2, 5, 6, 1, 3, 0}},
    {{4, 2, 5, 6, 1, 3, 0}},
}};
constexpr js8::aided::CostasTones kModified = {{
    {{0, 6, 2, 3, 5, 4, 1}},
    {{1, 5, 0, 2, 3, 6, 4}},
    {{2, 5, 0, 6, 4, 1, 3}},
}};

constexpr int kWindow = 32;
constexpr double kRate = 200.0; // Mode-A-like downsampled rate.

// Continuous-phase 79-symbol FSK line. injShift displaces the true symbol
// timing relative to frameStart; resFreq/drift use absolute sample time, the
// same convention as the search derotation.
std::vector<std::complex<float>>
synthLine(std::array<int, 79> const &tones, int frameStart, int injShift,
          double startPhase, double resFreq, double drift) {
    constexpr int kPad = 16;
    int const total = frameStart + 79 * kWindow + kPad;
    std::vector<std::complex<float>> line(static_cast<std::size_t>(total));
    constexpr double twoPi = 2.0 * std::numbers::pi;
    for (int idx = 0; idx < total; ++idx) {
        long const rel = static_cast<long>(idx) - frameStart - injShift;
        long sym = rel >= 0 ? rel / kWindow : -(((-rel) + kWindow - 1) /
                                                kWindow);
        if (sym < 0)
            sym = 0;
        if (sym > 78)
            sym = 78;
        long const local = rel - sym * kWindow;
        double const t = static_cast<double>(idx) / kRate;
        double const phase =
            startPhase + twoPi * resFreq * t +
            std::numbers::pi * drift * t * t +
            twoPi * tones[static_cast<std::size_t>(sym)] * local / kWindow;
        line[static_cast<std::size_t>(idx)] =
            std::polar(1.0f, static_cast<float>(phase));
    }
    return line;
}

std::array<int, 79> tonesFromBits(std::array<int8_t, 174> const &cw) {
    std::array<int, 79> tones{};
    js8::aided::codewordToTones(cw, kOriginal, tones);
    return tones;
}

std::array<float, 79> unitDataWeights() {
    // Costas 1.0, all data 1.0 (full-confidence hypothesis).
    std::array<float, 79> w{};
    for (int k = 0; k < 79; ++k)
        w[static_cast<std::size_t>(k)] = 1.0f;
    return w;
}

void runCodewordToTones() {
    std::printf("[codeword to tones]\n");
    // All-zero codeword: all data tones 0, Costas exact.
    std::array<int8_t, 174> cw{};
    std::array<int, 79> tones{};
    js8::aided::codewordToTones(cw, kOriginal, tones);
    bool dataZero = true;
    for (int j = 0; j < 58; ++j)
        if (tones[static_cast<std::size_t>(js8::aided::dataSymbolIndex(j))] !=
            0)
            dataZero = false;
    check(dataZero, "all-zero codeword gives all-zero data tones");
    check(tones[0] == 4 && tones[6] == 0 && tones[36] == 4 &&
              tones[42] == 0 && tones[72] == 4 && tones[78] == 0,
          "ORIGINAL Costas positions exact");
    // Bit pattern -> tone: j=0 bits (1,0,1) -> 4+0+1 = 5 at global 7.
    cw[0] = 1;
    cw[1] = 0;
    cw[2] = 1;
    // j=57 bits (1,1,0) -> 4+2+0 = 6 at global 71.
    cw[171] = 1;
    cw[172] = 1;
    cw[173] = 0;
    // j=29 (first message-block symbol) bits (0,1,1) -> 3 at global 43.
    cw[87] = 0;
    cw[88] = 1;
    cw[89] = 1;
    js8::aided::codewordToTones(cw, kOriginal, tones);
    check(tones[7] == 5, "tone = 4*b0 + 2*b1 + b2 at global 7");
    check(tones[71] == 6, "last data symbol maps to global 71");
    check(tones[43] == 3, "message block starts at global 43");
    check(js8::aided::dataSymbolIndex(0) == 7 &&
              js8::aided::dataSymbolIndex(28) == 35 &&
              js8::aided::dataSymbolIndex(29) == 43 &&
              js8::aided::dataSymbolIndex(57) == 71,
          "data symbol layout j<29 -> j+7, else j+14");
    // MODIFIED array differs at block starts.
    js8::aided::codewordToTones(cw, kModified, tones);
    check(tones[0] == 0 && tones[36] == 1 && tones[72] == 2,
          "MODIFIED Costas positions exact");
    check(tones[7] == 5 && tones[71] == 6,
          "data tones independent of Costas type");
}

void runConfidenceWeighting() {
    std::printf("[confidence weighting]\n");
    std::array<float, 174> llr{};
    // Symbol 0: strong (min 4.0). Symbol 1: weak (min 0.2). Symbol 2: NaN.
    llr[0] = 4.0f;
    llr[1] = -5.0f;
    llr[2] = 6.0f;
    llr[3] = 0.2f;
    llr[4] = -0.4f;
    llr[5] = 0.9f;
    llr[6] = 1.0f;
    llr[7] = 2.0f;
    llr[8] = std::numeric_limits<float>::quiet_NaN();
    // Symbol 3: +Inf.
    llr[9] = 3.0f;
    llr[10] = std::numeric_limits<float>::infinity();
    llr[11] = -3.0f;
    auto const conf = js8::aided::tentativeConfidences(llr);
    check(conf.weights[0] == 1.0f, "high-confidence symbol gets weight 1");
    check(conf.weights[1] == 0.0f, "low-confidence symbol gets weight 0");
    check(conf.weights[2] == 0.0f, "NaN LLR gives zero weight");
    check(conf.weights[3] == 0.0f, "Inf LLR gives zero weight");
    check(conf.weights[0] > conf.weights[1],
          "strong symbol outweighs weak symbol");
    check(conf.used == 1, "only confident symbols counted used");
    check(std::isfinite(conf.mean) && conf.mean == 1.0,
          "mean over used symbols finite");
    // Mid confidence ramps monotonically.
    std::array<float, 174> ramp{};
    ramp[0] = ramp[1] = ramp[2] = 1.0f; // min 1.0
    ramp[3] = ramp[4] = ramp[5] = 2.0f; // min 2.0
    auto const c2 = js8::aided::tentativeConfidences(ramp);
    check(c2.weights[0] > 0.0f && c2.weights[0] < c2.weights[1] &&
              c2.weights[1] <= 1.0f,
          "confidence ramps monotonically with min|LLR|");
    // Weights expand to 79 symbols with Costas at 1.0.
    std::array<float, 79> w79{};
    js8::aided::buildSymbolWeights(conf, w79);
    check(w79[0] == 1.0f && w79[36] == 1.0f && w79[78] == 1.0f,
          "Costas symbols get weight 1.0");
    check(w79[7] == 1.0f && w79[8] == 0.0f && w79[43] == 0.0f,
          "data weights land on correct globals");
}

void runBaselineRefinement() {
    std::printf("[baseline refinement]\n");
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; i += 3)
        cw[static_cast<std::size_t>(i)] = 1; // varied tones, not all zero
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    auto const line = synthLine(tones, frameStart, 0, 0.7, 0.0, 0.0);
    auto const out = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), frameStart, kWindow,
        kWindow, kRate, tones, unitDataWeights());
    check(out.searched, "perfect sync input is searched");
    check(out.baselineFinite, "baseline metric finite");
    check(out.best.deltaSamples == 0 && out.best.deltaHz == 0.0 &&
              out.best.driftHzPerSec == 0.0,
          "perfect sync selects delta (0, 0, 0)");
    check(!out.atBoundary, "baseline selection not at boundary");
    check(!js8::aided::refinementAccepted(out),
          "perfect sync produces no bogus refinement");
}

void runKnownTimingError() {
    std::printf("[known timing error]\n");
    // Note: with continuous-phase FSK the power-contrast metric is exactly
    // blind to a one-sample shift (a single boundary intruder always
    // phase-aligns), so recovery is asserted up to that inherent +/-1
    // ambiguity: correct side and magnitude >= 1. A one-sample residual is
    // harmless for decoding and the gain gate rejects dust-level ties.
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 2654435761u >> 13) & 1);
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    for (int inj : {2, -2}) {
        auto const line =
            synthLine(tones, frameStart, inj, 0.7, 0.0, 0.0);
        auto const out = js8::aided::refineSync(
            line.data(), static_cast<int>(line.size()), frameStart, kWindow,
            kWindow, kRate, tones, unitDataWeights());
        char name[96];
        std::snprintf(name, sizeof(name),
                      "injected %+d samples recovered onside (got %+d)", inj,
                      out.best.deltaSamples);
        check(out.searched && (out.best.deltaSamples > 0) == (inj > 0) &&
                  std::abs(out.best.deltaSamples) >= 1 &&
                  std::abs(out.best.deltaSamples) <= 2,
              name);
    }
}

void runKnownResidualFrequency() {
    std::printf("[known residual frequency]\n");
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 40503u >> 7) & 1);
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    for (double inj : {0.4, -0.4}) {
        auto const line =
            synthLine(tones, frameStart, 0, 0.7, inj, 0.0);
        auto const out = js8::aided::refineSync(
            line.data(), static_cast<int>(line.size()), frameStart, kWindow,
            kWindow, kRate, tones, unitDataWeights());
        char name[96];
        std::snprintf(name, sizeof(name),
                      "injected %+.1f Hz recovered (got %+.1f Hz)", inj,
                      out.best.deltaHz);
        check(out.searched &&
                  (out.best.deltaHz > 0.0) == (inj > 0.0) &&
                  std::abs(out.best.deltaHz - inj) < 0.051,
              name);
    }
}

void runTimingPlusFrequency() {
    std::printf("[timing plus frequency]\n");
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 2654435761u >> 13) & 1);
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    auto const line = synthLine(tones, frameStart, 2, 0.7, 0.3, 0.0);
    auto const out = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), frameStart, kWindow,
        kWindow, kRate, tones, unitDataWeights());
    check(out.searched && out.best.deltaSamples >= 1 &&
              out.best.deltaSamples <= 2 &&
              std::abs(out.best.deltaHz - 0.3) < 0.051 &&
              out.best.driftHzPerSec == 0.0,
          "joint timing+frequency hypothesis recovered (timing to +/-1)");
    check(out.best.metric > out.baselineMetric * 1.005,
          "combined impairment shows real metric gain");
    check(out.atBoundary,
          "grid-edge timing selection is flagged at boundary");
    check(!js8::aided::refinementAccepted(out),
          "boundary-pinned selection fails the safety gate");
}

void runInteriorGateAcceptance() {
    std::printf("[interior gate acceptance]\n");
    // Interior impairment (0, +0.4 Hz, 0): the gate must accept an exact,
    // strictly-interior optimum.
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 40503u >> 7) & 1);
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    auto const line = synthLine(tones, frameStart, 0, 0.7, 0.4, 0.0);
    auto const out = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), frameStart, kWindow,
        kWindow, kRate, tones, unitDataWeights());
    check(out.searched && std::abs(out.best.deltaHz - 0.4) < 0.051 &&
              std::abs(out.best.deltaSamples) <= 1,
          "interior frequency impairment recovered");
    check(!out.atBoundary, "interior optimum not flagged at boundary");
    check(js8::aided::refinementAccepted(out),
          "interior optimum with real gain passes the gate");
}

void runDriftHandling() {
    std::printf("[drift handling]\n");
    // The absolute-time derotation makes drift observable through its linear
    // component, but the gain is small: an exact on-grid drift must be
    // selected yet rejected by the gate (on the boundary and below the
    // minimum gain), so drift alone never triggers a bogus re-demodulation.
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 40503u >> 7) & 1);
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    for (double inj : {0.02, -0.02}) {
        auto const line = synthLine(tones, frameStart, 0, 0.7, 0.0, inj);
        auto const out = js8::aided::refineSync(
            line.data(), static_cast<int>(line.size()), frameStart, kWindow,
            kWindow, kRate, tones, unitDataWeights());
        char name[96];
        std::snprintf(name, sizeof(name),
                      "injected %+.2f Hz/s drift selected (got %+.2f)", inj,
                      out.best.driftHzPerSec);
        check(out.searched && out.best.driftHzPerSec == inj, name);
        check(out.best.metric > out.baselineMetric,
              "drift correction shows small positive gain");
        check(!js8::aided::refinementAccepted(out),
              "drift-only optimum fails the safety gate");
    }
}

void runWrongCodeword() {
    std::printf("[wrong tentative codeword]\n");
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 2654435761u >> 13) & 1);
    auto const trueTones = tonesFromBits(cw);
    // Deliberately wrong: every data tone shifted by 4 (all 58 wrong),
    // Costas still exact, all weights high.
    std::array<int, 79> wrongTones = trueTones;
    for (int j = 0; j < 58; ++j) {
        int const k = js8::aided::dataSymbolIndex(j);
        wrongTones[static_cast<std::size_t>(k)] =
            (wrongTones[static_cast<std::size_t>(k)] + 4) % 8;
    }
    constexpr int frameStart = 64;
    auto const line = synthLine(trueTones, frameStart, 0, 0.7, 0.0, 0.0);
    auto const out = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), frameStart, kWindow,
        kWindow, kRate, wrongTones, unitDataWeights());
    check(out.searched, "wrong-word input still searches safely");
    check(!js8::aided::refinementAccepted(out),
          "wrong tentative word cannot pass the gain gate");
}

void runBoundsClipping() {
    std::printf("[bounds and clipping]\n");
    std::array<int8_t, 174> cw{};
    cw[0] = 1;
    auto const tones = tonesFromBits(cw);
    constexpr int need = 64 + 79 * kWindow + 16;
    check(js8::aided::sampleBoundsValid(64, 79, kWindow, kWindow, need, 2),
          "interior frame bounds valid");
    check(!js8::aided::sampleBoundsValid(-1, 79, kWindow, kWindow, need, 2),
          "negative start rejected");
    check(!js8::aided::sampleBoundsValid(64, 79, kWindow, kWindow,
                                         64 + 79 * kWindow - 1, 2),
          "end overflow rejected");
    check(!js8::aided::sampleBoundsValid(64, 79, kWindow, 0, need, 2),
          "zero window rejected");
    // Search with an out-of-bounds frame refuses instead of reading OOB.
    auto const line = synthLine(tones, 64, 0, 0.7, 0.0, 0.0);
    auto const bad = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), 0, kWindow, kWindow,
        kRate, tones, unitDataWeights());
    check(!bad.searched, "OOB frame start refuses search");
    // Tight-but-valid edge frame runs cleanly with finite metrics.
    int const edgeStart = 2;
    auto const edge = synthLine(tones, edgeStart, 0, 0.7, 0.0, 0.0);
    auto const ok = js8::aided::refineSync(
        edge.data(), static_cast<int>(edge.size()), edgeStart, kWindow,
        kWindow, kRate, tones, unitDataWeights());
    check(ok.searched && std::isfinite(ok.best.metric) &&
              std::isfinite(ok.baselineMetric),
          "edge-valid frame searches with finite metrics");
}

void runNonFiniteSafety() {
    std::printf("[NaN and Inf safety]\n");
    // NaN/Inf LLRs: zero weight, finite summary.
    std::array<float, 174> llr{};
    for (int i = 0; i < 174; ++i)
        llr[static_cast<std::size_t>(i)] = 2.0f;
    llr[0] = std::numeric_limits<float>::quiet_NaN();
    llr[4] = std::numeric_limits<float>::infinity();
    auto const conf = js8::aided::tentativeConfidences(llr);
    check(conf.weights[0] == 0.0f && conf.weights[1] == 0.0f,
          "non-finite LLR symbols get zero weight");
    check(std::isfinite(conf.mean), "confidence mean stays finite");
    // NaN samples: no NaN propagates into metrics or selection.
    std::array<int8_t, 174> cw{};
    cw[0] = 1;
    auto const tones = tonesFromBits(cw);
    constexpr int frameStart = 64;
    auto line = synthLine(tones, frameStart, 0, 0.7, 0.0, 0.0);
    line[static_cast<std::size_t>(frameStart + 10 * kWindow + 3)] =
        std::complex<float>(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    auto const out = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), frameStart, kWindow,
        kWindow, kRate, tones, unitDataWeights());
    check(!std::isnan(out.best.metric) &&
              !std::isnan(out.baselineMetric),
          "NaN samples never propagate into metrics");
    check(!js8::aided::refinementAccepted(out),
          "corrupted frame cannot pass the gate");
    // Direct metric guards.
    check(!std::isfinite(js8::aided::contrastMetric(
              std::numeric_limits<double>::quiet_NaN(), 1.0, kWindow)),
          "contrast rejects NaN power");
    check(!std::isfinite(js8::aided::expectedBinPower(
              nullptr, 100, 0, kWindow, 0, 0.0, 0.0, kRate)),
          "bin power rejects null samples");
}

void runRescueDemonstration() {
    std::printf("[helper-level rescue demonstration]\n");
    // True transmitted word; tentative word has 3 bit errors (near miss).
    std::array<int8_t, 174> trueCw{};
    for (int i = 0; i < 174; ++i)
        trueCw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 2654435761u >> 13) & 1);
    std::array<int8_t, 174> tentCw = trueCw;
    tentCw[5] ^= 1;
    tentCw[60] ^= 1;
    tentCw[150] ^= 1;
    auto const trueTones = tonesFromBits(trueCw);
    auto tentTones = tonesFromBits(tentCw);
    // Impaired frame: +2 samples, +0.3 Hz, plus fixed-seed noise.
    constexpr int frameStart = 64;
    auto line = synthLine(trueTones, frameStart, 2, 0.7, 0.3, 0.0);
    {
        std::uint32_t state = 0x12345678u;
        auto const randn = [&] {
            state = state * 1664525u + 1013904223u;
            double u =
                (static_cast<double>(state >> 8) + 1.0) / 4294967296.0;
            state = state * 1664525u + 1013904223u;
            double v =
                (static_cast<double>(state >> 8) + 1.0) / 4294967296.0;
            return std::sqrt(-2.0 * std::log(u)) * std::cos(6.28318530718 * v);
        };
        for (auto &x : line)
            x += std::complex<float>(static_cast<float>(0.1 * randn()),
                                     static_cast<float>(0.1 * randn()));
    }
    // Realistic LLRs: moderate magnitudes, signs mostly right.
    std::array<float, 174> llr{};
    for (int i = 0; i < 174; ++i) {
        float const mag = 1.5f + static_cast<float>((i * 37) % 40) / 10.0f;
        llr[static_cast<std::size_t>(i)] =
            trueCw[static_cast<std::size_t>(i)] ? mag : -mag;
    }
    auto const conf = js8::aided::tentativeConfidences(llr);
    std::array<float, 79> weights{};
    js8::aided::buildSymbolWeights(conf, weights);
    auto const out = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), frameStart, kWindow,
        kWindow, kRate, tentTones, weights);
    check(out.searched, "impaired near-miss frame is searched");
    check(out.best.deltaSamples >= 1 && out.best.deltaSamples <= 2 &&
              std::abs(out.best.deltaHz - 0.3) < 0.051,
          "noisy near-miss frame recovers timing side and frequency");
    check(out.best.metric > out.baselineMetric * 1.005,
          "noisy near-miss refinement shows real metric gain");
    check(conf.used > 40, "most data symbols contribute");
    std::printf("    info: used=%d meanConf=%.3f gainDb=%.2f\n", conf.used,
                conf.mean,
                js8::aided::metricGainDb(out.best.metric,
                                        out.baselineMetric));
}

} // namespace

int main() {
    runCodewordToTones();
    runConfidenceWeighting();
    runBaselineRefinement();
    runKnownTimingError();
    runKnownResidualFrequency();
    runTimingPlusFrequency();
    runInteriorGateAcceptance();
    runDriftHandling();
    runWrongCodeword();
    runBoundsClipping();
    runNonFiniteSafety();
    runRescueDemonstration();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED"
                                                      : "TESTS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
