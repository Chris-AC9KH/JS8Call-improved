// Deterministic admission tests; no Qt or hardware-specific timing constants.
// clang++ -std=c++20 -O2 -I. tools/optional_work_test.cpp -o /tmp/optional_work_test
#include "JS8_Mode/optional_work.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>

int main() {
    using js8::OptionalStage;
    using js8::OptionalWorkBudget;
    using js8::OptionalWorkSignal;
    using Clock = OptionalWorkBudget::Clock;
    using namespace std::chrono_literals;

    OptionalWorkSignal signal;
    std::array<OptionalWorkBudget::Duration, OptionalWorkBudget::stages>
        estimates{};
    OptionalWorkBudget budget{&signal, 5ms, estimates};
    assert(budget.allow(OptionalStage::Aided)); // standalone, no UI forecast
    signal.pending(true);
    assert(!budget.allow(OptionalStage::Aided) && budget.stop());
    signal.pending(false);
    signal.nextReady(Clock::now() + 10s);
    assert(budget.allow(OptionalStage::Aided) && !budget.stop());
    budget.observe(OptionalStage::Aided, Clock::now() - 100ms);
    assert(estimates[static_cast<std::size_t>(OptionalStage::Aided)] >= 125ms);
    signal.nextReady(Clock::now() + 10ms);
    assert(!budget.allow(OptionalStage::Aided) && !budget.stop());
    signal.nextReady(Clock::now() - 1ms);
    assert(budget.stop());
    signal.nextReady(Clock::now() + 10s);
    OptionalWorkBudget unknown{&signal, 5ms, estimates, true};
    assert(!unknown.allow(OptionalStage::Deep));
    OptionalWorkBudget unconfigured{nullptr, 0ms, estimates, true};
    assert(unconfigured.allow(OptionalStage::Deep) && !unconfigured.stop());
    std::puts("optional-work admission tests passed");
}
