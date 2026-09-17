// Deterministic unit test for the #5c post-decode timing-drift model.
//
// Standalone: links only the header-only refinement template, no Qt/FFTW.
// Build/run:
//   g++ -std=c++20 -O2 -I.. tools/sic_timing_warp_test.cpp -o /tmp/sic_tarp_test
//   /tmp/sic_tarp_test
//
// It drives js8::refineSicReference (the #5c warp lives inside it) with
// synthetic known waveforms carrying a known linear sample-clock drift and
// asserts the recovered drift slope and the in-bounds / no-NaN behavior.
//
// The reference (nominal) is complex, but the received waveform is real (as in
// the actual decoder, where dd is real-valued). A real received signal dd[k] =
// cos(phase(warped(k))) carries the (warped) phase trajectory, which the
// per-symbol matched correlation exposes; the fit recovers the injected drift.

#include "../JS8_Mode/sic_refinement.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {

constexpr int NSPS = 1920; // Mock samples-per-symbol (ModeA-sized).
constexpr int NN = 79;     // Full JS8-length symbol count.
constexpr double SAMPLE_RATE = 12000.0;
constexpr double TEST_BASE_HZ = 1000.0;
constexpr double POSITIVE_DRIFT = 0.15;
constexpr double NEGATIVE_DRIFT = -0.15;
constexpr double TWO_PI = 6.28318530717958647692;

using Comp = std::complex<float>;

// Deterministic tone pattern exercising every adjacent-symbol transition while
// remaining exactly reproducible.
std::array<int, NN> makeTestTones() {
    std::array<int, NN> tones{};
    for (int i = 0; i < NN; ++i)
        tones[i] = (3 * i + 1) % 8;
    return tones;
}

struct MockMode {
    static constexpr int NSPS = ::NSPS;
};

// Nominal (complex) reference plus its real phase trajectory.
struct Wave {
    std::vector<Comp> nominal;
    std::vector<double> phase;
};

// Build a nominal JS8-style reference from a tone array on a nonzero carrier,
// matching the phase accumulation used by genjs8refsig(). The unwrapped phase
// trajectory is retained so the synthetic receiver can apply an exact timing
// warp rather than interpolating a wrapped phase.
Wave buildWave(std::array<int, NN> const &tones) {
    Wave w;
    w.nominal.reserve(static_cast<std::size_t>(NN) * NSPS);
    w.phase.reserve(static_cast<std::size_t>(NN) * NSPS);
    double phase = 0.0;
    for (int i = 0; i < NN; ++i) {
        double const dphi = TWO_PI * TEST_BASE_HZ / SAMPLE_RATE +
                            TWO_PI * static_cast<double>(tones[i]) / NSPS;
        for (int is = 0; is < NSPS; ++is) {
            w.phase.push_back(phase);
            w.nominal.push_back(std::polar(
                1.0f, static_cast<float>(std::fmod(phase, TWO_PI))));
            phase += dphi;
        }
    }
    return w;
}

// Fractional-linear interpolation of the phase trajectory, clamped to the frame.
double interpPhase(std::vector<double> const &phase, double x) {
    std::size_t const n = phase.size();
    if (x <= 0.0)
        return phase[0];
    if (x >= static_cast<double>(n - 1))
        return phase[n - 1];
    std::size_t i0 = static_cast<std::size_t>(x);
    std::size_t i1 = std::min(i0 + 1, n - 1);
    double frac = x - static_cast<double>(i0);
    return phase[i0] * (1.0 - frac) + phase[i1] * frac;
}

// Synthetic real received waveform carrying a known linear drift b (samples/sec):
// dd[k] = cos(phase(k - b*t)). Mixing with the complex nominal exposes the
// (warped) phase trajectory, from which the fit recovers b.
std::vector<float> buildSamples(Wave const &w, double b) {
    std::size_t const n = w.nominal.size();
    std::vector<float> samples(n);
    for (std::size_t k = 0; k < n; ++k) {
        double const t = static_cast<double>(k) / SAMPLE_RATE;
        samples[k] = static_cast<float>(std::cos(interpPhase(w.phase,
                                                            static_cast<double>(k) - b * t)));
    }
    return samples;
}

int failures = 0;
void check(bool cond, char const *what) {
    if (cond) {
        std::printf("  PASS: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// True when every output sample is finite (no NaN/Inf slipped into the warp).
bool allFinite(std::vector<Comp> const &warped) {
    for (std::size_t i = 0; i < warped.size(); ++i) {
        if (!std::isfinite(warped[i].real()) || !std::isfinite(warped[i].imag()))
            return false;
    }
    return true;
}

// Smallest absolute difference between two angles, in radians.
double phaseError(double actual, double expected) {
    double const halfTurn = 0.5 * TWO_PI;
    double const wrapped = std::fmod(actual - expected + halfTurn, TWO_PI);
    double const nonnegative = wrapped < 0.0 ? wrapped + TWO_PI : wrapped;
    return std::abs(nonnegative - halfTurn);
}

void runWarpHelper() {
    std::printf("[timing warp helper]\n");
    std::array<int, NN> tones = makeTestTones();
    Wave w = buildWave(tones);

    auto const same = js8::applySicTimingWarp(w.nominal, 0.0, 0.0, SAMPLE_RATE);
    check(same.size() == w.nominal.size(), "warp preserves vector length");
    check(same == w.nominal,
          "zero timing drift returns the original waveform");

    auto const late = js8::applySicTimingWarp(w.nominal, 0.75, 0.0, SAMPLE_RATE);
    auto const early =
        js8::applySicTimingWarp(w.nominal, -0.75, 0.0, SAMPLE_RATE);
    check(allFinite(late) && allFinite(early),
          "fractional offsets stay finite and in bounds");
    // Spot-check interpolation away from frame edges and tone boundaries.
    std::size_t const probe =
        static_cast<std::size_t>(5) * static_cast<std::size_t>(NSPS) +
        static_cast<std::size_t>(NSPS / 2);
    check(phaseError(std::arg(late[probe]),
                     interpPhase(w.phase, probe - 0.75)) < 1.0e-3,
          "positive timing drift looks earlier in the reference");
    check(phaseError(std::arg(early[probe]),
                     interpPhase(w.phase, probe + 0.75)) < 1.0e-3,
          "negative timing drift looks later in the reference");
    // Sources outside the reference become zero rather than wrapping around.
    check(late.front() == Comp{0.0F, 0.0F},
          "out-of-frame warp samples are zeroed");
}

void runClean() {
    std::printf("[clean, no drift]\n");
    std::array<int, NN> tones = makeTestTones();
    Wave w = buildWave(tones);

    auto s = buildSamples(w, 0.0); // no drift

    auto r = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, true);
    auto const baseline = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, false);

    check(std::abs(r.timingDriftSamplesPerSecond) < 0.02,
          "no-drift recovered slope near zero");
    check(std::abs(r.timingEndToEndSamples) <= 2.0,
          "no-drift end-to-end within bound");
    check(!r.timingRefined, "no-drift commits no warp (falls back to #5b)");
    check(r.reference == baseline.reference,
          "no-drift result matches timing-disabled #5b behavior");
    check(allFinite(r.reference), "no-drift output all finite / in bounds");
}

void runPositiveDrift(double b) {
    std::printf("[positive drift b=%.3f]\n", b);
    std::array<int, NN> tones = makeTestTones();
    Wave w = buildWave(tones);
    auto s = buildSamples(w, b);

    auto r = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, true);

    check(r.timingRefined, "positive drift commits a warp");
    check(r.timingDriftSamplesPerSecond > 0.0,
          "positive drift recovers a positive slope");
    check(std::abs(r.timingDriftSamplesPerSecond - b) < 0.20 * b + 0.03,
          "recovered slope close to injected value");
    check(std::abs(r.timingEndToEndSamples) <= 2.0,
          "end-to-end within bound");
    check(allFinite(r.reference), "warped output all finite / in bounds");
    check(r.refined, "positive-drift refinement accepted");
}

void runNegativeDrift(double b) {
    std::printf("[negative drift b=%.3f]\n", b);
    std::array<int, NN> tones = makeTestTones();
    Wave w = buildWave(tones);
    auto s = buildSamples(w, b);

    auto r = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, true);

    check(r.timingRefined, "negative drift commits a warp");
    check(r.timingDriftSamplesPerSecond < 0.0,
          "negative drift recovers a negative slope");
    check(std::abs(r.timingDriftSamplesPerSecond - b) < 0.20 * std::abs(b) + 0.03,
          "recovered slope close to injected value");
}

void runDisabled() {
    std::printf("[timing drift disabled]\n");
    std::array<int, NN> tones = makeTestTones();
    Wave w = buildWave(tones);
    auto s = buildSamples(w, 0.4); // drift present but must be ignored

    auto r = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, false);

    check(!r.timingRefined, "disabled: no warp committed");
    check(std::abs(r.timingDriftSamplesPerSecond) < 1e-6,
          "disabled: slope left at zero");
}

void runNoisy() {
    std::printf("[noisy, drift present]\n");
    std::array<int, NN> tones = makeTestTones();
    Wave w = buildWave(tones);

    std::mt19937 rng(1234);
    std::normal_distribution<double> noise(0.0, 0.45);
    auto s = buildSamples(w, 0.4);
    for (std::size_t k = 0; k < s.size(); ++k) {
        double re = static_cast<double>(s[k]) + noise(rng);
        s[k] = static_cast<float>(std::clamp(re, -2.0, 2.0));
    }

    auto r = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, true);
    auto const baseline = js8::refineSicReference<MockMode::NSPS, NN>(
        w.nominal, s, tones, 0.0f, false);

    check(!r.timingRefined,
          "noisy: incoherent fit rejected (falls back to #5b)");
    check(std::isfinite(r.timingFitRmsSamples), "noisy: fit RMS finite");
    check(r.reference == baseline.reference,
          "noisy: rejected warp leaves #5b output unchanged");
    check(allFinite(r.reference), "noisy: output all finite / in bounds");
}

} // namespace

int main() {
    runWarpHelper();
    runClean();
    runPositiveDrift(POSITIVE_DRIFT);
    runNegativeDrift(NEGATIVE_DRIFT);
    runDisabled();
    runNoisy();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED"
                                                       : "TESTS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
