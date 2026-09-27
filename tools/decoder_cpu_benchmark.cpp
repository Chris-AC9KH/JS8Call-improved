// Reproducible multi-mode decoder CPU replay. Compile twice from identical
// sources. For the pre-optimization baseline add all five macros:
// -DJS8_BENCHMARK_UNOPTIMIZED_HOTSPOTS
// -DJS8_BENCHMARK_ORIGINAL_COARSE_SYNC
// -DJS8_BENCHMARK_DISABLE_BP_CACHE
// -DJS8_BENCHMARK_FULL_BP_TANH
// -DJS8_BENCHMARK_ORIGINAL_CHECK_TO_BIT_LOOP
// To isolate reciprocal BP edge lookup, add only
// -DJS8_BENCHMARK_LINEAR_BP_EDGES to the reference build.
// -DJS8_BENCHMARK_MAPPED_BP_CHECK_TO_BIT tests mapping the reverse lookup.
// -DJS8_BENCHMARK_ORIGINAL_CHECK_TO_BIT_LOOP isolates check-centric BP.
// -DJS8_BENCHMARK_ORIGINAL_RANK_SCAN compares the original candidate scan.
// -DJS8_BENCHMARK_VERIFY_RANK_SCAN checks every unnormalized score bitwise.
// -DJS8_CPU_BENCHMARK instruments the hot paths identically in both builds.
// -DJS8_BENCHMARK_VERIFY_COARSE_SYNC compares every cached coarse metric with
// the original implementation bit-for-bit (verification only, not timing).
// Build (replace the moc_JS8.cpp path if the autogen layout differs):
// clang++ -std=c++20 -O2 -I. -Ibuild/JS8Call_autogen/include \
//   $(pkg-config --cflags Qt6Core fftw3f) tools/decoder_cpu_benchmark.cpp \
//   JS8_Mode/FrequencyTracker.cpp \
//   build/JS8Call_autogen/33ZB6LRKMI/moc_JS8.cpp \
//   $(pkg-config --libs Qt6Core fftw3f) -o /tmp/decoder_cpu_benchmark
// Run from the repository root:
// decoder_cpu_benchmark [crowded|noise] [seconds] [diag] [autosync]
//
// The benchmark advances a 60-second sample ring on a virtual 1-second clock
// and uses the production five-mode decode worker for each ready mode set.
// CPU/duty are process CPU during decoding, excluding sample synthesis and
// worker FFT-plan startup. Since virtual time is NOT wall-clock paced, lag is
// an estimated deadline risk, not a measurement of the live UI's queue.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

#include <QCoreApplication>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QThread>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;
Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

// Referenced only by the benchmark-instrumented decoder translation unit.
std::atomic<std::uint64_t> benchmarkRefineCalls{0};
std::atomic<std::uint64_t> benchmarkRefineNanos{0};
std::atomic<std::uint64_t> benchmarkCoherentCalls{0};
std::atomic<std::uint64_t> benchmarkCoherentNanos{0};
std::atomic<std::uint64_t> benchmarkSyncCalls{0};
std::atomic<std::uint64_t> benchmarkSyncNanos{0};
std::atomic<std::uint64_t> benchmarkBpCalls{0};
std::atomic<std::uint64_t> benchmarkBpNanos{0};
std::atomic<std::uint64_t> benchmarkSyncMismatches{0};
std::atomic<std::uint64_t> benchmarkCandidateGenCalls{0};
std::atomic<std::uint64_t> benchmarkCandidateGenNanos{0};
std::atomic<std::uint64_t> benchmarkBasebandCalls{0};
std::atomic<std::uint64_t> benchmarkBasebandNanos{0};
std::atomic<std::uint64_t> benchmarkDownsampleCalls{0};
std::atomic<std::uint64_t> benchmarkDownsampleNanos{0};
std::atomic<std::uint64_t> benchmarkDemodCalls{0};
std::atomic<std::uint64_t> benchmarkDemodNanos{0};
std::atomic<std::uint64_t> benchmarkToneBinsCalls{0};
std::atomic<std::uint64_t> benchmarkToneBinsNanos{0};
std::atomic<std::uint64_t> benchmarkDecodeCalls{0};
std::atomic<std::uint64_t> benchmarkDecodeNanos{0};
std::atomic<std::uint64_t> benchmarkReferenceCalls{0};
std::atomic<std::uint64_t> benchmarkReferenceNanos{0};
std::atomic<std::uint64_t> benchmarkSicCalls{0};
std::atomic<std::uint64_t> benchmarkSicNanos{0};
std::atomic<std::uint64_t> benchmarkSpectrumCalls{0};
std::atomic<std::uint64_t> benchmarkSpectrumNanos{0};
std::atomic<std::uint64_t> benchmarkBaselineCalls{0};
std::atomic<std::uint64_t> benchmarkBaselineNanos{0};
std::atomic<std::uint64_t> benchmarkBpProfiledCalls{0};
std::atomic<std::uint64_t> benchmarkBpVariableNanos{0};
std::atomic<std::uint64_t> benchmarkBpSyndromeNanos{0};
std::atomic<std::uint64_t> benchmarkBpBitToCheckNanos{0};
std::atomic<std::uint64_t> benchmarkBpTanhNanos{0};
std::atomic<std::uint64_t> benchmarkBpCheckToBitNanos{0};
std::atomic<std::uint64_t> benchmarkRankScoreCalls{0};
std::atomic<std::uint64_t> benchmarkRankScoreNanos{0};
std::atomic<std::uint64_t> benchmarkRankIndexCalls{0};
std::atomic<std::uint64_t> benchmarkRankIndexNanos{0};
std::atomic<std::uint64_t> benchmarkRankMismatches{0};
std::atomic<std::uint64_t> benchmarkRankComparedScores{0};

struct BenchmarkScope {
    std::atomic<std::uint64_t> &calls;
    std::atomic<std::uint64_t> &nanoseconds;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    ~BenchmarkScope() {
        ++calls;
        nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    }
};

#ifdef JS8_DIAG_DECODER_SOURCE
#include JS8_DIAG_DECODER_SOURCE
#else
#include "../JS8_Mode/JS8.cpp"
#endif

namespace {
constexpr int rate = JS8_RX_SAMPLE_RATE;

std::vector<int16_t> readWav(std::string const &filename) {
    std::ifstream input(filename, std::ios::binary);
    if (!input)
        throw std::runtime_error("missing fixture: " + filename);
    auto read32 = [&] {
        unsigned char b[4];
        if (!input.read(reinterpret_cast<char *>(b), 4))
            throw std::runtime_error("bad WAV header: " + filename);
        return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
               (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
    };
    char riff[4], wave[4];
    input.read(riff, 4);
    (void)read32();
    input.read(wave, 4);
    if (std::memcmp(riff, "RIFF", 4) || std::memcmp(wave, "WAVE", 4))
        throw std::runtime_error("not a RIFF WAV: " + filename);
    bool formatOk = false;
    for (;;) {
        char tag[4];
        if (!input.read(tag, 4))
            break;
        std::uint32_t const bytes = read32();
        if (!std::memcmp(tag, "fmt ", 4)) {
            std::array<unsigned char, 16> fmt{};
            if (bytes < fmt.size() ||
                !input.read(reinterpret_cast<char *>(fmt.data()), fmt.size()))
                throw std::runtime_error("bad WAV format");
            auto word = [&](int i) { return fmt[i] | (fmt[i + 1] << 8); };
            std::uint32_t const sampleRate =
                std::uint32_t(fmt[4]) | (std::uint32_t(fmt[5]) << 8) |
                (std::uint32_t(fmt[6]) << 16) | (std::uint32_t(fmt[7]) << 24);
            formatOk = word(0) == 1 && word(2) == 1 &&
                       sampleRate == rate && word(14) == 16;
            input.seekg(bytes - fmt.size(), std::ios::cur);
        } else if (!std::memcmp(tag, "data", 4)) {
            if (!formatOk || bytes % 2 != 0)
                throw std::runtime_error("expected mono 12-kHz 16-bit PCM");
            std::vector<int16_t> pcm(bytes / 2);
            if (!input.read(reinterpret_cast<char *>(pcm.data()), bytes))
                throw std::runtime_error("truncated WAV: " + filename);
            return pcm;
        } else {
            input.seekg(bytes, std::ios::cur);
        }
        if (bytes & 1)
            input.seekg(1, std::ios::cur);
    }
    throw std::runtime_error("missing WAV data chunk: " + filename);
}

std::vector<int16_t> makeSamples(bool crowded, int seconds) {
    auto const normal = readWav("media/tests/A_2_9.wav");
    auto const slow = readWav("media/tests/E_2_1.wav");
    std::vector<int16_t> samples(static_cast<std::size_t>(seconds) * rate);
    std::mt19937 rng(0xBEEFu);
    std::normal_distribution<double> noise(0.0, crowded ? 670.0 : 860.0);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        double value = noise(rng);
        if (crowded) {
            value += 0.26 * normal[i % normal.size()];
            value += 0.42 * slow[(i + rate * 3) % slow.size()];
            double const t = static_cast<double>(i) / rate;
            value += 110.0 * std::cos(2.0 * std::numbers::pi * 1793.2 * t);
            value += 70.0 * std::cos(2.0 * std::numbers::pi * 2643.4 * t);
        }
        samples[i] = static_cast<int16_t>(
            std::clamp(std::lround(value), -32768l, 32767l));
    }
    return samples;
}

struct ModeSchedule {
    int period;
    int required;
    int samplesNeeded;
    bool fastReady;
    int lastStart = -1;
    int lastCycle = -1;
};

struct InputEvent {
    int tick; // 0.1 s after start
    std::array<int, 5> pos{};
    std::array<int, 5> size{};
    int modes = 0;
};

std::vector<InputEvent> schedule(int seconds, bool autosync) {
    std::array<ModeSchedule, 5> modes = {{
        {15, 79 * ModeA::NSPS, 0, false},
        {10, 79 * ModeB::NSPS, 0, false},
        {6, 0, 79 * ModeC::NSPS + rate * 6 / 10, true},
        {30, 79 * ModeE::NSPS, 0, false},
        {4, 0, 79 * ModeI::NSPS + rate * 6 / 10, true},
    }};
    std::vector<InputEvent> events;
    int const buffer = JS8_RX_SAMPLE_SIZE;
    for (int tick = 1; tick <= seconds * 10; ++tick) {
        int const k = (tick * (rate / 10)) % buffer;
        InputEvent event;
        event.tick = tick;
        for (int mode = 0; mode < 5; ++mode) {
            auto &m = modes[mode];
            int const periodSamples = m.period * rate;
            int const cycleStart = (k / periodSamples) * periodSamples;
            int const ready = k - cycleStart;
            if (m.lastStart < 0)
                m.lastStart = cycleStart;
            int elapsed = k - m.lastStart;
            if (elapsed < 0)
                elapsed += buffer;
            bool accept = false;
            int pos = cycleStart;
            int sz = ready;
            if (autosync) {
                accept = elapsed >= rate;
                pos = k - periodSamples;
                if (pos < 0)
                    pos += buffer;
                sz = periodSamples;
            } else if (m.fastReady) {
                accept = ready >= m.samplesNeeded &&
                         m.lastCycle != cycleStart;
                sz = m.samplesNeeded;
                if (accept)
                    m.lastCycle = cycleStart;
            } else {
                accept = (elapsed >= rate * 3 / 2 && ready >= m.required) ||
                         (elapsed >= rate &&
                          ready >= m.required - rate * 3 / 2) ||
                         (elapsed >= rate && ready < rate * 3 / 2);
            }
            if (!accept)
                continue;
            m.lastStart = k;
            event.modes |= 1 << mode;
            event.pos[mode] = pos;
            event.size[mode] = sz;
        }
        if (event.modes != 0)
            events.push_back(event);
    }
    return events;
}

double cpuMillis() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto micros = [](timeval const &tv) {
        return double(tv.tv_sec) * 1.0e6 + tv.tv_usec;
    };
    return (micros(usage.ru_utime) + micros(usage.ru_stime)) / 1000.0;
}

std::atomic<int> aidedEligible{0};
std::atomic<int> aidedSearches{0};
std::atomic<int> aidedAccepted{0};
std::atomic<int> rescueStarts{0};
std::atomic<int> coherentBlends{0};

void collectTelemetry(QtMsgType, QMessageLogContext const &context,
                      QString const &message) {
    if (!context.category || std::strcmp(context.category, "decoder.js8"))
        return;
    if (message.startsWith(QStringLiteral("Decoder-aided re-demod"))) {
        ++aidedEligible;
        if (message.contains(QStringLiteral("attempted 1")))
            ++aidedSearches;
        if (message.contains(QStringLiteral("crcAccepted 1")))
            ++aidedAccepted;
    } else if (message.startsWith(QStringLiteral("LDPC rescue start"))) {
        ++rescueStarts;
    } else if (message.startsWith(QStringLiteral("coherent likelihood")) &&
               message.contains(QStringLiteral("coherentEnabled true"))) {
        ++coherentBlends;
    }
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    bool const crowded = argc <= 1 || std::string_view(argv[1]) == "crowded";
    int const seconds = argc > 2 ? std::atoi(argv[2]) : 90;
    if (seconds < 30 || seconds > 300)
        return 2;
    auto const samples = makeSamples(crowded, seconds);
    bool const autosync = argc > 4 && std::string_view(argv[4]) == "autosync";
    auto const events = schedule(seconds, autosync);
    bool const diagnostics = argc > 3 && std::string_view(argv[3]) == "diag";
    QLoggingCategory::setFilterRules(
        diagnostics ? QStringLiteral("decoder.js8.debug=true\n")
                    : QStringLiteral("decoder.js8.debug=false\n"));
    if (diagnostics)
        qInstallMessageHandler(collectTelemetry);

    JS8::Decoder decoder;
    QEventLoop loop;
    std::size_t finished = 0, decoded = 0;
    std::uint64_t fingerprint = 14695981039346656037ull;
    QObject::connect(&decoder, &JS8::Decoder::decodeEvent, &loop,
                     [&](JS8::Event::Variant const &event) {
        if (auto found = std::get_if<JS8::Event::Decoded>(&event)) {
            ++decoded;
            for (unsigned char c : found->data) {
                fingerprint ^= c;
                fingerprint *= 1099511628211ull;
            }
            fingerprint ^= static_cast<unsigned>(found->mode);
            fingerprint *= 1099511628211ull;
        }
        if (std::get_if<JS8::Event::DecodeFinished>(&event)) {
            ++finished;
            loop.quit();
        }
    });
    decoder.start(QThread::LowestPriority);
    // Trigger FFT plan construction outside the timed interval.
    dec_data.params.nsubmodes = 0;
    decoder.decode();
    loop.exec();

    std::size_t nextSample = 0;
    double totalCpu = 0.0, totalWall = 0.0, maxPass = 0.0;
    double hypotheticalDone = 0.0, maxLag = 0.0;
    int lateEvents = 0;
    for (auto const &event : events) {
        auto const upTo = std::min(samples.size(),
                                   std::size_t(event.tick) * (rate / 10));
        for (; nextSample < upTo; ++nextSample)
            dec_data.d2[nextSample % JS8_RX_SAMPLE_SIZE] = samples[nextSample];
        dec_data.params.kin = static_cast<int>(upTo % JS8_RX_SAMPLE_SIZE);
        dec_data.params.nsubmodes = event.modes;
        dec_data.params.kposA = event.pos[0];
        dec_data.params.kszA = event.size[0];
        dec_data.params.kposB = event.pos[1];
        dec_data.params.kszB = event.size[1];
        dec_data.params.kposC = event.pos[2];
        dec_data.params.kszC = event.size[2];
        dec_data.params.kposE = event.pos[3];
        dec_data.params.kszE = event.size[3];
        dec_data.params.kposI = event.pos[4];
        dec_data.params.kszI = event.size[4];
        dec_data.params.nfa = 100;
        dec_data.params.nfb = 4000;
        dec_data.params.nfqso = 1004;
        dec_data.params.syncStats = false;
        dec_data.params.newdat = true;
        dec_data.params.nutc = code_time(0, 0, (event.tick / 10) % 60);
        auto const wallStart = std::chrono::steady_clock::now();
        double const cpuStart = cpuMillis();
        decoder.decode();
        loop.exec();
        double const elapsedCpu = cpuMillis() - cpuStart;
        double const elapsedWall = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - wallStart)
                                       .count();
        totalCpu += elapsedCpu;
        totalWall += elapsedWall;
        maxPass = std::max(maxPass, elapsedWall);
        double const scheduledMs = event.tick * 100.0;
        hypotheticalDone =
            std::max(hypotheticalDone, scheduledMs) + elapsedWall;
        double const lag = hypotheticalDone - scheduledMs;
        maxLag = std::max(maxLag, lag);
        if (lag > 1000.0)
            ++lateEvents;
    }
    decoder.quit();
    std::printf(
        "workload=%s nominalSeconds=%d events=%zu decoded=%zu fingerprint=%016llx "
        "cpuMs=%.1f cpuDutyPct=%.2f wallMs=%.1f maxPassMs=%.1f "
        "lagOver1s=%d maxVirtualLagMs=%.1f\n",
        autosync ? "autosync" : (crowded ? "crowded" : "noise"),
        seconds, events.size(), decoded,
        static_cast<unsigned long long>(fingerprint), totalCpu,
        totalCpu / (10.0 * seconds), totalWall, maxPass, lateEvents, maxLag);
#ifdef JS8_CPU_BENCHMARK
    std::printf("hotspots aidedCalls=%llu aidedMs=%.1f "
                "coherentCalls=%llu coherentMs=%.1f "
                "syncCalls=%llu syncMs=%.1f bpCalls=%llu bpMs=%.1f\n",
                static_cast<unsigned long long>(benchmarkRefineCalls.load()),
                benchmarkRefineNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkCoherentCalls.load()),
                benchmarkCoherentNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkSyncCalls.load()),
                benchmarkSyncNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkBpCalls.load()),
                benchmarkBpNanos.load() / 1.0e6);
    std::printf("stages candidateGenCalls=%llu candidateGenMs=%.1f "
                "basebandCalls=%llu basebandMs=%.1f "
                "decodeCalls=%llu decodeMs=%.1f "
                "downsampleCalls=%llu downsampleMs=%.1f "
                "demodCalls=%llu demodMs=%.1f "
                "toneBinsCalls=%llu toneBinsMs=%.1f "
                "referenceCalls=%llu referenceMs=%.1f "
                "sicCalls=%llu sicMs=%.1f\n",
                static_cast<unsigned long long>(benchmarkCandidateGenCalls.load()),
                benchmarkCandidateGenNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkBasebandCalls.load()),
                benchmarkBasebandNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkDecodeCalls.load()),
                benchmarkDecodeNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkDownsampleCalls.load()),
                benchmarkDownsampleNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkDemodCalls.load()),
                benchmarkDemodNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkToneBinsCalls.load()),
                benchmarkToneBinsNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkReferenceCalls.load()),
                benchmarkReferenceNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkSicCalls.load()),
                benchmarkSicNanos.load() / 1.0e6);
    std::printf("candidateParts spectrumCalls=%llu spectrumMs=%.1f "
                "baselineCalls=%llu baselineMs=%.1f "
                "rankScoreMs=%.1f rankIndexMs=%.1f\n",
                static_cast<unsigned long long>(benchmarkSpectrumCalls.load()),
                benchmarkSpectrumNanos.load() / 1.0e6,
                static_cast<unsigned long long>(benchmarkBaselineCalls.load()),
                benchmarkBaselineNanos.load() / 1.0e6,
                benchmarkRankScoreNanos.load() / 1.0e6,
                benchmarkRankIndexNanos.load() / 1.0e6);
    std::printf("bpParts sampledCalls=%llu variableMs=%.1f "
                "syndromeMs=%.1f bitToCheckMs=%.1f tanhMs=%.1f "
                "checkToBitMs=%.1f\n",
                static_cast<unsigned long long>(benchmarkBpProfiledCalls.load()),
                benchmarkBpVariableNanos.load() / 1.0e6,
                benchmarkBpSyndromeNanos.load() / 1.0e6,
                benchmarkBpBitToCheckNanos.load() / 1.0e6,
                benchmarkBpTanhNanos.load() / 1.0e6,
                benchmarkBpCheckToBitNanos.load() / 1.0e6);
#endif
#ifdef JS8_BENCHMARK_VERIFY_COARSE_SYNC
    std::printf("coarseSyncMismatches=%llu\n",
                static_cast<unsigned long long>(benchmarkSyncMismatches.load()));
#endif
#ifdef JS8_BENCHMARK_VERIFY_RANK_SCAN
    std::printf("candidateScoresCompared=%llu candidateScoreMismatches=%llu\n",
                static_cast<unsigned long long>(
                    benchmarkRankComparedScores.load()),
                static_cast<unsigned long long>(benchmarkRankMismatches.load()));
#endif
    if (diagnostics)
        std::printf("telemetry aidedEligible=%d searched=%d crcAccepted=%d "
                    "rescueStarts=%d coherentBlends=%d\n",
                    aidedEligible.load(), aidedSearches.load(),
                    aidedAccepted.load(), rescueStarts.load(),
                    coherentBlends.load());
    return 0;
}
