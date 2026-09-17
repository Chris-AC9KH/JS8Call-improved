// Diagnostic harness for JS8 whitening / noise estimation.
// This is a standalone command-line tool that synthesizes a Mode A frame,
// runs the decoder twice (whitening OFF vs ON), and prints a concise summary.
//
// With --coherent-benchmark it instead sweeps SNRs -24..-32 dB with the
// coherent data likelihood path ON vs OFF, reporting valid decodes,
// CRC-valid false positives (payload mismatches), wall time, and observed
// LDPC rescue syndromes (min/count; successes imply a zero syndrome).
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
    using Mode = ModeA; // Fixed to Mode A for this diagnostic.

    std::size_t
    synth_frame(double snrDb, unsigned seed = 0xBEEF)
    {
        constexpr char message[] = "TESTTEST1234"; // 12 chars

        int tones[NN] = {};
        JS8::encode(0, JS8::Costas::array(JS8::Costas::Type::ORIGINAL),
                    message, tones);

        constexpr double fs   = 12000.0;
        constexpr double baud = fs / Mode::NSPS;
        constexpr double f0   = 1000.0; // Hz

        std::vector<float> samples(Mode::NMAX, 0.0f);

        double snrLin   = std::pow(10.0, snrDb / 10.0);
        double noiseVar = (snrLin > 0.0) ? (1.0 / snrLin) : 1.0;
        std::mt19937 rng(seed);
        std::normal_distribution<double> noise(0.0, std::sqrt(noiseVar));

        for (int sym = 0; sym < NN; ++sym)
        {
            double freq = f0 + tones[sym] * baud;
            double dphi = 2.0 * M_PI * freq / fs;
            double phi  = 0.0;

            for (int n = 0; n < Mode::NSPS; ++n)
            {
                double s = std::cos(phi);
                phi = std::fmod(phi + dphi, 2.0 * M_PI);
                std::size_t idx = sym * Mode::NSPS + n;
                if (idx < samples.size()) samples[idx] = static_cast<float>(s + noise(rng));
            }
        }

        auto const count = std::min(samples.size(), std::size_t(JS8_RX_SAMPLE_SIZE));
        for (std::size_t i = 0; i < count; ++i)
        {
            dec_data.d2[i] = static_cast<std::int16_t>(std::round(samples[i] * 2000.0));
        }

        dec_data.params.kin   = static_cast<int>(count);
        dec_data.params.kposA = 0;
        dec_data.params.kszA  = static_cast<int>(count);
        dec_data.params.nsubmodes = 1 << 0; // ModeA only.
        dec_data.params.nfa = 0;
        dec_data.params.nfb = 4000;
        dec_data.params.nfqso = static_cast<int>(f0);
        dec_data.params.syncStats = false;
        dec_data.params.newdat = true;
        dec_data.params.kposB = dec_data.params.kszB = 0;
        dec_data.params.kposC = dec_data.params.kszC = 0;
        dec_data.params.kposE = dec_data.params.kszE = 0;
        dec_data.params.kposI = dec_data.params.kszI = 0;
        dec_data.params.nutc = code_time(0,0,0);
        return count;
    }

    struct Result
    {
        bool decoded = false;
        int  nhard   = -1;
        float snr    = -99.0f;
        std::vector<std::string> payloads;
        std::vector<int> bestChecks;
        long long wallMs = 0;
    };

    // Message-handler target for bestChecks capture. qInstallMessageHandler
    // requires a plain function pointer, so the active sink is set through
    // this pointer around each benchmark run.
    std::vector<int> *g_checksSink = nullptr;

    void benchmarkMessageHandler(QtMsgType type, QMessageLogContext const &ctx,
                                 QString const &msg) {
        (void)type;
        if (std::strcmp(ctx.category, "decoder.js8") != 0)
            return;
        std::string s = msg.toStdString();
        std::cerr << s << "\n";
        if (g_checksSink == nullptr)
            return;
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) {
            if (tok == "bestChecks") {
                int v = 0;
                if (iss >> v)
                    g_checksSink->push_back(v);
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
        auto prevHandler = qInstallMessageHandler(
            +[](QtMsgType, QMessageLogContext const &, QString const &) {});
        if (captureChecks) {
            QLoggingCategory::setFilterRules(
                QStringLiteral("decoder.js8.debug=true\n"));
            g_checksSink = &checks;
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
        g_checksSink = nullptr;
        qInstallMessageHandler(prevHandler);

        return r;
    }

    void
    run_coherent_benchmark()
    {
        constexpr char expected[] = "TESTTEST1234";
        std::printf("%-8s %-9s %-8s %-8s %-12s %-10s %s\n", "snrDb",
                    "coherent", "decoded", "exact", "falsePos", "wallMs",
                    "bestChecks");
        int trial = 0;
        for (double snrDb : {-24.0, -26.0, -28.0, -30.0, -32.0}) {
            for (bool coherent : {true, false}) {
                // Same noise seed for both settings: fair A/B comparison.
                synth_frame(snrDb, 0xBEEF + trial);
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
                // CRC-valid decodes carry a zero-syndrome codeword; failures
                // report the best observed rescue syndrome as min/count.
                if (r.decoded && r.bestChecks.empty())
                    checks = "0/success";
                std::printf("%-8.1f %-9s %-8d %-8d %-12d %-10lld %s\n", snrDb,
                            coherent ? "on" : "off",
                            r.decoded ? 1 : 0, exact, falsePos, r.wallMs,
                            checks.c_str());
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
    }

    constexpr double snrDb = 0.0;
    synth_frame(snrDb);

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
