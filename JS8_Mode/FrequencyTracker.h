/**
 * @file FrequencyTracker.h
 * @brief Trackers for residual frequency and timing offsets used by the decoder.
 *
 * `FrequencyTracker` and `TimingTracker` provide lightweight per-candidate
 * estimators used during decoding to refine frequency and sample timing
 * offsets.
 */

#pragma once

#include <complex>
#include <numbers>

namespace js8 {
/**
 * @brief Lightweight PLL/Kalman-style tracker for residual frequency offset.
 *
 * Initialized with coarse estimates and sample rate; apply() rotates samples
 * by the tracked offset, update() nudges the estimate using pilot residuals.
 * Used inside the JS8 decode loop per candidate frame.
 */
class FrequencyTracker {
  public:
    /**
     * @brief Initialize or reconfigure the tracker.
     * @param initial_hz Initial frequency estimate in Hz.
     * @param sample_rate_hz Sample rate in Hz for the input data.
     * @param alpha Smoothing coefficient for the estimator (default 0.15).
     * @param max_step_hz Maximum allowed step change per update (Hz).
     * @param max_error_hz Maximum tolerated error before aggressive limiting.
     */
    void reset(double initial_hz, double sample_rate_hz, double alpha = 0.15,
               double max_step_hz = 0.3, double max_error_hz = 5.0);

    /**
     * @brief Disable the tracker (subsequent apply() calls become no-ops).
     */
    void disable();

    /**
     * @brief Whether the tracker is enabled.
     * @return true when enabled, false when disabled.
     */
    [[nodiscard]] bool enabled() const noexcept;

    /**
     * @brief Current estimated residual frequency.
     * @return Frequency estimate in Hz.
     */
    [[nodiscard]] double currentHz() const noexcept;

    /**
     * @brief Average step size applied by the tracker (Hz).
     * @return Average per-update frequency step in Hz.
     */
    [[nodiscard]] double averageStepHz() const noexcept;

    /**
     * @brief Rotate an array of complex samples by the tracked frequency.
     * @param data Pointer to complex input/output samples to be rotated in-place.
     * @param count Number of complex samples.
     */
    void apply(std::complex<float> *data, int count) const;

    /**
     * @brief Update the tracker estimate using a measured residual.
     * @param residual_hz Measured residual frequency (Hz) to incorporate.
     * @param weight Weighting applied to this update (default 1.0).
     */
    void update(double residual_hz, double weight = 1.0);

  private:
    bool m_enabled = true;          ///< Whether tracker updates are active
    double m_est_hz = 0.0;          ///< Current frequency estimate (Hz)
    double m_fs = 0.0;              ///< Sample rate (Hz)
    double m_alpha = 0.15;          ///< Smoothing coefficient
    double m_max_step_hz = 0.3;     ///< Max allowed step per update (Hz)
    double m_max_error_hz = 5.0;    ///< Max tolerated error before limiting
    double m_sum_abs = 0.0;         ///< Accumulated absolute step magnitudes
    int m_updates = 0;              ///< Count of updates applied
};

class TimingTracker {
  public:
    /**
     * @brief Tracks residual timing (sample) offset between the symbol clock
     * and the signal.
     *
     * Initialized with bounds and step limits; update() ingests early/late
     * energy errors from pilots to refine sample alignment. Used per candidate
     * in the JS8 decode loop alongside FrequencyTracker.
     */

    /**
     * @brief Initialize the timing tracker.
     * @param initial_samples Initial timing offset in samples.
     * @param alpha Smoothing coefficient (default 0.15).
     * @param max_step Maximum allowed step per update (samples).
     * @param max_total_error Maximum cumulative error tolerated (samples).
     */
    void reset(double initial_samples, double alpha = 0.15,
           double max_step = 0.35, double max_total_error = 2.0);

    /** Disable timing tracker updates. */
    void disable();

    /**
     * @brief Whether the timing tracker is enabled.
     * @return true if enabled.
     */
    [[nodiscard]] bool enabled() const noexcept;

    /**
     * @brief Current estimated residual timing offset in samples.
     * @return Estimated sample offset.
     */
    [[nodiscard]] double currentSamples() const noexcept;

    /**
     * @brief Average timing step applied per update (samples).
     * @return Average step in sample units.
     */
    [[nodiscard]] double averageStepSamples() const noexcept;

    /**
     * @brief Update timing estimate using a measured residual (samples).
     * @param residual_samples Measured residual in samples.
     * @param weight Weighting for this update (default 1.0).
     */
    void update(double residual_samples, double weight = 1.0);

  private:
    bool m_enabled = true;      ///< Whether timing updates are active
    double m_est_samples = 0.0; ///< Current estimated sample offset
    double m_alpha = 0.15;      ///< Smoothing coefficient
    double m_max_step = 0.35;   ///< Max step allowed per update (samples)
    double m_max_total = 2.0;   ///< Max total error tolerated (samples)
    double m_sum_abs = 0.0;     ///< Accumulated absolute steps
    int m_updates = 0;          ///< Number of updates applied
};
} // namespace js8

// JS8.cpp includes JS8.h before this header. Limit the SIC wrapper macros to
// that decoder translation unit so tracker-only users do not inherit them.
#ifdef __JS8
#include "sic_refinement.h"
#endif
