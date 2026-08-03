#pragma once

#include <algorithm>
#include <cstddef>

namespace sawer {

inline constexpr double board_half_extent = 1'000'000.0;
// Imported and in-memory objects obey the same ceiling as processed gestures
// so callers cannot bypass the input path's memory limit.
inline constexpr std::size_t maximum_stroke_point_count = 2'000'000U;

struct Vec2d final {
    double x{};
    double y{};

    friend constexpr bool operator==(const Vec2d&, const Vec2d&) = default;
};

struct Aabb final {
    double min_x{};
    double min_y{};
    double max_x{};
    double max_y{};

    [[nodiscard]] constexpr bool intersects(const Aabb& other) const noexcept
    {
        return min_x <= other.max_x && max_x >= other.min_x
            && min_y <= other.max_y && max_y >= other.min_y;
    }

    [[nodiscard]] static constexpr Aabb from_points(
        const Vec2d first,
        const Vec2d second,
        const double padding = 0.0) noexcept
    {
        return {
            std::min(first.x, second.x) - padding,
            std::min(first.y, second.y) - padding,
            std::max(first.x, second.x) + padding,
            std::max(first.y, second.y) + padding,
        };
    }
};

} // namespace sawer
