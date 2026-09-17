// Deterministic unit tests for decoder-aided re-demodulation helpers.
//
// Covers: codeword->tones mapping, confidence weighting, baseline
// refinement, known timing error (incl. fractional true shifts), known
// residual frequency, timing+frequency, interior gate acceptance, drift
// handling, other-seven-tones contrast (Fix 9.A), tracked baseline (Fix
// 9.B), wrong tentative codeword, bounds/clipping, NaN/Inf safety, plus a
// helper-level rescue demonstration.
//
// The synthetic 79-symbol waveform uses continuous FSK phase (boundary phase
// jumps are multiples of 2*pi) with absolute-time residual/drift terms, the
// same physical model the refinement search assumes. Timing shifts displace
// actual symbol boundaries (fractional shifts supported); interferers are
// optional extra tones with continuous phase.
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
constexpr int kFrameStart = 64;

enum class Interferer { NONE, UNUSED_BIN, COMPETITOR };

// Continuous-phase 79-symbol FSK line. injShift displaces the true symbol
// timing (fractional values supported: the tone phase advances fractionally,
// which is exact for pure tones); resFreq/drift use absolute sample time.
// Optional interferer (added as a complex sum, not a phase shift):
// UNUSED_BIN adds a unit tone at DFT bin 12 (integer, hence orthogonal to
// bins 0..7: zero leakage into the valid tones); COMPETITOR adds a unit tone
// at (expected+4)%8 per symbol.
std::vector<std::complex<float>>
synthLine(std::array<int, 79> const &tones, int frameStart, double injShift,
          double startPhase, double resFreq, double drift,
          Interferer interferer = Interferer::NONE) {
    constexpr int kPad = 16;
    int const total = frameStart + 79 * kWindow + kPad;
    std::vector<std::complex<float>> line(static_cast<std::size_t>(total));
    constexpr double twoPi = 2.0 * std::numbers::pi;
    for (int idx = 0; idx < total; ++idx) {
        double const rel = static_cast<double>(idx) - frameStart - injShift;
        long sym = static_cast<long>(std::floor(rel / kWindow));
        if (sym < 0)
            sym = 0;
        if (sym > 78)
            sym = 78;
        double const local = rel - static_cast<double>(sym) * kWindow;
        double const t = static_cast<double>(idx) / kRate;
        int const tone = tones[static_cast<std::size_t>(sym)];
        double const phase = startPhase + twoPi * resFreq * t +
                             std::numbers::pi * drift * t * t +
                             twoPi * tone * local / kWindow;
        std::complex<float> sample =
            std::polar(1.0f, static_cast<float>(phase));
        if (interferer == Interferer::UNUSED_BIN) {
            double const iphase =
                startPhase + twoPi * 12.0 * local / kWindow;
            sample += std::polar(1.0f, static_cast<float>(iphase));
        } else if (interferer == Interferer::COMPETITOR) {
            double const iphase = startPhase +
                                  twoPi * ((tone + 4) % 8) * local / kWindow;
            sample += std::polar(1.0f, static_cast<float>(iphase));
        }
        line[static_cast<std::size_t>(idx)] = sample;
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

// Untracked baselines: nominal starts, zero tracker correction.
std::array<js8::aided::SymbolBaseline, 79>
makeBaselines(int frameStart, int extraShift = 0, float trackerHz = 0.0f) {
    std::array<js8::aided::SymbolBaseline, 79> b{};
    for (int k = 0; k < 79; ++k) {
        b[static_cast<std::size_t>(k)].startSamples =
            frameStart + k * kWindow + extraShift;
        b[static_cast<std::size_t>(k)].trackerHz = trackerHz;
    }
    return b;
}

js8::aided::Refinement runSearch(std::vector<std::complex<float>> const &line,
                                 std::array<js8::aided::SymbolBaseline, 79> const &b,
                                 std::array<int, 79> const &tones,
                                 std::array<float, 79> const &weights) {
    return js8::aided::refineSync(line.data(), static_cast<int>(line.size()),
                                  b, kWindow, kRate, tones, weights);
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
    auto const line = synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, 0.0);
    auto const out =
        runSearch(line, makeBaselines(kFrameStart), tones, unitDataWeights());
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
    for (double inj : {2.0, -2.0, 1.5, -1.5}) {
        auto const line =
            synthLine(tones, kFrameStart, inj, 0.7, 0.0, 0.0);
        auto const out = runSearch(line, makeBaselines(kFrameStart), tones,
                                   unitDataWeights());
        char name[96];
        std::snprintf(name, sizeof(name),
                      "injected %+.1f samples recovered onside (got %+d)", inj,
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
    for (double inj : {0.4, -0.4}) {
        auto const line =
            synthLine(tones, kFrameStart, 0.0, 0.7, inj, 0.0);
        auto const out = runSearch(line, makeBaselines(kFrameStart), tones,
                                   unitDataWeights());
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
    auto const line = synthLine(tones, kFrameStart, 2.0, 0.7, 0.3, 0.0);
    auto const out = runSearch(line, makeBaselines(kFrameStart), tones,
                               unitDataWeights());
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
    auto const line = synthLine(tones, kFrameStart, 0.0, 0.7, 0.4, 0.0);
    auto const out = runSearch(line, makeBaselines(kFrameStart), tones,
                               unitDataWeights());
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
    for (double inj : {0.02, -0.02}) {
        auto const line = synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, inj);
        auto const out = runSearch(line, makeBaselines(kFrameStart), tones,
                                   unitDataWeights());
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

void runOtherSevenTones() {
    std::printf("[other seven tones]\n");
    // Direct contract: contrast uses the actual 8 valid bins only.
    double const low[7] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    double const hot[7] = {50.0, 50.0, 50.0, 50.0, 50.0, 50.0, 50.0};
    check(js8::aided::contrastEight(100.0, low) == 99.0,
          "contrast = expected - mean(other 7)");
    check(js8::aided::contrastEight(100.0, hot) == 50.0,
          "energy in valid other tones lowers contrast");
    check(js8::aided::contrastEight(100.0, low) >
              js8::aided::contrastEight(100.0, hot),
          "valid-tone competition decreases contrast");
    check(!std::isfinite(js8::aided::contrastEight(
              std::numeric_limits<double>::quiet_NaN(), low)),
          "contrast rejects NaN expected power");
    check(!std::isfinite(js8::aided::contrastEight(100.0, nullptr)),
          "contrast rejects null other-tones");
    // End to end: energy at an UNUSED integer DFT bin (12 of 32) is
    // orthogonal to bins 0..7, so it must barely affect the metric. A total
    // time-domain power (Parseval) implementation would fold it into the
    // alternative-tone estimate and drop ~14%.
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 2654435761u >> 13) & 1);
    auto const tones = tonesFromBits(cw);
    auto const clean = synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, 0.0);
    auto const withUnused =
        synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, 0.0,
                  Interferer::UNUSED_BIN);
    auto const base = makeBaselines(kFrameStart);
    auto const w = unitDataWeights();
    auto const mClean =
        runSearch(clean, base, tones, w).baselineMetric;
    auto const mUnused =
        runSearch(withUnused, base, tones, w).baselineMetric;
    check(std::isfinite(mClean) && std::isfinite(mUnused) && mClean > 0.0,
          "both frames score finite positive metrics");
    check(mUnused / mClean > 0.95,
          "unused-bin energy barely affects contrast");
    // Sanity: per-symbol competitor energy DOES move the metric.
    auto const withCompetitor =
        synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, 0.0,
                  Interferer::COMPETITOR);
    auto const mComp =
        runSearch(withCompetitor, base, tones, w).baselineMetric;
    double const ratio = mComp / mClean;
    char info[96];
    std::snprintf(info, sizeof(info),
                  "valid-tone competition moves metric (ratio %.3f)", ratio);
    check(std::isfinite(ratio) && ratio < 0.95 && ratio > 0.5, info);
}

void runTrackedBaseline() {
    std::printf("[tracked baseline]\n");
    // Frame with true (+2 samples, +0.4 Hz) impairment. With UNTRACKED
    // baselines the search must recover it (control). With baselines that
    // already contain the correction (starts pre-shifted by +2, trackerHz
    // set to the mirror-correcting value that re-centers the tone peak),
    // the baseline is already optimal: the search must select a
    // (0,0,0)-relative hypothesis and claim no gain.
    std::array<int8_t, 174> cw{};
    for (int i = 0; i < 174; ++i)
        cw[static_cast<std::size_t>(i)] =
            static_cast<int8_t>((i * 40503u >> 7) & 1);
    auto const tones = tonesFromBits(cw);
    auto const line = synthLine(tones, kFrameStart, 2.0, 0.7, 0.4, 0.0);
    auto const w = unitDataWeights();
    auto const untracked = runSearch(line, makeBaselines(kFrameStart), tones,
                                     w);
    check(untracked.searched &&
              untracked.best.deltaSamples >= 1 &&
              std::abs(untracked.best.deltaHz - 0.4) < 0.051,
          "control: untracked baseline recovers impairment");
    check(untracked.best.metric > untracked.baselineMetric * 1.005,
          "control: untracked baseline shows real gain");
    auto const tracked =
        runSearch(line, makeBaselines(kFrameStart, 2, -0.4f), tones, w);
    check(tracked.searched, "tracked baseline is searched");
    check(tracked.best.deltaSamples >= -1 && tracked.best.deltaSamples <= 1 &&
              tracked.best.deltaHz == 0.0 &&
              tracked.best.driftHzPerSec == 0.0,
          "tracked baseline selects relative (0, 0, 0)");
    check(!tracked.atBoundary, "tracked selection not at boundary");
    check(!js8::aided::refinementAccepted(tracked),
          "no fake gain over the already-tracked baseline");
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
    auto const line = synthLine(trueTones, kFrameStart, 0.0, 0.7, 0.0, 0.0);
    auto const out = runSearch(line, makeBaselines(kFrameStart), wrongTones,
                               unitDataWeights());
    check(out.searched, "wrong-word input still searches safely");
    check(!js8::aided::refinementAccepted(out),
          "wrong tentative word cannot pass the gain gate");
}

void runBoundsClipping() {
    std::printf("[bounds and clipping]\n");
    std::array<int8_t, 174> cw{};
    cw[0] = 1;
    auto const tones = tonesFromBits(cw);
    auto good = makeBaselines(kFrameStart);
    check(js8::aided::symbolBoundsValid(good, kWindow,
                                        kFrameStart + 79 * kWindow + 16, 2),
          "interior frame bounds valid");
    auto negStart = makeBaselines(kFrameStart);
    negStart[0].startSamples = -5;
    check(!js8::aided::symbolBoundsValid(negStart, kWindow,
                                         kFrameStart + 79 * kWindow + 16, 2),
          "negative start rejected");
    check(!js8::aided::symbolBoundsValid(
              good, kWindow, kFrameStart + 79 * kWindow - 1, 2),
          "end overflow rejected");
    check(!js8::aided::symbolBoundsValid(good, 0,
                                         kFrameStart + 79 * kWindow + 16, 2),
          "zero window rejected");
    // Search with out-of-bounds baselines refuses instead of reading OOB.
    auto const line = synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, 0.0);
    auto const bad = runSearch(line, negStart, tones, unitDataWeights());
    check(!bad.searched, "OOB baseline start refuses search");
    // Non-finite tracker metadata on a USED symbol refuses; on an unused
    // symbol it is ignored.
    auto badFreq = makeBaselines(kFrameStart);
    badFreq[10].trackerHz = std::numeric_limits<float>::quiet_NaN();
    auto const badF =
        runSearch(line, badFreq, tones, unitDataWeights());
    check(!badF.searched, "non-finite tracker metadata refuses search");
    std::array<float, 79> sparse{};
    sparse[0] = 1.0f; // only Costas symbol 0 used
    auto const okF = js8::aided::refineSync(
        line.data(), static_cast<int>(line.size()), badFreq, kWindow, kRate,
        tones, sparse);
    check(okF.searched && std::isfinite(okF.baselineMetric),
          "unused corrupt metadata is ignored");
    // Tight-but-valid edge frame runs cleanly with finite metrics.
    auto const edge = synthLine(tones, 2, 0.0, 0.7, 0.0, 0.0);
    auto const ok =
        runSearch(edge, makeBaselines(2), tones, unitDataWeights());
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
    auto line = synthLine(tones, kFrameStart, 0.0, 0.7, 0.0, 0.0);
    line[static_cast<std::size_t>(kFrameStart + 10 * kWindow + 3)] =
        std::complex<float>(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    auto const out = runSearch(line, makeBaselines(kFrameStart), tones,
                               unitDataWeights());
    check(!std::isnan(out.best.metric) &&
              !std::isnan(out.baselineMetric),
          "NaN samples never propagate into metrics");
    check(!js8::aided::refinementAccepted(out),
          "corrupted frame cannot pass the gate");
    // Direct tracker-replay guards.
    std::complex<float> win[4] = {};
    check(!js8::aided::replayTrackerCorrection(nullptr, 4, 0.0, kRate),
          "replay rejects null window");
    check(!js8::aided::replayTrackerCorrection(
              win, 4, std::numeric_limits<double>::quiet_NaN(), kRate),
          "replay rejects non-finite correction");
    // Tracker replay is a no-op at 0 Hz (matches disabled-tracker path).
    win[0] = {1.0f, 0.5f};
    win[1] = {-0.25f, 2.0f};
    check(js8::aided::replayTrackerCorrection(win, 2, 0.0, kRate) &&
              win[0] == std::complex<float>{1.0f, 0.5f} &&
              win[1] == std::complex<float>{-0.25f, 2.0f},
          "zero-Hz replay is bit-exact no-op");
}

void runFinalizeTiming() {
    std::printf("[finalize timing]\n");
    // xdt2 = ibest*DT2 extraction grid; aidedDt moves the seed relatively.
    // FS2 = 200 -> DT2 = 0.005 s/sample: +2 samples -> +0.01 s.
    constexpr float xdt2 = 0.5f;
    constexpr float dt2 = 0.005f;
    float const late = js8::aided::aidedFrameXdt(xdt2, 2, dt2);
    float const early = js8::aided::aidedFrameXdt(xdt2, -2, dt2);
    check(late == xdt2 + 2 * dt2, "aidedDt=+2 gives xdt2+2/FS2");
    check(early == xdt2 - 2 * dt2, "aidedDt=-2 moves the opposite way");
    check(late > xdt2 && early < xdt2,
          "late signal increases xdt, early decreases it");
    check(js8::aided::aidedFrameXdt(xdt2, 0, dt2) == xdt2,
          "zero delta preserves xdt2");
    check(!std::isfinite(js8::aided::aidedFrameXdt(
              std::numeric_limits<float>::quiet_NaN(), 2, dt2)),
          "non-finite xdt2 gives NaN");
}

// Baselines with uniform tracker metadata for the midpoint tests.
std::array<js8::aided::SymbolBaseline, 79>
makeTrackedBaselines(float trackerHz) {
    std::array<js8::aided::SymbolBaseline, 79> b{};
    for (int k = 0; k < 79; ++k) {
        b[static_cast<std::size_t>(k)].startSamples =
            kFrameStart + k * kWindow;
        b[static_cast<std::size_t>(k)].trackerHz = trackerHz;
    }
    return b;
}

std::array<float, 79> costasOnlyUnitWeights() {
    std::array<float, 79> w{};
    js8::aided::buildCostasWeights(w);
    return w;
}

void runFinalizeTrackerDisabled() {
    std::printf("[finalize tracker disabled]\n");
    // trackerHz = 0, physical +0.3, aidedDf = +0.3, no drift.
    auto const out = js8::aided::aidedPhysicalResidualAtMidpoint(
        0.3, 0.0, makeTrackedBaselines(0.0f), kWindow, kRate,
        costasOnlyUnitWeights());
    check(out.valid, "midpoint fit valid");
    check(std::abs(out.residualHz - 0.3) < 1.0e-6,
          "disabled tracker reduces to f1+aidedDf (+0.3 Hz)");
    check(std::abs(out.trackerHzAtMid) < 1.0e-6,
          "midpoint tracker estimate is zero");
    check(out.midpointSeconds > 0.0, "midpoint time positive");
}

void runFinalizePerfectTracker() {
    std::printf("[finalize perfect tracker]\n");
    // Physical +0.4, tracker recorded -0.4 (mirror convention: replay adds
    // the recorded value, so a correcting tracker holds -residual),
    // aidedDf ~= 0.
    auto const out = js8::aided::aidedPhysicalResidualAtMidpoint(
        0.0, 0.0, makeTrackedBaselines(-0.4f), kWindow, kRate,
        costasOnlyUnitWeights());
    check(out.valid, "midpoint fit valid");
    check(std::abs(out.residualHz - 0.4) < 1.0e-6,
          "perfect tracker still yields physical +0.4 Hz");
    check(std::abs(out.trackerHzAtMid + 0.4) < 1.0e-6,
          "midpoint tracker value recovered as -0.4");
}

void runFinalizePartialTracker() {
    std::printf("[finalize partial tracker]\n");
    // Physical +0.4, tracker recorded -0.25, aidedDf = +0.15.
    auto const out = js8::aided::aidedPhysicalResidualAtMidpoint(
        0.15, 0.0, makeTrackedBaselines(-0.25f), kWindow, kRate,
        costasOnlyUnitWeights());
    check(out.valid, "midpoint fit valid");
    check(std::abs(out.residualHz - 0.4) < 1.0e-6,
          "partial correction sums to physical +0.4 Hz");
}

void runFinalizeNegativeFrequency() {
    std::printf("[finalize negative frequency]\n");
    // Mirror of the perfect-tracker case: physical -0.4, tracker +0.4.
    auto const out = js8::aided::aidedPhysicalResidualAtMidpoint(
        0.0, 0.0, makeTrackedBaselines(0.4f), kWindow, kRate,
        costasOnlyUnitWeights());
    check(out.valid, "midpoint fit valid");
    check(std::abs(out.residualHz + 0.4) < 1.0e-6,
          "signs reverse correctly to physical -0.4 Hz");
}

void runFinalizeDrift() {
    std::printf("[finalize drift]\n");
    // Linear residual r(t) = 0.1 + 0.02*t, tracker zero: the scalar must be
    // evaluated at the frame midpoint, not at the edges, and aidedDd must
    // contribute (drift is never discarded).
    auto const b = makeTrackedBaselines(0.0f);
    auto const out = js8::aided::aidedPhysicalResidualAtMidpoint(
        0.1, 0.02, b, kWindow, kRate, costasOnlyUnitWeights());
    check(out.valid, "midpoint fit valid with drift");
    // t_k = (64 + 32k + 16)/200, k = 0..78 -> t in [0.4, 12.88],
    // tMid = 6.64 s -> expected 0.1 + 0.02*6.64 = 0.2328.
    double const expected = 0.1 + 0.02 * out.midpointSeconds;
    check(std::abs(out.midpointSeconds - 6.64) < 1.0e-9,
          "midpoint time is the frame center");
    check(std::abs(out.residualHz - expected) < 1.0e-9,
          "scalar equals physical frequency at midpoint");
    check(out.residualHz > 0.2,
          "drift contributes (not the df-only value 0.1)");
    // Degenerate inputs are rejected, not silently estimated.
    std::array<float, 79> noWeights{};
    auto const bad = js8::aided::aidedPhysicalResidualAtMidpoint(
        0.1, 0.02, b, kWindow, kRate, noWeights);
    check(!bad.valid, "all-zero weights give invalid result");
    auto const badDf = js8::aided::aidedPhysicalResidualAtMidpoint(
        std::numeric_limits<double>::quiet_NaN(), 0.02, b, kWindow, kRate,
        costasOnlyUnitWeights());
    check(!badDf.valid, "non-finite aidedDf gives invalid result");
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
    auto line = synthLine(trueTones, kFrameStart, 2.0, 0.7, 0.3, 0.0);
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
    auto const out =
        runSearch(line, makeBaselines(kFrameStart), tentTones, weights);
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
    runOtherSevenTones();
    runTrackedBaseline();
    runFinalizeTiming();
    runFinalizeTrackerDisabled();
    runFinalizePerfectTracker();
    runFinalizePartialTracker();
    runFinalizeNegativeFrequency();
    runFinalizeDrift();
    runWrongCodeword();
    runBoundsClipping();
    runNonFiniteSafety();
    runRescueDemonstration();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED"
                                                      : "TESTS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
