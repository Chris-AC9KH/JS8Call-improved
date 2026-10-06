/**
 * @file sic_decode_context_test.cpp
 * @brief Checks the production SIC wrapper against finalized decode timing.
 */
#include <QDebug>
#include <QLoggingCategory>

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <vector>

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

#include "JS8_Mode/sic_refinement.h"

namespace {
constexpr int NN = 79;
struct Mode {
    static constexpr int NSPS = 384;
};

struct DecodeContext {
    std::vector<std::complex<float>> nominal;
    std::vector<float> dd;
    std::array<int, NN> tones{};
    float xdt2;
    struct {
        float frequencyHz = 1003.7f;
        float xdtSeconds = 0.1f;
    } fs;

    std::vector<std::complex<float>>
    genjs8refsig(std::array<int, NN> const &, float const) {
        return nominal;
    }

    std::vector<std::complex<float>> referenceForDecode() {
        return genjs8refsig(tones, fs.frequencyHz);
    }
};
} // namespace

int main() {
    ::unsetenv("JS8_DISABLE_SIC_REFINEMENT");
    ::unsetenv("JS8_DISABLE_SIC_TIMING_DRIFT");
    DecodeContext context;
    context.nominal.resize(NN * Mode::NSPS);
    context.dd.resize(context.nominal.size() + 2400);
    double phase = 0.0;
    for (int symbol = 0; symbol < NN; ++symbol) {
        context.tones[symbol] = (symbol * 5 + symbol / 8) % 8;
        double const frequency = context.fs.frequencyHz +
                                 context.tones[symbol] * 12000.0 / Mode::NSPS;
        for (int sample = 0; sample < Mode::NSPS; ++sample) {
            int const index = symbol * Mode::NSPS + sample;
            phase += 2.0 * std::numbers::pi * frequency / 12000.0;
            context.nominal[index] = {
                static_cast<float>(std::cos(phase)),
                static_cast<float>(std::sin(phase))};
            context.dd[1200 + index] = context.nominal[index].real();
        }
    }
    auto const expected = js8::refineSicReference<Mode::NSPS, NN>(
        context.nominal, context.dd, context.tones,
        context.fs.xdtSeconds, true);
    int failures = 0;
    bool rounded = true;
    int legacyMismatches = 0;
    for (int rate : {100, 200, 500, 1000}) {
        for (int sample = -6000; sample <= 6000; ++sample) {
            float const seconds = sample * (1.0f / rate);
            int const expectedSample = sample * (12000 / rate);
            rounded &= js8::sicStartSample(seconds) == expectedSample;
            legacyMismatches += static_cast<int>(seconds * 12000.0f) !=
                                expectedSample;
        }
    }
    failures += !rounded;
    std::printf("SIC timing grids legacyMismatches=%d %s\n",
                legacyMismatches, rounded ? "PASS" : "FAIL");
    for (float coarse : {0.05f, 0.0985f, 0.1f, 0.14f}) {
        context.xdt2 = coarse;
        auto const actual = context.referenceForDecode();
        bool const correct = actual == expected.reference;
        failures += !correct;
        std::printf("SIC coarseXdt=%.4f finalizedXdt=%.4f %s\n",
                    coarse, context.fs.xdtSeconds, correct ? "PASS" : "FAIL");
    }
    return failures == 0 ? 0 : 1;
}
