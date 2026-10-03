#pragma once

#include <cstddef>
#include <algorithm>

namespace sawer {

struct AdaptiveStrokeSettings final {
    // 0 is the most direct response; 1 applies the most correction.
    double strength{0.32};
    // Maximum permitted distance between the pointer and filtered point in
    // screen pixels. This is a hard latency bound, independent of zoom.
    double max_trailing_pixels{8.0};
};

enum class StrokeStabilization {
    off,
    light,
    standard,
    strong,
};

struct DrawingSettings final {
    // Mouse drawing always uses the Responsive filter.
    [[nodiscard]] constexpr AdaptiveStrokeSettings
    adaptive_stroke_settings() const noexcept
    {
        return {.strength = 0.32, .max_trailing_pixels = 8.0};
    }

    // Compatibility settings for the previous drawing pipeline. They remain
    // available for the legacy processing helpers.
#if defined(_WIN32)
    StrokeStabilization stabilization{StrokeStabilization::light};
#else
    StrokeStabilization stabilization{StrokeStabilization::standard};
#endif

    [[nodiscard]] static constexpr double stabilization_scale() noexcept
    {
#if defined(_WIN32)
        return 0.8;
#else
        return 1.0;
#endif
    }

    [[nodiscard]] constexpr bool stabilization_enabled() const noexcept
    {
        return stabilization != StrokeStabilization::off;
    }

    [[nodiscard]] constexpr double gaussian_sigma() const noexcept
    {
        switch (stabilization) {
        case StrokeStabilization::off: return 0.0;
        case StrokeStabilization::light:
            return 12.0 * stabilization_scale();
        case StrokeStabilization::standard:
            return 24.0 * stabilization_scale();
        case StrokeStabilization::strong:
            return 36.0 * stabilization_scale();
        }
        return 0.0;
    }

    [[nodiscard]] constexpr double sampling_distance() const noexcept
    {
        return stabilization_enabled() ? 0.01 : 0.75;
    }

    [[nodiscard]] static constexpr double simplification_tolerance() noexcept
    {
        return 0.15;
    }

    // Pointer input is normalized to one sample per screen pixel before this
    // completion filter, so the radius has the same visual meaning on every
    // window system and mouse polling rate.
    [[nodiscard]] constexpr std::size_t completion_smoothing_radius() const noexcept
    {
        std::size_t radius = 0U;
        switch (stabilization) {
        case StrokeStabilization::off: break;
        case StrokeStabilization::light: radius = 6U; break;
        case StrokeStabilization::standard: radius = 14U; break;
        case StrokeStabilization::strong: radius = 22U; break;
        }
#if defined(_WIN32)
        // Radii are integral, so round the 0.8 scale to the nearest sample.
        return (radius * 4U + 2U) / 5U;
#else
        return radius;
#endif
    }
};

} // namespace sawer
