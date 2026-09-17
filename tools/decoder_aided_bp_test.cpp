// A/B BP semantics test for decoder-aided re-demodulation (Fix 9.D).
//
// Verifies that the normal `cw` output of bpdecode174() keeps last-iteration
// semantics on failure (the pre-aided-commit contract) while BPResult.bestCw
// exposes the minimum-syndrome word for the aided feature, identically
// whether re-demodulation is enabled or not.
//
// This TU includes the real decoder implementation (same pattern as
// tools/whitening_diag.cpp) so it exercises the actual bpdecode174,
// including the anonymous-namespace parity tables.
//
// Build (mirrors the whitening_diag link):
//   clang++ -std=c++20 -O2 -Wall -I. -Ibuild/JS8Call_autogen/include \
//     $(pkg-config --cflags Qt6Core) tools/decoder_aided_bp_test.cpp \
//     JS8_Mode/FrequencyTracker.cpp \
//     build/JS8Call_autogen/33ZB6LRKMI/moc_JS8.cpp \
//     $(pkg-config --libs Qt6Core) -L/opt/homebrew/lib -lfftw3f -lpthread \
//     -o /tmp/bp_test && /tmp/bp_test

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <QLoggingCategory>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

// Provide the globals expected by JS8.cpp
struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

// Include implementation (in the diagnostic binary only).
#include "../JS8_Mode/JS8.cpp"

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

// Independent syndrome count using the decoder's parity tables.
int syndromeCount(std::array<int8_t, N> const &cw) {
    int ncheck = 0;
    for (int i = 0; i < M; ++i) {
        int s = 0;
        for (int j = 0; j < Nm[i].valid_neighbors; ++j)
            s += cw[Nm[i].neighbors[j]];
        if (s % 2 != 0)
            ++ncheck;
    }
    return ncheck;
}

void runConvergedWord() {
    std::printf("[converged word]\n");
    // All-zero word is always a valid codeword: strong bit-0 LLRs converge
    // at iteration 0 with everything consistent.
    std::array<float, N> llr{};
    llr.fill(-6.0f);
    std::array<int8_t, K> decoded{};
    std::array<int8_t, N> cw{};
    BPResult const bp = bpdecode174(llr, decoded, cw, BPOptions{});
    check(bp.bestChecks == 0, "converged syndrome is zero");
    check(bp.bestCwValid, "bestCw valid on success");
    check(syndromeCount(bp.bestCw) == 0, "bestCw is the codeword");
    check(std::memcmp(bp.bestCw.data(), cw.data(), N) == 0,
          "bestCw matches cw on success");
    check(bp.hardErrors == 0, "no hard errors on clean word");
}

void runFailedDecode() {
    std::printf("[failed decode keeps last-iterate cw]\n");
    // Fixed-seed adversarial LLRs at the decoding cliff.
    std::uint32_t state = 0xC10Cu;
    auto const randf = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    };
    std::array<float, N> llr{};
    for (auto &v : llr)
        v = 2.5f * randf();
    std::array<int8_t, K> decoded{};
    std::array<int8_t, N> cw{};
    BPResult const bp = bpdecode174(llr, decoded, cw, BPOptions{});
    std::printf("    info: bestChecks=%d finalChecks=%d earlyAborted=%d\n",
                bp.bestChecks, bp.finalChecks,
                bp.earlyAborted ? 1 : 0);
    check(bp.bestCwValid, "bestCw valid on failure");
    check(syndromeCount(bp.bestCw) == bp.bestChecks,
          "bestCw syndrome equals bestChecks (min word exposed)");
    check(syndromeCount(cw) == bp.finalChecks,
          "cw syndrome equals finalChecks (last-iterate semantics)");
    check(bp.bestChecks <= bp.finalChecks,
          "bestChecks is the minimum over iterates");
}

void runDeterminism() {
    std::printf("[determinism]\n");
    std::uint32_t state = 0xBEEFu;
    std::array<float, N> llr{};
    for (auto &v : llr) {
        state = state * 1664525u + 1013904223u;
        v = 3.0f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
    }
    std::array<int8_t, K> d1{};
    std::array<int8_t, N> c1{};
    BPResult const b1 = bpdecode174(llr, d1, c1, BPOptions{});
    std::array<int8_t, K> d2{};
    std::array<int8_t, N> c2{};
    BPResult const b2 = bpdecode174(llr, d2, c2, BPOptions{});
    check(b1.bestChecks == b2.bestChecks, "repeated runs agree on bestChecks");
    check(std::memcmp(b1.bestCw.data(), b2.bestCw.data(), N) == 0,
          "repeated runs agree on bestCw (first-minimum-wins)");
    check(std::memcmp(c1.data(), c2.data(), N) == 0,
          "repeated runs agree on cw");
}

void runZeroIterationBound() {
    std::printf("[zero iteration bound]\n");
    // Single-1 word cannot be a codeword (its checks must fail); with zero
    // iterations only the initial slice is evaluated.
    std::array<float, N> llr{};
    llr.fill(-4.0f);
    llr[17] = 4.0f;
    std::array<int8_t, K> decoded{};
    std::array<int8_t, N> cw{};
    BPOptions options;
    options.maxIterations = 0;
    BPResult const bp = bpdecode174(llr, decoded, cw, options);
    check(bp.bestChecks > 0, "single-1 word fails parity");
    check(bp.bestCwValid, "bestCw valid at iteration bound");
    check(syndromeCount(bp.bestCw) == bp.bestChecks,
          "bestCw exposes the evaluated word");
}

} // namespace

int main() {
    runConvergedWord();
    runFailedDecode();
    runDeterminism();
    runZeroIterationBound();

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED"
                                                      : "TESTS FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
