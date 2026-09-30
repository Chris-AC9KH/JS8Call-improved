/**
 * @file publishDecodePressure.cpp
 * @brief Publishes receive scheduling pressure to the decoder worker.
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/WideGraph.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

void UI_Constructor::publishDecodePressure(qint32 k) {
    int nextReadySamples = JS8_RX_SAMPLE_RATE * JS8_NTMAX;
    for (int const mode : {Varicode::JS8CallNormal, Varicode::JS8CallFast,
                           Varicode::JS8CallTurbo, Varicode::JS8CallSlow,
                           Varicode::JS8CallUltra}) {
        bool const autosync = m_wideGraph->shouldAutoSyncSubmode(mode);
        int const period = JS8::Submode::samplesPerPeriod(mode);
        int const earliest =
            (mode == Varicode::JS8CallTurbo || mode == Varicode::JS8CallUltra)
                ? JS8::Submode::samplesNeeded(mode)
                : std::max(0, static_cast<int>(
                                  JS8::Submode::samplesForSymbols(mode)) -
                                  JS8_RX_SAMPLE_RATE * 3 / 2);
        int until = (earliest - (k % period) + period) % period;
        if (until == 0)
            until = period;
        if (autosync)
            until = std::min(until, JS8_RX_SAMPLE_RATE);
        if (m_lastDecodeStartMap.contains(mode)) {
            int const since =
                (k - m_lastDecodeStartMap.value(mode) + JS8_RX_SAMPLE_SIZE) %
                JS8_RX_SAMPLE_SIZE;
            if (since < JS8_RX_SAMPLE_RATE)
                until = std::min(until, JS8_RX_SAMPLE_RATE - since);
        }
        nextReadySamples = std::min(nextReadySamples, until);
    }
    auto const nextReady = std::chrono::steady_clock::now() +
        std::chrono::nanoseconds{static_cast<std::int64_t>(nextReadySamples) *
                                 1000000000LL / JS8_RX_SAMPLE_RATE};
    m_decoder.nextDecodeReady(nextReady);
    m_decoder.pendingDecode(m_decoderBusy && !m_decoderQueue.isEmpty());
}
