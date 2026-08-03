#pragma once

#include "input/PointerSample.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace sawer {

// Converts platform-dependent pointer event cadence into a bounded stream
// with uniform screen-space spacing. Windows commonly delivers many tiny
// motions while Wayland batches them into fewer, longer segments.
class PointerResampler final {
public:
    static constexpr std::size_t capacity = 128U;
    static constexpr double spacing_pixels = 1.0;

    void reset(PointerSample initial_sample) noexcept;

    [[nodiscard]] std::span<const PointerSample> push(
        PointerSample sample,
        double coordinate_scale,
        bool include_endpoint = false) noexcept;

private:
    PointerSample previous_{};
    double distance_to_next_pixels_{spacing_pixels};
    bool initialized_{};
    std::array<PointerSample, capacity> output_{};
    std::size_t output_count_{};
};

} // namespace sawer
