#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Operations shared by the receive-period rollover and its regression tests.
namespace RxPeriodContinuity {

struct FlushResult {
    std::size_t inputSamples = 0;
    std::size_t outputSamples = 0;
};

// Process all complete decimation groups from a partial FFT input block.
// The caller owns the decoder's time-aligned write index and FIR state.
template <typename Downsample>
FlushResult flushPartial(const std::int16_t *input,
                         std::size_t pendingSamples,
                         std::size_t decimation,
                         std::int16_t *output,
                         std::size_t outputCapacity,
                         Downsample downsample)
{
    if (!decimation)
        return {};

    const auto count = std::min(pendingSamples / decimation, outputCapacity);
    for (std::size_t i = 0; i < count; ++i)
        output[i] = downsample(input + i * decimation);

    return {count * decimation, count};
}

// Assemble a contiguous FFT window spanning a minute boundary. The previous
// minute's tail is aligned so tail[window-1] immediately precedes current[0].
// Unavailable samples at initial startup are silence, never uninitialized RAM.
inline void fillFftInput(float *output,
                         std::size_t window,
                         const std::int16_t *current,
                         std::size_t currentAvailable,
                         std::size_t endOffset,
                         const std::int16_t *previousTail,
                         bool previousValid)
{
    for (std::size_t i = 0; i < window; ++i) {
        const auto j = static_cast<std::ptrdiff_t>(endOffset) +
                       static_cast<std::ptrdiff_t>(i) -
                       static_cast<std::ptrdiff_t>(window);
        std::int16_t sample = 0;
        if (j >= 0 && static_cast<std::size_t>(j) < currentAvailable) {
            sample = current[j];
        } else if (j < 0 && previousValid && previousTail &&
                   j >= -static_cast<std::ptrdiff_t>(window)) {
            sample = previousTail[static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(window) + j)];
        }
        output[i] = 0.1f * sample;
    }
}

} // namespace RxPeriodContinuity
