/**
 * @file decoder_comparison_benchmark.cpp
 * @brief Paired waveform benchmark for master and experimental JS8 decoders.
 *
 * Compile twice with target-specific include paths and moc files. Define
 * JS8_COMPARISON_SOURCE to the absolute target JS8.cpp path and
 * JS8_COMPARISON_LABEL to identify the build. Each trial resets decoder state;
 * default combining remains active within that trial. Timing excludes waveform
 * synthesis, FFT plan construction and destruction. Optional work has no live
 * queue deadline in this synchronous DSP benchmark.
 *
 * Usage: decoder_comparison_benchmark sensitivity|weak-reference|weak-display|
 *        impairments|collisions|noise|acquisition|point
 *        [trials=100] [A|B|C|E|I|all=all]
 * A single "point" accepts additional [binSnr=8] [firstTrial=0] arguments.
 * binSnrDb is nominal matched-symbol SNR. referenceSnrDb uses 2500 Hz of
 * white-noise power; reportedSnrDb is the decoder estimate for a correct
 * payload (NaN on a miss), not a label assigned to failed trials.
 */
#include <QCoreApplication>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;
Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

#ifdef JS8_COMPARISON_SOURCE
#include JS8_COMPARISON_SOURCE
#else
#include "../JS8_Mode/JS8.cpp"
#endif
#ifndef JS8_COMPARISON_LABEL
#define JS8_COMPARISON_LABEL "working"
#endif

namespace {
constexpr double rate = 12000.0;
constexpr double referenceBandwidth = 2500.0;
constexpr std::array<char const *, 6> messages = {
    "TESTTEST1234", "TESTTEST1235", "AAAAAAAAAAAA", "ZZZZZZZZZZZZ",
    "ABCDEFGH1234", "0123456789AB"};

struct Scenario {
    char const *name;
    double binSnr;
    int stations = 1;
    double spacingBaud = 0.0;
    double powerRangeDb = 0.0;
    double timingSymbols = 0.0;
    double driftHzPerSecond = 0.0;
};

double cpuMs() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}

struct Input {
    std::uint64_t hash = 14695981039346656037ull;
    unsigned expectedMask = 0;
    int clipped = 0;
};

template <typename Mode>
Input synthesize(Scenario const &scenario, unsigned trial) {
    std::vector<double> samples(Mode::NMAX, 0.0);
    double const sigma = std::sqrt(Mode::NSPS /
        (4.0 * std::pow(10.0, scenario.binSnr / 10.0)));
    double const gain = 2000.0 / std::max(1.0, sigma);
    double const baud = rate / Mode::NSPS;
    std::mt19937 rng(0x3722026u + 7919u * trial);
    std::uniform_real_distribution<double> phaseDist(-std::numbers::pi,
                                                    std::numbers::pi);
    Input input;
    for (int station = 0; station < scenario.stations; ++station) {
        unsigned const message = (trial + station) % messages.size();
        input.expectedMask |= 1u << message;
        std::array<int, NN> tones{};
        JS8::encode(0, JS8::Costas::array(Mode::NCOSTAS),
                    messages[message], tones.data());
        double const amplitude = std::pow(10.0,
            -scenario.powerRangeDb * station /
            (20.0 * std::max(1, scenario.stations - 1)));
        double const frequency = 1503.7 + station * scenario.spacingBaud * baud;
        int const start = static_cast<int>(std::round(Mode::ASTART * rate +
            (scenario.timingSymbols + station * 0.03) * Mode::NSPS));
        double const initialPhase = phaseDist(rng);
        std::complex<double> phase = std::polar(1.0, initialPhase);
        std::complex<double> const chirp = std::polar(1.0,
            2.0 * std::numbers::pi * scenario.driftHzPerSecond / (rate * rate));
        for (int symbol = 0; symbol < NN; ++symbol) {
            double const time = symbol * Mode::NSPS / rate;
            std::complex<double> step = std::polar(1.0,
                2.0 * std::numbers::pi *
                (frequency + tones[symbol] * baud +
                 scenario.driftHzPerSecond * time) / rate);
            for (int sample = 0; sample < Mode::NSPS; ++sample) {
                int const index = start + symbol * Mode::NSPS + sample;
                phase *= step;
                step *= chirp;
                if (index < 0 || index >= Mode::NMAX)
                    continue;
                double value = amplitude * phase.real();
                double const t = (symbol * Mode::NSPS + sample) / rate;
                if (std::string_view(scenario.name) == "phase_mod") {
                    double const modulation = 1.5 * std::sin(
                        2.0 * std::numbers::pi * t / (8.0 * Mode::NSPS / rate) +
                        initialPhase);
                    value = amplitude * std::real(phase *
                        std::polar(1.0, modulation));
                } else if (std::string_view(scenario.name) == "phase_jump") {
                    bool const pilot = symbol < 7 ||
                        (symbol >= 36 && symbol < 43) || symbol >= 72;
                    if (!pilot)
                        value = -value;
                } else if (std::string_view(scenario.name) == "fade") {
                    value *= 0.6 + 0.4 * std::sin(
                        2.0 * std::numbers::pi * t / (20.0 * Mode::NSPS / rate) +
                        initialPhase);
                }
                samples[index] += value;
            }
        }
    }
    std::normal_distribution<double> noise(0.0, sigma);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        long const quantized = std::lround((samples[i] + noise(rng)) * gain);
        input.clipped += quantized < -32768 || quantized > 32767;
        dec_data.d2[i] = static_cast<std::int16_t>(
            std::clamp(quantized, -32768l, 32767l));
        auto const bits = static_cast<std::uint16_t>(dec_data.d2[i]);
        for (unsigned byte : {bits & 255u, static_cast<unsigned>(bits >> 8u)}) {
            input.hash ^= byte;
            input.hash *= 1099511628211ull;
        }
    }
    return input;
}

template <typename Mode>
void runScenario(char const *suite, char const *modeName,
                 Scenario const &scenario, int trials, unsigned firstTrial = 0) {
    for (int trial = 0; trial < trials; ++trial) {
        unsigned const inputTrial = firstTrial + static_cast<unsigned>(trial);
        auto const input = synthesize<Mode>(scenario, inputTrial);
        dec_data.params.nfa = 100;
        dec_data.params.nfb = 5000;
        dec_data.params.nfqso = 1504;
        dec_data.params.newdat = true;
        dec_data.params.syncStats = false;
        dec_data.params.nutc = code_time(0, 0, 0);
        auto decoder = std::make_unique<DecodeMode<Mode>>();
        unsigned decodedMask = 0;
        double reportedSnr = std::numeric_limits<double>::quiet_NaN();
        std::set<std::string> falsePayloads;
        auto const began = std::chrono::steady_clock::now();
        double const beganCpu = cpuMs();
        (*decoder)(dec_data, 0, Mode::NMAX, [&](JS8::Event::Variant const &event) {
            if (auto decoded = std::get_if<JS8::Event::Decoded>(&event)) {
                auto const found = std::find(messages.begin(), messages.end(),
                                             decoded->data);
                unsigned const index = static_cast<unsigned>(
                    std::distance(messages.begin(), found));
                if (found != messages.end() &&
                    (input.expectedMask & (1u << index))) {
                    decodedMask |= 1u << index;
                    if (!std::isfinite(reportedSnr) || decoded->snr > reportedSnr)
                        reportedSnr = decoded->snr;
                } else
                    falsePayloads.insert(decoded->data);
            }
        });
        double const elapsedCpu = cpuMs() - beganCpu;
        double const wall = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - began).count();
        double const referenceSnr = scenario.binSnr -
            10.0 * std::log10(Mode::NSPS * referenceBandwidth / rate);
        std::printf("%s,%s,%s,%s,%.2f,%d,%.2f,%.1f,%u,%u,%u,%zu,%d,%.3f,%.3f,%016llx,%.2f,%.1f\n",
            JS8_COMPARISON_LABEL, suite, modeName, scenario.name, scenario.binSnr,
            scenario.stations, scenario.spacingBaud, scenario.powerRangeDb,
            inputTrial, input.expectedMask, decodedMask, falsePayloads.size(),
            input.clipped, elapsedCpu, wall,
            static_cast<unsigned long long>(input.hash), referenceSnr, reportedSnr);
        std::fflush(stdout);
        if (std::string_view(suite) == "acquisition" &&
            (decodedMask != input.expectedMask || !falsePayloads.empty()))
            throw std::runtime_error("acquisition regression");
    }
}

template <typename Mode>
void runMode(char const *suite, char const *mode, int trials,
             double pointSnr, unsigned firstTrial) {
    if (std::string_view(suite) == "point") {
        runScenario<Mode>(suite, mode, {"awgn", pointSnr}, trials, firstTrial);
    } else if (std::string_view(suite) == "acquisition") {
        runScenario<Mode>(suite, mode, {"nominal_start", 20.0}, trials);
        runScenario<Mode>(suite, mode,
            {"zero_start", 20.0, 1, 0.0, 0.0,
             -Mode::ASTART * rate / Mode::NSPS}, trials);
        runScenario<Mode>(suite, mode,
            {"late_start", 20.0, 1, 0.0, 0.0, 0.25}, trials);
        if constexpr (Mode::NSUBMODE == ModeA::NSUBMODE)
            runScenario<Mode>(suite, mode, {"normal_regression", 10.0}, 2);
    } else if (std::string_view(suite) == "weak-reference") {
        double const processingGain =
            10.0 * std::log10(Mode::NSPS * referenceBandwidth / rate);
        for (double snr : {-32.0, -30.0, -28.0, -26.0, -24.0, -22.0, -20.0, -18.0})
            runScenario<Mode>(suite, mode, {"awgn", snr + processingGain}, trials);
    } else if (std::string_view(suite) == "weak-display") {
        for (double snr : {2.0, 3.0, 4.0, 4.5, 5.0, 5.5, 6.0, 7.0, 8.0})
            runScenario<Mode>(suite, mode, {"awgn", snr}, trials);
    } else if (std::string_view(suite) == "sensitivity") {
        for (double snr : {4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0})
            runScenario<Mode>(suite, mode, {"awgn", snr}, trials);
    } else if (std::string_view(suite) == "impairments") {
        for (char const *name : {"clean", "phase_jump", "phase_mod", "fade"})
            runScenario<Mode>(suite, mode, {name, 12.0}, trials);
        runScenario<Mode>(suite, mode,
            {"drift_timing", 8.0, 1, 0.0, 0.0, 0.12, 0.04}, trials);
    } else if (std::string_view(suite) == "collisions") {
        for (int count : {3, 6})
            for (double spacing : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0})
                for (double power : {0.0, 12.0})
                    runScenario<Mode>(suite, mode,
                        {"overlap", power == 0.0 ? 12.0 : 22.0,
                         count, spacing, power}, trials);
    } else if (std::string_view(suite) == "noise") {
        runScenario<Mode>(suite, mode, {"noise", 6.0, 0}, trials);
    } else {
        throw std::invalid_argument("unknown benchmark suite");
    }
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2)
        return 1;
    int const trials = argc > 2 ? std::atoi(argv[2]) : 100;
    if (trials < 1)
        return 1;
    char const *suite = argv[1];
    std::string_view const mode = argc > 3 ? argv[3] : "all";
    double const pointSnr = argc > 4 ? std::atof(argv[4]) : 8.0;
    unsigned const firstTrial = argc > 5
        ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)) : 0;
    std::printf("build,suite,mode,scenario,binSnrDb,stations,spacingBaud,powerRangeDb,"
                "trial,expectedMask,decodedMask,falsePayloads,clipped,cpuMs,wallMs,inputHash,"
                "referenceSnrDb,reportedSnrDb\n");
    if (mode == "all" || mode == "A") runMode<ModeA>(suite, "A", trials, pointSnr, firstTrial);
    if (mode == "all" || mode == "B") runMode<ModeB>(suite, "B", trials, pointSnr, firstTrial);
    if (mode == "all" || mode == "C") runMode<ModeC>(suite, "C", trials, pointSnr, firstTrial);
    if (mode == "all" || mode == "E") runMode<ModeE>(suite, "E", trials, pointSnr, firstTrial);
    if (mode == "all" || mode == "I") runMode<ModeI>(suite, "I", trials, pointSnr, firstTrial);
}
