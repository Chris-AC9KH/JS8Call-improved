/**
 * @file bp_math.h
 * @brief Bounded confidence and finite check-node messages for LDPC decoding.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

/** @brief Numerical limits for JS8 belief propagation. */
namespace js8::bp {
/**
 * @brief Limits channel confidence so a contaminated strong symbol is correctable.
 * @return A finite LLR in [-8, 8], or zero for missing (NaN) evidence.
 */
inline float channelLlr(float value) {
    return std::isnan(value) ? 0.0f : std::clamp(value, -8.0f, 8.0f);
}

/**
 * @brief Converts a check-node product to a finite extrinsic LLR.
 * @return The bounded inverse-hyperbolic-tangent message.
 */
inline float checkMessage(float product) {
    constexpr float limit = 1.0f - std::numeric_limits<float>::epsilon();
    return 2.0f * std::atanh(-std::clamp(product, -limit, limit));
}
} // namespace js8::bp
