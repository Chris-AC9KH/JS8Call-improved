/** @file optional_work.h
 *  @brief Hardware-independent pressure signal for optional JS8 decode work.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace js8 {

class OptionalWorkSignal {
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    void pending(bool value) noexcept {
        m_pending.store(value, std::memory_order_release);
    }

    void nextReady(Clock::time_point when) noexcept {
        m_nextReadyNs.store(
            std::chrono::duration_cast<Duration>(when.time_since_epoch())
                .count(),
            std::memory_order_release);
    }

    bool hasDeadline() const noexcept {
        return m_nextReadyNs.load(std::memory_order_acquire) != 0;
    }

    // No forecast is available in standalone decoders; their normal behavior
    // remains unchanged. The application publishes a deadline every RX tick.
    bool pressured(Duration reserve = Duration{0}) const noexcept {
        if (m_pending.load(std::memory_order_acquire))
            return true;
        auto const deadline = m_nextReadyNs.load(std::memory_order_acquire);
        if (deadline == 0)
            return false;
        auto const now = std::chrono::duration_cast<Duration>(
                             Clock::now().time_since_epoch())
                             .count();
        return now >= deadline || reserve.count() >= deadline - now;
    }

  private:
    std::atomic<bool> m_pending{false};
    std::atomic<std::int64_t> m_nextReadyNs{0};
};

enum class OptionalStage : std::size_t { Deep, Rescue, Aided, Count };

// Worker-owned stage estimates are updated from this device's actual elapsed
// time; they are neither model-dependent nor shared between decoder threads.
class OptionalWorkBudget {
  public:
    using Clock = OptionalWorkSignal::Clock;
    using Duration = OptionalWorkSignal::Duration;
    static constexpr std::size_t stages =
        static_cast<std::size_t>(OptionalStage::Count);

    OptionalWorkBudget(OptionalWorkSignal const *signal, Duration reserve,
                       std::array<Duration, stages> &estimates,
                       bool unknownMandatory = false)
        : m_signal(signal), m_reserve(reserve), m_estimates(estimates),
          m_unknownMandatory(unknownMandatory) {}

    bool allow(OptionalStage stage) const {
        if (m_signal == nullptr)
            return true;
        if (m_unknownMandatory && m_signal->pressured())
            return false;
        if (m_unknownMandatory && m_signal->hasDeadline())
            return false;
        return !m_signal->pressured(m_reserve +
                                    m_estimates[static_cast<std::size_t>(stage)]);
    }

    bool stop() const {
        return m_signal && m_signal->pressured(m_reserve);
    }

    void observe(OptionalStage stage, Clock::time_point started) {
        auto const elapsed = std::chrono::duration_cast<Duration>(
            Clock::now() - started);
        auto &estimate = m_estimates[static_cast<std::size_t>(stage)];
        // A decaying high-water estimate reacts quickly to slower hardware
        // and slowly recovers when the receiver has spare capacity.
        estimate = std::max(estimate * 9 / 10, elapsed + elapsed / 4);
    }

  private:
    OptionalWorkSignal const *m_signal;
    Duration m_reserve;
    std::array<Duration, stages> &m_estimates;
    bool m_unknownMandatory;
};
} // namespace js8
