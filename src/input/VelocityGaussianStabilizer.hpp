#pragma once

#include "input/PointerSample.hpp"

#include <array>
#include <cstddef>

namespace sawer {

// Velocity-aware moving average. Slow, jitter-prone motion uses more history;
// fast intentional motion naturally gives recent points more influence.
class VelocityGaussianStabilizer final {
public:
    void reset(
        double sigma,
        PointerSample initial_sample,
        double coordinate_scale = 1.0) noexcept;
    [[nodiscard]] PointerSample push(PointerSample sample) noexcept;
    [[nodiscard]] std::size_t sample_count() const noexcept;

private:
    static constexpr std::size_t capacity = 64U;

    std::array<PointerSample, capacity> samples_{};
    std::array<double, capacity> velocities_{};
    std::size_t count_{1U};
    std::uint64_t last_timestamp_{};
    double two_sigma_squared_{0.5};
    double coordinate_scale_{1.0};
};

} // namespace sawer
