#pragma once

#include "geometry/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace sawer {

// Round upward so a cached mesh never exceeds the quarter-pixel error at any
// zoom in its bucket. Only renderer scratch points change, never the document.
inline int stroke_detail_bucket(const double zoom) noexcept
{
    return static_cast<int>(std::ceil(std::log2(std::max(zoom, 1.0e-9)) * 4.0));
}

inline double stroke_detail_tolerance(const double zoom) noexcept
{
    return 0.25 / std::exp2(static_cast<double>(stroke_detail_bucket(zoom)) / 4.0);
}

inline void simplify_stroke_for_rendering(
    const std::span<const Vec2d> source,
    const double tolerance,
    std::vector<Vec2d>& result)
{
    result.clear();
    if (source.empty()) return;
    result.push_back(source.front());
    const double squared = tolerance * tolerance;
    for (std::size_t index = 1U; index + 1U < source.size(); ++index) {
        const double dx = source[index].x - result.back().x;
        const double dy = source[index].y - result.back().y;
        // Every omitted point is within tolerance of a retained endpoint,
        // hence also within tolerance of the replacement segment.
        if (dx * dx + dy * dy >= squared) result.push_back(source[index]);
    }
    if (source.size() > 1U) result.push_back(source.back());
}

} // namespace sawer
