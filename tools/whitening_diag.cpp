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
// carrier cycles.
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
        double timingShiftSmpl = 0.0; // Fractional symbol-start displacement.
        unsigned seed = 0xBEEF;
    };

    template <typename Mode>
    std::size_t synth_frame(SynthConfig const &cfg)
    {
        constexpr char message[] = "TESTTEST1234"; // 12 chars

        int tones[NN] = {};
        JS8::encode(0, JS8::Costas::array(Mode::NCOSTAS), message, tones);

        constexpr double fs   = 12000.0;
        constexpr double baud = fs / Mode::NSPS;

        std::vector<float> samples(Mode::NMAX, 0.0f);

        double snrLin   = std::pow(10.0, cfg.snrDb / 10.0);
        double noiseVar = (snrLin > 0.0) ? (1.0 / snrLin) : 1.0;
        std::mt19937 rng(cfg.seed);
        std::normal_distribution<double> noise(0.0, std::sqrt(noiseVar));

        // Continuous carrier phase across all 79 symbols: never reset per
        // symbol, so coherent structure is preserved end to end. A fractional
        // timing shift displaces sampling instants, which for a constant
        // within-symbol frequency is exactly a phase advance dphi*shift.
        double phi = cfg.startPhase;

        for (int sym = 0; sym < NN; ++sym)
        {
            double const centerTime =
                (sym * Mode::NSPS + Mode::NSPS / 2) / fs;
            double const freq = cfg.baseHz + tones[sym] * baud +
                                cfg.freqOffsetHz +
                                cfg.driftHzPerSec * centerTime;
            double const dphi = 2.0 * M_PI * freq / fs;

            for (int n = 0; n < Mode::NSPS; ++n)
            {
                double s = std::cos(phi + dphi * cfg.timingShiftSmpl);
                phi      = std::fmod(phi + dphi, 2.0 * M_PI);
                std::size_t idx = sym * Mode::NSPS + n;
                if (idx < samples.size()) samples[idx] = static_cast<float>(s + noise(rng));
            }
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

    void benchmarkMessageHandler(QtMsgType type, QMessageLogContext const &ctx,
                                 QString const &msg) {
        (void)type;
        if (std::strcmp(ctx.category, "decoder.js8") != 0)
            return;
        std::string s = msg.toStdString();
        std::cerr << s << "\n";
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
               bool captureChecks = false)
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

        Result r;
        std::vector<int> checks;
        double maxAlpha = 0.0;
        auto prevHandler = qInstallMessageHandler(
            +[](QtMsgType, QMessageLogContext const &, QString const &) {});
        if (captureChecks) {
            QLoggingCategory::setFilterRules(
                QStringLiteral("decoder.js8.debug=true\n"));
            g_checksSink = &checks;
            g_alphaSink = &maxAlpha;
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
                auto count = synth_frame<Mode>(cfg);
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
                auto count = synth_frame<ModeA>(cfg);
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
    }

    SynthConfig cfg;
    cfg.snrDb = 0.0;
    auto count = synth_frame<ModeA>(cfg);
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
