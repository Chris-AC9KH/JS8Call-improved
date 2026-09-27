// Paired-seed, symbol-bin AWGN benchmark of complete (174,87) LDPC frames.
// Runs the production encoder, likelihood processor, BP, feedback, and rescue.
// Assumes correct sync/FFT extraction; use whitening_diag for waveform checks.
// Build (with an existing CMake autogen directory):
// clang++ -std=c++20 -O2 -I. -Ibuild/JS8Call_autogen/include \
//   $(pkg-config --cflags Qt6Core fftw3f) tools/llr_frame_benchmark.cpp \
//   JS8_Mode/FrequencyTracker.cpp \
//   build/JS8Call_autogen/33ZB6LRKMI/moc_JS8.cpp \
//   $(pkg-config --libs Qt6Core fftw3f) \
//   -o /tmp/llr_frame_benchmark
// Usage: llr_frame_benchmark [trials-per-SNR=200] [first-SNR-dB=4] \
//                              [last-SNR-dB=10] [step-dB=0.5]

#include <QLoggingCategory>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;
Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

#include "../JS8_Mode/JS8.cpp"

namespace {
using Processor = js8::WhiteningProcessor<8, 58, 174>;
using Matrix = std::array<std::array<float, 58>, 8>;
constexpr char const *messages[] = {"TESTTEST1234", "TESTTEST1235",
                                     "AAAAAAAAAAAA", "ZZZZZZZZZZZZ"};

struct Frame {
    Matrix magnitudes{};
    std::array<int, 58> winners{};
    std::array<int8_t, N> expected{};
    std::optional<js8::CoherentBlend<8, 58>> blend;
};

Frame synthesize(double snrDb, unsigned trial) {
    Frame frame;
    std::array<int, NN> tones{};
    JS8::encode(0, JS8::Costas::array(ModeA::NCOSTAS),
                messages[trial % 4], tones.data());
    for (int j = 0; j < 58; ++j) {
        int const symbol = j < 29 ? j + 7 : j + 14;
        int const tone = tones[symbol];
        for (int b = 0; b < 3; ++b)
            frame.expected[3 * j + b] = (tone >> (2 - b)) & 1;
    }
    double const sigma = std::pow(10.0, -snrDb / 20.0);
    std::mt19937 rng(0x712349u + 7919u * trial);
    std::normal_distribution<double> noise(0.0, sigma / std::sqrt(2.0));
    std::uniform_real_distribution<double> phaseDist(-3.0, 3.0);
    double const phase = phaseDist(rng);
    std::complex<double> const signal = std::polar(1.0, phase);

    std::vector<js8::CoherentPilot> pilots;
    std::vector<js8::CoherentDataSymbol> data;
    for (int sym = 0; sym < NN; ++sym) {
        bool const pilot = sym < 7 || (sym >= 36 && sym < 43) || sym >= 72;
        int const dataIndex = sym < 36 ? sym - 7 : sym - 14;
        if (pilot) {
            auto const value = signal + std::complex<double>{noise(rng), noise(rng)};
            pilots.push_back({sym * 0.16, value, tones[sym], sym, 0.0, 0.0});
        } else {
            js8::CoherentDataSymbol entry;
            entry.baseTimeSeconds = sym * 0.16;
            for (int tone = 0; tone < 8; ++tone) {
                std::complex<double> value{noise(rng), noise(rng)};
                if (tone == tones[sym])
                    value += signal;
                entry.bins[tone] = {static_cast<float>(value.real()),
                                    static_cast<float>(value.imag())};
                frame.magnitudes[tone][dataIndex] =
                    static_cast<float>(std::abs(value));
            }
            data.push_back(entry);
        }
    }
    for (int j = 0; j < 58; ++j) {
        frame.winners[j] = 0;
        for (int tone = 1; tone < 8; ++tone)
            if (frame.magnitudes[tone][j] >
                frame.magnitudes[frame.winners[j]][j])
                frame.winners[j] = tone;
    }
    auto const coherent = js8::computeCoherentToneScores(
        pilots, data, 12, 5.0, 32, 375.0);
    if (coherent.alpha > 0.0 && coherent.coherentNumerators.size() == 58) {
        js8::CoherentBlend<8, 58> blend;
        blend.amplitude = coherent.amplitude;
        blend.alpha = static_cast<float>(coherent.alpha);
        for (int j = 0; j < 58; ++j)
            for (int tone = 0; tone < 8; ++tone)
                blend.numerators[tone][j] = coherent.coherentNumerators[j][tone];
        frame.blend = blend;
    }
    return frame;
}

// Exact pre-e67b5ad max-of-four magnitude and log-magnitude metrics,
// whitening, erasure, and old 2.83-per-frame normalization.
std::array<std::array<float, N>, 2> legacy(Frame const &frame, float threshold) {
    std::array<std::array<float, N>, 2> llr{};
    auto median = [](std::vector<float> v) {
        auto const mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + mid, v.end());
        float result = v[mid];
        if (v.size() % 2 == 0) {
            std::nth_element(v.begin(), v.begin() + mid - 1, v.end());
            result = 0.5f * (result + v[mid - 1]);
        }
        return result;
    };
    std::array<float, 8> toneNoise{};
    for (int tone = 0; tone < 8; ++tone) {
        std::vector<float> values;
        for (int j = 0; j < 58; ++j)
            if (frame.winners[j] != tone)
                values.push_back(frame.magnitudes[tone][j]);
        toneNoise[tone] = median(values);
    }
    for (int j = 0; j < 58; ++j) {
        std::array<float, 8> ps{};
        std::vector<float> noise;
        for (int tone = 0; tone < 8; ++tone) {
            ps[tone] = frame.magnitudes[tone][j];
            if (tone != frame.winners[j])
                noise.push_back(ps[tone]);
        }
        float const localNoise = std::sqrt(
            toneNoise[frame.winners[j]] * median(noise) + 1e-12f);
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1)
                for (float &value : ps)
                    value = std::log(value + 1e-32f);
            for (int bit = 0; bit < 3; ++bit) {
                float one = -INFINITY, zero = -INFINITY;
                for (int tone = 0; tone < 8; ++tone)
                    (((tone >> (2 - bit)) & 1) ? one : zero) =
                        std::max(((tone >> (2 - bit)) & 1) ? one : zero,
                                 ps[tone]);
                float &value = llr[pass][3 * j + bit];
                value = (one - zero) / localNoise;
                if (threshold > 0.0f && std::abs(value) < threshold)
                    value = 0.0f;
            }
        }
    }
    for (auto &pass : llr) {
        double sum = 0.0, squares = 0.0;
        for (float value : pass) {
            sum += value;
            squares += value * value;
        }
        double const mean = sum / N;
        double const variance = squares / N - mean * mean;
        double const sig = std::sqrt(variance > 0.0 ? variance : squares / N);
        if (sig > 0.0)
            for (float &value : pass)
                value = static_cast<float>(value / sig * 2.83);
    }
    return llr;
}

bool decode(std::array<std::array<float, N>, 2> llrs,
            std::array<int8_t, N> const &expected, float threshold) {
    std::array<int8_t, K> decoded{};
    std::array<int8_t, N> cw{};
    std::array<float, N> bestLlr{};
    int bestChecks = M + 1;
    int passes = 0;
    auto tryBp = [&](std::array<float, N> const &input, BPOptions options,
                     bool remember) {
        auto const bp = bpdecode174(input, decoded, cw, options);
        if (remember && bp.bestChecks < bestChecks) {
            bestChecks = bp.bestChecks;
            bestLlr = input;
        }
        return bp.hardErrors >= 0 && bp.hardErrors < 60 &&
               cw == expected && checkCRC12(decoded);
    };
    for (int pass = 1; pass <= 4 && passes < 8; ++pass) {
        auto &llr = pass == 2 ? llrs[1] : llrs[0];
        if (pass == 3)
            std::fill(llrs[0].begin(), llrs[0].begin() + 24, 0.0f);
        if (pass == 4)
            std::fill(llrs[0].begin() + 24, llrs[0].begin() + 48, 0.0f);
        if (tryBp(llr, {}, true))
            return true;
        ++passes;
        if (passes >= 8)
            break;
        std::array<float, N> refined{};
        int confident = 0, uncertain = 0;
        js8::refineLlrsWithLdpcFeedback(llr, cw, threshold, refined,
                                         confident, uncertain);
        if (tryBp(refined, {}, true))
            return true;
        ++passes;
    }
    if (bestChecks <= BP_RESCUE_MAX_CHECKS)
        for (float scale : BP_RESCUE_LLR_SCALES) {
            BPOptions options;
            options.maxIterations = BP_RESCUE_ITERATIONS;
            options.earlyAbort = false;
            options.llrScale = scale;
            if (tryBp(bestLlr, options, false))
                return true;
        }
    return false;
}

struct Setting {
    char const *kind;
    float scale;
    float threshold;
};

std::vector<Setting> settings(std::string_view selection) {
    std::vector<Setting> result = {{"legacy", 1.0f, 0.25f},
                                   {"normalized", 1.0f, 0.25f},
                                   {"calibrated", 1.0f, 0.25f}};
    float const scales[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    float const thresholds[] = {0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.5f};
    if (selection == "final") {
        result.push_back({"calibrated", 1.5f, 0.0f});
        result.push_back({"calibrated", 2.0f, 0.0f});
        result.push_back({"calibrated", 2.0f, 0.1f});
        return result;
    }
    for (float scale : scales)
        for (float threshold : thresholds)
            if (selection == "sweep")
                result.push_back({"calibrated", scale, threshold});
    return result;
}
} // namespace

int main(int argc, char **argv) {
    int const trials = argc > 1 ? std::atoi(argv[1]) : 200;
    double const first = argc > 2 ? std::atof(argv[2]) : 4.0;
    double const last = argc > 3 ? std::atof(argv[3]) : 10.0;
    double const step = argc > 4 ? std::atof(argv[4]) : 0.5;
    std::string_view const selection = argc > 5 ? argv[5] : "default";
    bool const coherent = argc > 6 && std::string(argv[6]) == "coherent";
    if (trials <= 0 || step <= 0.0 || last < first)
        return 1;
    auto const variants = settings(selection);
    std::printf("kind,scale,erasure,snrDb,decoded,trials,wallMs,coherent\n");
    for (double snr = first; snr <= last + 1e-8; snr += step) {
        struct Count { int hits = 0; double ms = 0.0; };
        std::vector<Count> counts(variants.size());
        for (int trial = 0; trial < trials; ++trial) {
            Frame const frame = synthesize(snr, static_cast<unsigned>(trial));
            for (std::size_t k = 0; k < variants.size(); ++k) {
                auto const &v = variants[k];
                auto const start = std::chrono::steady_clock::now();
                std::array<std::array<float, N>, 2> llrs{};
                if (std::string_view(v.kind) == "legacy") {
                    llrs = legacy(frame, v.threshold);
                } else {
                    auto const result = Processor::process(
                        frame.magnitudes, frame.winners, v.threshold, false,
                        coherent ? frame.blend : std::nullopt, v.scale,
                        std::string_view(v.kind) == "normalized"
                            ? Processor::Normalization::FrameSigma283
                            : Processor::Normalization::None);
                    llrs = {result.llr0, result.llr1};
                }
                counts[k].hits += decode(llrs, frame.expected, v.threshold);
                counts[k].ms += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start).count();
            }
        }
        for (std::size_t k = 0; k < variants.size(); ++k)
            std::printf("%s,%.2f,%.2f,%.2f,%d,%d,%.2f,%d\n",
                        variants[k].kind, variants[k].scale,
                        variants[k].threshold, snr, counts[k].hits, trials,
                        counts[k].ms, coherent);
        std::fflush(stdout);
    }
}
