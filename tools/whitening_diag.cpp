// Diagnostic harness for JS8 whitening / noise estimation.
// This is a standalone command-line tool that synthesizes a Mode A frame,
// runs the decoder twice (whitening OFF vs ON), and prints a concise summary.
//
// With --coherent-benchmark it instead sweeps SNRs -24..-32 dB across all
// five JS8 modes with the coherent data likelihood path ON vs OFF, reporting
// valid decodes, CRC-valid false positives (payload mismatches), wall time,
// peak coherent weight, and observed LDPC rescue syndromes (min/count;
// successes imply a zero syndrome).
//
// With --coherent-scenarios it runs end-to-end Mode A integration scenarios
// (clean/residual-frequency/drift/timing) with continuous phase and a
// non-round base frequency.
//
// The synthesizer keeps carrier phase continuous across all 79 symbols and
// uses a non-round base frequency so symbol boundaries are not exact integer
// carrier cycles. Timing impairments genuinely displace symbol boundaries in
// time (fractional source-coordinate evaluation), not just tone phase.
//
// Build example (adjust Qt/FFTW paths as needed):
//   g++ -std=c++17 -O2 -I.. tools/whitening_diag.cpp FrequencyTracker.cpp -lQt5Core -lfftw3f -lpthread
//
// Note: this links only the pieces needed for decoding; it defines the
// globals (dec_data, specData, fftw_mutex) that JS8 expects.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <mutex>
#include <thread>
#include <cstdlib>

#include <QCoreApplication>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QThread>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

// Provide the globals expected by JS8.cpp
struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

// Include implementation (in the diagnostic binary only).
#include "../JS8_Mode/JS8.cpp"

namespace
{
    struct SynthConfig
    {
        double snrDb = 0.0;
        double baseHz = 1003.7; // Non-round: avoids exact integer carrier
                                // cycles at symbol boundaries.
        double startPhase = 0.0;
        double freqOffsetHz = 0.0;
        double driftHzPerSec = 0.0;
        double timingShiftSmpl = 0.0; // Real symbol-boundary displacement in
                                      // 12 kHz samples (may be fractional or
                                      // negative); see synth_frame.
        unsigned seed = 0xBEEF;
    };

    template <typename Mode>
    std::size_t synth_frame(SynthConfig const &cfg, char const *message)
    {
        int tones[NN] = {};
        JS8::encode(0, JS8::Costas::array(Mode::NCOSTAS), message, tones);

        constexpr double fs   = 12000.0;
        constexpr double baud = fs / Mode::NSPS;

        std::vector<float> samples(Mode::NMAX, 0.0f);

        double snrLin   = std::pow(10.0, cfg.snrDb / 10.0);
        double noiseVar = (snrLin > 0.0) ? (1.0 / snrLin) : 1.0;
        std::mt19937 rng(cfg.seed);
        std::normal_distribution<double> noise(0.0, std::sqrt(noiseVar));

        // Real timing displacement: each output sample is evaluated at the
        // shifted source coordinate s = idx - timingShiftSmpl, so FSK symbol
        // boundaries genuinely move in time (fractional shifts supported,
        // positive and negative). Carrier phase accumulates sequentially and
        // stays continuous; the tone phase advances fractionally within the
        // symbol, which is exact for pure tones, with no phase reset at
        // boundaries (an integer tone advances by a multiple of 2*pi per
        // symbol). Out-of-frame edges extend the edge symbols.
        double phi = cfg.startPhase;

        for (std::size_t idx = 0; idx < samples.size(); ++idx)
        {
            double const s = static_cast<double>(idx) - cfg.timingShiftSmpl;
            long sym = static_cast<long>(
                std::floor(s / static_cast<double>(Mode::NSPS)));
            if (sym < 0)
                sym = 0;
            if (sym >= NN)
                sym = NN - 1;
            double const centerTime = (s >= 0.0 ? s : 0.0) / fs;
            double const freq = cfg.baseHz + tones[sym] * baud +
                                cfg.freqOffsetHz +
                                cfg.driftHzPerSec * centerTime;
            double const dphi = 2.0 * M_PI * freq / fs;
            phi = std::fmod(phi + dphi, 2.0 * M_PI);
            samples[idx] = static_cast<float>(std::cos(phi) + noise(rng));
        }

        auto const count = std::min(samples.size(), std::size_t(JS8_RX_SAMPLE_SIZE));
        for (std::size_t i = 0; i < count; ++i)
        {
            dec_data.d2[i] = static_cast<std::int16_t>(std::round(samples[i] * 2000.0));
        }

        return count;
    }

    // Configure decode params for one submode bit (0=A,1=B,2=C,3=E,4=I).
    void set_mode_params(int bit, int count, double baseHz)
    {
        dec_data.params.kin   = static_cast<int>(count);
        dec_data.params.kposA = dec_data.params.kszA = 0;
        dec_data.params.kposB = dec_data.params.kszB = 0;
        dec_data.params.kposC = dec_data.params.kszC = 0;
        dec_data.params.kposE = dec_data.params.kszE = 0;
        dec_data.params.kposI = dec_data.params.kszI = 0;
        dec_data.params.nsubmodes = 1 << bit;
        switch (bit)
        {
        case 0: dec_data.params.kposA = 0; dec_data.params.kszA = static_cast<int>(count); break;
        case 1: dec_data.params.kposB = 0; dec_data.params.kszB = static_cast<int>(count); break;
        case 2: dec_data.params.kposC = 0; dec_data.params.kszC = static_cast<int>(count); break;
        case 3: dec_data.params.kposE = 0; dec_data.params.kszE = static_cast<int>(count); break;
        case 4: dec_data.params.kposI = 0; dec_data.params.kszI = static_cast<int>(count); break;
        default: break;
        }
        dec_data.params.nfa = 0;
        dec_data.params.nfb = 4000;
        dec_data.params.nfqso = static_cast<int>(baseHz);
        dec_data.params.syncStats = false;
        dec_data.params.newdat = true;
        dec_data.params.nutc = code_time(0,0,0);
    }

    struct Result
    {
        bool decoded = false;
        int  nhard   = -1;
        float snr    = -99.0f;
        std::vector<std::string> payloads;
        std::vector<int> bestChecks;
        double maxAlpha = 0.0;
        long long wallMs = 0;
    };

    // Message-handler targets. qInstallMessageHandler requires a plain
    // function pointer, so active sinks are set through these pointers.
    std::vector<int> *g_checksSink = nullptr;
    double *g_alphaSink = nullptr;
    std::vector<std::string> *g_aidedSink = nullptr;

    void benchmarkMessageHandler(QtMsgType type, QMessageLogContext const &ctx,
                                 QString const &msg) {
        (void)type;
        if (std::strcmp(ctx.category, "decoder.js8") != 0)
            return;
        std::string s = msg.toStdString();
        std::cerr << s << "\n";
        if (s.find("Decoder-aided re-demod") != std::string::npos) {
            // Captured verbatim for aided A/B analysis; its
            // initialBestChecks/aidedBestChecks tokens must not pollute the
            // rescue-syndrome sink.
            if (g_aidedSink != nullptr)
                g_aidedSink->push_back(s);
            return;
        }
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) {
            if (tok == "bestChecks") {
                int v = 0;
                if (iss >> v && g_checksSink != nullptr)
                    g_checksSink->push_back(v);
            } else if (tok == "coherentWeight") {
                double v = 0.0;
                if (iss >> v && g_alphaSink != nullptr)
                    *g_alphaSink = std::max(*g_alphaSink, v);
            }
        }
    }

    Result
    run_decode(bool disableWhitening, bool coherentEnabled,
               bool captureChecks = false, bool redemodEnabled = true,
               std::vector<std::string> *aidedLines = nullptr,
               bool ldpcRescueEnabled = true)
    {
        if (disableWhitening) {
            ::setenv("JS8_DISABLE_WHITENING", "1", 1);
        } else {
            ::unsetenv("JS8_DISABLE_WHITENING");
        }
        if (coherentEnabled) {
            ::unsetenv("JS8_DISABLE_COHERENT_DATA");
        } else {
            ::setenv("JS8_DISABLE_COHERENT_DATA", "1", 1);
        }
        if (redemodEnabled) {
            ::unsetenv("JS8_DISABLE_REDEMOD");
        } else {
            ::setenv("JS8_DISABLE_REDEMOD", "1", 1);
        }
        if (ldpcRescueEnabled) {
            ::unsetenv("JS8_DISABLE_LDPC_RESCUE");
        } else {
            ::setenv("JS8_DISABLE_LDPC_RESCUE", "1", 1);
        }

        Result r;
        std::vector<int> checks;
        double maxAlpha = 0.0;
        std::vector<std::string> aided;
        auto prevHandler = qInstallMessageHandler(
            +[](QtMsgType, QMessageLogContext const &, QString const &) {});
        if (captureChecks) {
            QLoggingCategory::setFilterRules(
                QStringLiteral("decoder.js8.debug=true\n"));
            g_checksSink = &checks;
            g_alphaSink = &maxAlpha;
            g_aidedSink = aidedLines != nullptr ? aidedLines : &aided;
            qInstallMessageHandler(benchmarkMessageHandler);
        }
        auto const started = std::chrono::steady_clock::now();
        JS8::Decoder decoder;
        QEventLoop loop;

        QObject::connect(&decoder, &JS8::Decoder::decodeEvent,
                         [&r, &loop](JS8::Event::Variant const & ev)
        {
            if (auto dec = std::get_if<JS8::Event::Decoded>(&ev))
            {
                r.decoded = true;
                r.snr     = dec->snr;
                r.payloads.push_back(dec->data);
            }
            else if (auto fin = std::get_if<JS8::Event::DecodeFinished>(&ev))
            {
                // We don't have iterations from the decoder; use decoded count.
                r.nhard = static_cast<int>(fin->decoded);
                loop.quit();
            }
        });

        decoder.start(QThread::LowestPriority);
        decoder.decode();
        loop.exec();
        decoder.quit();
        r.wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count();
        r.bestChecks = checks;
        r.maxAlpha = maxAlpha;
        g_checksSink = nullptr;
        g_alphaSink = nullptr;
        g_aidedSink = nullptr;
        qInstallMessageHandler(prevHandler);

        return r;
    }

    void printBenchmarkRow(char const *mode, double snrDb, bool coherent,
                            Result const &r, char const *expected) {
        int exact = 0;
        for (auto const &payload : r.payloads)
            if (payload == expected)
                ++exact;
        int falsePos = static_cast<int>(r.payloads.size()) - exact;
        std::string checks = "n/a";
        if (!r.bestChecks.empty()) {
            int mn = *std::min_element(r.bestChecks.begin(),
                                       r.bestChecks.end());
            checks = std::to_string(mn) + "/" +
                     std::to_string(r.bestChecks.size());
        }
        // CRC-valid decodes carry a zero-syndrome codeword; failures report
        // the best observed rescue syndrome as min/count.
        if (r.decoded && r.bestChecks.empty())
            checks = "0/success";
        std::printf("%-6s %-8.1f %-9s %-8d %-8d %-12d %-10lld %-12.3f %s\n",
                    mode, snrDb, coherent ? "on" : "off",
                    r.decoded ? 1 : 0, exact, falsePos, r.wallMs,
                    r.maxAlpha, checks.c_str());
    }

    void printBenchmarkHeader() {
        std::printf("%-6s %-8s %-9s %-8s %-8s %-12s %-10s %-12s %s\n", "mode",
                    "snrDb", "coherent", "decoded", "exact", "falsePos",
                    "wallMs", "maxAlpha", "bestChecks");
    }

    template <typename Mode>
    void run_mode_benchmark(int bit, char const *name) {
        constexpr char expected[] = "TESTTEST1234";
        int trial = 0;
        for (double snrDb : {-24.0, -26.0, -28.0, -30.0, -32.0}) {
            for (bool coherent : {true, false}) {
                // Same noise seed for both settings: fair A/B comparison.
                SynthConfig cfg;
                cfg.snrDb = snrDb;
                cfg.seed = 0xBEEF + trial;
                cfg.startPhase = 0.7 + trial;
                auto count = synth_frame<Mode>(cfg, expected);
                set_mode_params(bit, static_cast<int>(count), cfg.baseHz);
                auto r = run_decode(false, coherent, true);
                printBenchmarkRow(name, snrDb, coherent, r, expected);
            }
            ++trial;
        }
    }

    void run_coherent_benchmark() {
        printBenchmarkHeader();
        run_mode_benchmark<ModeA>(0, "A");
        run_mode_benchmark<ModeB>(1, "B");
        run_mode_benchmark<ModeC>(2, "C");
        run_mode_benchmark<ModeE>(3, "E");
        run_mode_benchmark<ModeI>(4, "I");
    }

    struct Scenario {
        char const *name;
        double freqOffsetHz;
        double driftHzPerSec;
        double timingShiftSmpl;
    };

    // End-to-end integration scenarios at a decodable SNR: random absolute
    // phase, residual frequency offsets, linear drift, timing error, and the
    // no-drift case, all with continuous phase and a non-round base frequency.
    void run_coherent_scenarios() {
        constexpr char expected[] = "TESTTEST1234";
        constexpr Scenario scenarios[] = {
            {"clean", 0.0, 0.0, 0.0},
            {"freqPlus", 0.3, 0.0, 0.0},
            {"freqMinus", -0.3, 0.0, 0.0},
            {"driftPlus", 0.0, 0.01, 0.0},
            {"driftMinus", 0.0, -0.01, 0.0},
            {"timing", 0.0, 0.0, 0.02 * ModeA::NSPS},
            {"freqTimingPlus", 0.3, 0.0, 0.02 * ModeA::NSPS},
            {"freqTimingMinus", -0.3, 0.0, 0.02 * ModeA::NSPS},
        };
        std::printf("%-11s %-9s %-8s %-8s %-12s %-10s %-12s %s\n", "scenario",
                    "coherent", "decoded", "exact", "falsePos", "wallMs",
                    "maxAlpha", "bestChecks");
        int trial = 0;
        for (auto const &scenario : scenarios) {
            for (bool coherent : {true, false}) {
                SynthConfig cfg;
                cfg.snrDb = -12.0;
                cfg.seed = 0xCAFE + trial;
                cfg.startPhase = 2.1 + 0.3 * trial;
                cfg.freqOffsetHz = scenario.freqOffsetHz;
                cfg.driftHzPerSec = scenario.driftHzPerSec;
                cfg.timingShiftSmpl = scenario.timingShiftSmpl;
                auto count = synth_frame<ModeA>(cfg, expected);
                set_mode_params(0, static_cast<int>(count), cfg.baseHz);
                auto r = run_decode(false, coherent, true);
                int exact = 0;
                for (auto const &payload : r.payloads)
                    if (payload == expected)
                        ++exact;
                int falsePos =
                    static_cast<int>(r.payloads.size()) - exact;
                std::string checks = "n/a";
                if (!r.bestChecks.empty()) {
                    int mn = *std::min_element(r.bestChecks.begin(),
                                               r.bestChecks.end());
                    checks = std::to_string(mn) + "/" +
                             std::to_string(r.bestChecks.size());
                }
                if (r.decoded && r.bestChecks.empty())
                    checks = "0/success";
                std::printf("%-11s %-9s %-8d %-8d %-12d %-10lld %-12.3f %s\n",
                            scenario.name, coherent ? "on" : "off",
                            r.decoded ? 1 : 0, exact, falsePos, r.wallMs,
                            r.maxAlpha, checks.c_str());
            }
            ++trial;
        }
    }
    // Decoder-aided re-demodulation experiments.
    //
    // run_aided_rescue() sweeps impairments/noise around the decode cliff and
    // reports, per config, OFF (re-demod disabled) vs ON: decoded flag, best
    // observed rescue syndrome, aided attempts and CRC-accepted rescues. The
    // tentative codeword always comes from the actual failed LDPC output.
    //
    // run_aided_benchmark() compares re-demod ON vs OFF across modes and low
    // SNR points with multiple deterministic seeds: successful CRC decodes,
    // near-misses, aided attempts/rescues, false CRC payloads, wall time.

    int minRescueChecks(Result const &r) {
        if (r.bestChecks.empty())
            return r.decoded ? 0 : 99;
        return *std::min_element(r.bestChecks.begin(), r.bestChecks.end());
    }

    int countAidedToken(std::vector<std::string> const &lines,
                        char const *token, char const *value) {
        int n = 0;
        std::string const needle =
            std::string(token) + " " + value;
        for (auto const &line : lines)
            if (line.find(needle) != std::string::npos)
                ++n;
        return n;
    }

    struct AidedSweep {
        double snrDb;
        double freqOffsetHz;
        double driftHzPerSec;
        double timingFrac; // Fraction of Mode::NSPS.
        unsigned seed;
        double startPhase;
    };

    void run_aided_rescue() {
        // Payload diversity: the LDPC cliff is codeword-dependent, so several
        // distinct messages are tried per impairment cell.
        constexpr char const *messages[] = {
            "TESTTEST1234", "TESTTEST1235", "AAAAAAAAAAAA", "ZZZZZZZZZZZZ",
        };
        std::printf("%-12s %-6s %-7s %-7s %-7s %-8s %-4s %-9s %-4s %-9s %-9s %-8s %-8s\n",
                    "message", "snrDb", "freqHz", "drift", "timeFr", "seed",
                    "off", "offChecks", "onEx", "attempts", "rescues",
                    "onChecks", "wallMs");
        int rescued = 0;
        int nearMiss = 0;
        // Impairments target the first pass's weak spots: multi-sample
        // timing offsets near the tracker's pull-in limit, residual
        // frequencies between grid points, and drift beyond the aided box.
        for (char const *message : messages) {
        for (double snrDb : {-14.0, -16.0, -18.0}) {
            for (double freq : {0.35}) {
                for (double drift : {0.02}) {
                    for (double tfrac : {0.03}) {
                        for (unsigned seed : {0xC11u, 0xC22u, 0xC33u}) {
                            SynthConfig cfg;
                            cfg.snrDb = snrDb;
                            cfg.seed = seed;
                            cfg.startPhase = 1.1 + 0.7 * (seed & 0xF);
                            cfg.freqOffsetHz = freq;
                            cfg.driftHzPerSec = drift;
                            cfg.timingShiftSmpl = tfrac * ModeA::NSPS;
                            auto count = synth_frame<ModeA>(cfg, message);
                            set_mode_params(0, static_cast<int>(count),
                                            cfg.baseHz);
                            auto off = run_decode(false, true, true, false);
                            auto const offMin = minRescueChecks(off);
                            std::vector<std::string> aidedLines;
                            auto on = run_decode(false, true, true, true,
                                                 &aidedLines);
                            int exact = 0;
                            for (auto const &p : on.payloads)
                                if (p == message)
                                    ++exact;
                            int const attempts = countAidedToken(
                                aidedLines, "attempted", "1");
                            int const rescues = countAidedToken(
                                aidedLines, "crcAccepted", "1");
                            if (offMin >= 1 && offMin <= 4)
                                ++nearMiss;
                            if (rescues > 0)
                                ++rescued;
                            std::printf(
                                "%-12s %-6.1f %-7.2f %-7.3f %-7.3f 0x%-6X "
                                "%-4d %-9d %-4d %-9d %-9d %-8d %lld\n",
                                message, snrDb, freq, drift, tfrac, seed,
                                off.decoded ? 1 : 0, offMin, exact, attempts,
                                rescues, minRescueChecks(on),
                                on.wallMs + off.wallMs);
                            for (auto const &line : aidedLines)
                                std::printf("    aided: %s\n", line.c_str());
                        }
                    }
                }
            }
        }
        }
        std::printf("aided-rescue sweep: near-miss configs=%d rescued=%d\n",
                    nearMiss, rescued);
    }

    void run_aided_benchmark() {
        constexpr char expected[] = "TESTTEST1234";
        std::printf("%-6s %-8s %-9s %-8s %-8s %-12s %-10s %-9s %-9s %s\n",
                    "mode", "snrDb", "redemod", "decoded", "exact",
                    "falsePos", "wallMs", "attempts", "rescues",
                    "bestChecks");
        long totalWallOn = 0;
        long totalWallOff = 0;
        int decodedOn = 0;
        int decodedOff = 0;
        int rescues = 0;
        int attempts = 0;
        int falsePos = 0;
        auto const benchMode = [&](auto modeTag, int bit, char const *name) {
            using Mode = decltype(modeTag);
            int trial = 0;
            for (double snrDb : {-24.0, -26.0, -28.0, -30.0, -32.0}) {
                for (unsigned seed : {0xBEEFu, 0xBEF0u}) {
                    for (bool redemod : {false, true}) {
                        SynthConfig cfg;
                        cfg.snrDb = snrDb;
                        cfg.seed = seed + trial;
                        cfg.startPhase = 0.7 + trial;
                        auto count = synth_frame<Mode>(cfg, expected);
                        set_mode_params(bit, static_cast<int>(count),
                                        cfg.baseHz);
                        std::vector<std::string> aidedLines;
                        auto r = run_decode(false, true, true, redemod,
                                            &aidedLines);
                        int exact = 0;
                        for (auto const &p : r.payloads)
                            if (p == expected)
                                ++exact;
                        int const att = countAidedToken(aidedLines,
                                                        "attempted", "1");
                        int const res = countAidedToken(aidedLines,
                                                        "crcAccepted", "1");
                        attempts += att;
                        rescues += res;
                        falsePos += static_cast<int>(r.payloads.size()) -
                                    exact;
                        if (redemod) {
                            totalWallOn += r.wallMs;
                            decodedOn += r.decoded ? 1 : 0;
                        } else {
                            totalWallOff += r.wallMs;
                            decodedOff += r.decoded ? 1 : 0;
                        }
                        std::string checks = "n/a";
                        if (!r.bestChecks.empty()) {
                            int mn = *std::min_element(r.bestChecks.begin(),
                                                       r.bestChecks.end());
                            checks = std::to_string(mn) + "/" +
                                     std::to_string(r.bestChecks.size());
                        }
                        if (r.decoded && r.bestChecks.empty())
                            checks = "0/success";
                        std::printf(
                            "%-6s %-8.1f %-9s %-8d %-8d %-12d %-10lld "
                            "%-9d %-9d %s\n",
                            name, snrDb, redemod ? "on" : "off",
                            r.decoded ? 1 : 0, exact,
                            static_cast<int>(r.payloads.size()) - exact,
                            r.wallMs, att, res, checks.c_str());
                    }
                    ++trial;
                }
            }
        };
        benchMode(ModeA{}, 0, "A");
        benchMode(ModeB{}, 1, "B");
        benchMode(ModeC{}, 2, "C");
        benchMode(ModeE{}, 3, "E");
        benchMode(ModeI{}, 4, "I");
        std::printf("aided-benchmark: decoded off=%d on=%d attempts=%d "
                    "rescues=%d falsePos=%d wallMs off=%ld on=%ld\n",
                    decodedOff, decodedOn, attempts, rescues, falsePos,
                    totalWallOff, totalWallOn);
    }

    // Rescue-budget behavior test (Fix 9.E): run fixed near-miss-prone cells
    // with LDPC rescue enabled vs disabled and verify per aided attempt:
    // - extended scales run only when rescue is enabled and budget remains
    // - an aided scale group consumes exactly one budget unit, never more
    // - budget never increases and is unchanged when no scales run
    // - re-demodulation (normal BP, always allowed) is observable via a set
    //   aidedBestChecks independently of the scale group
    // Returns the number of invariant violations (0 = PASS).
    int verifyBudgetLine(std::string const &line, bool rescueEnabled) {
        auto const field = [&](char const *key, long long fallback) {
            std::istringstream iss(line);
            std::string tok;
            while (iss >> tok) {
                if (tok == key) {
                    long long v = fallback;
                    if (iss >> v)
                        return v;
                    return fallback;
                }
            }
            return fallback;
        };
        long long const scales = field("aidedScales", -1);
        long long const before = field("budgetBefore", -1);
        long long const after = field("budgetAfter", -2);
        long long const bestChecks = field("aidedBestChecks", -999);
        int violations = 0;
        auto const violation = [&](char const *what) {
            std::printf("    BUDGET-VIOLATION %s: %s\n", what, line.c_str());
            ++violations;
        };
        if (scales < 0 || scales > 3)
            violation("scales out of range");
        if (before < 0)
            violation("missing budgetBefore");
        if (!rescueEnabled && scales != 0)
            violation("scales ran with rescue disabled");
        if (scales > 0) {
            if (before <= 0)
                violation("scales ran with no budget");
            if (after != before - 1)
                violation("group did not consume exactly one unit");
        } else if (after != before) {
            violation("budget changed without scales");
        }
        // Re-demodulation ran iff LDPC saw the fresh LLRs.
        bool const redemodRan = bestChecks >= 0;
        if (redemodRan && scales < 0)
            violation("re-demod without scale accounting");
        (void)redemodRan;
        return violations;
    }

    void run_aided_budget() {
        constexpr char const *messages[] = {
            "TESTTEST1234", "TESTTEST1235", "AAAAAAAAAAAA", "ZZZZZZZZZZZZ",
        };
        int violations = 0;
        int redemods = 0;
        int scaleGroups = 0;
        int normalOnlyAtZeroBudget = 0;
        for (bool rescueEnabled : {true, false}) {
            for (char const *message : messages) {
                for (double snrDb : {-16.0, -17.0, -18.0}) {
                    for (unsigned seed : {0xC11u, 0xC22u, 0xC33u}) {
                        SynthConfig cfg;
                        cfg.snrDb = snrDb;
                        cfg.seed = seed;
                        cfg.startPhase = 1.1 + 0.7 * (seed & 0xF);
                        cfg.freqOffsetHz = 0.35;
                        cfg.driftHzPerSec = 0.02;
                        cfg.timingShiftSmpl = 0.03 * ModeA::NSPS;
                        auto count = synth_frame<ModeA>(cfg, message);
                        set_mode_params(0, static_cast<int>(count),
                                        cfg.baseHz);
                        std::vector<std::string> aidedLines;
                        auto r = run_decode(false, true, true, true,
                                            &aidedLines, rescueEnabled);
                        (void)r;
                        for (auto const &line : aidedLines) {
                            std::istringstream iss(line);
                            std::string tok;
                            long long scales = -1;
                            long long before = -1;
                            long long best = -999;
                            while (iss >> tok) {
                                if (tok == "aidedScales")
                                    iss >> scales;
                                else if (tok == "budgetBefore")
                                    iss >> before;
                                else if (tok == "aidedBestChecks")
                                    iss >> best;
                            }
                            if (best >= 0)
                                ++redemods;
                            if (scales > 0)
                                ++scaleGroups;
                            if (best >= 0 && scales == 0 && before == 0)
                                ++normalOnlyAtZeroBudget;
                            violations += verifyBudgetLine(line,
                                                           rescueEnabled);
                        }
                    }
                }
            }
        }
        std::printf("aided-budget: re-demodulations=%d scaleGroups=%d "
                    "normalOnlyAtZeroBudget=%d violations=%d %s\n",
                    redemods, scaleGroups, normalOnlyAtZeroBudget, violations,
                    violations == 0 ? "PASS" : "FAIL");
    }
}

int
main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--coherent-benchmark") {
            run_coherent_benchmark();
            return 0;
        }
        if (std::string(argv[i]) == "--coherent-scenarios") {
            run_coherent_scenarios();
            return 0;
        }
        if (std::string(argv[i]) == "--aided-rescue") {
            run_aided_rescue();
            return 0;
        }
        if (std::string(argv[i]) == "--aided-benchmark") {
            run_aided_benchmark();
            return 0;
        }
        if (std::string(argv[i]) == "--aided-budget") {
            run_aided_budget();
            return 0;
        }
    }

    SynthConfig cfg;
    cfg.snrDb = 0.0;
    auto count = synth_frame<ModeA>(cfg, "TESTTEST1234");
    set_mode_params(0, static_cast<int>(count), cfg.baseHz);

    auto off = run_decode(true, true);
    auto on  = run_decode(false, true);

    std::cout << "Whitening OFF: decoded=" << off.decoded
              << " iterations=" << off.nhard
              << " metric=" << off.snr << "\n";

    std::cout << "Whitening ON:  decoded=" << on.decoded
              << " iterations=" << on.nhard
              << " metric=" << on.snr << "\n";

    return 0;
}
