#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sawer {

// Fixed board spacing, enlarged by 15% from the original 50-unit pattern.
inline constexpr double grid_world_spacing = 57.5;

// Renderer-local background families. Keeping the planning calculation free
// of SDL and UI types makes the worst-case geometry budget cheap to test.
enum class BackgroundGridPattern : std::uint8_t {
    solid,
    dot,
    square,
    graph,
    hybrid,
    diamond,
    wide_rule,
    triangle,
    narrow_rule,
};

struct BackgroundGridPlan final {
    double world_spacing{grid_world_spacing};
    double density{1.0};
    std::size_t estimated_vertices{};
};

namespace detail {

inline std::size_t saturated_product(
    const std::size_t first,
    const std::size_t second) noexcept
{
    if (first != 0U
        && second > std::numeric_limits<std::size_t>::max() / first) {
        return std::numeric_limits<std::size_t>::max();
    }
    return first * second;
}

inline std::size_t grid_line_count(
    const double extent_pixels,
    const double spacing_pixels) noexcept
{
    if (!(extent_pixels > 0.0) || !(spacing_pixels > 0.0)
        || !std::isfinite(extent_pixels)
        || !std::isfinite(spacing_pixels)) {
        return 0U;
    }
    const double count = std::ceil(extent_pixels / spacing_pixels) + 2.0;
    if (count >= static_cast<double>(
                     std::numeric_limits<std::size_t>::max())) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(count);
}

} // namespace detail

inline std::size_t estimate_background_grid_vertices(
    const BackgroundGridPattern pattern,
    const double viewport_width,
    const double viewport_height,
    const double screen_spacing) noexcept
{
    using detail::grid_line_count;
    using detail::saturated_product;
    if (pattern == BackgroundGridPattern::solid
        || !(screen_spacing > 0.0)) {
        return 0U;
    }

    const auto line_vertices = [](const std::size_t lines) noexcept {
        return saturated_product(lines, 6U);
    };
    const auto lattice_vertices = [&](const double spacing) noexcept {
        const std::size_t columns = grid_line_count(viewport_width, spacing);
        const std::size_t rows = grid_line_count(viewport_height, spacing);
        const double radius = spacing * 0.045;
        const std::size_t vertices_per_dot = radius < 2.0
            ? 6U
            : (radius < 6.0 ? 30U : 54U);
        return saturated_product(
            saturated_product(columns, rows), vertices_per_dot);
    };
    const auto orthogonal_vertices = [&](const double spacing) noexcept {
        const std::size_t vertical = grid_line_count(viewport_width, spacing);
        const std::size_t horizontal = grid_line_count(viewport_height, spacing);
        return line_vertices(vertical + horizontal);
    };
    const auto diagonal_vertices = [&](
        const double slope,
        const double spacing) noexcept {
        const std::size_t lines = grid_line_count(
            viewport_height + std::abs(slope) * viewport_width,
            spacing);
        return line_vertices(lines);
    };

    switch (pattern) {
    case BackgroundGridPattern::dot:
        return lattice_vertices(screen_spacing);
    case BackgroundGridPattern::square:
        return orthogonal_vertices(screen_spacing);
    case BackgroundGridPattern::graph:
        return orthogonal_vertices(screen_spacing / 5.0);
    case BackgroundGridPattern::hybrid:
        return orthogonal_vertices(screen_spacing)
            + lattice_vertices(screen_spacing);
    case BackgroundGridPattern::diamond:
        return diagonal_vertices(1.0, screen_spacing) * 2U;
    case BackgroundGridPattern::wide_rule:
        return line_vertices(grid_line_count(viewport_height, screen_spacing));
    case BackgroundGridPattern::narrow_rule:
        return line_vertices(
            grid_line_count(viewport_height, screen_spacing * 0.5));
    case BackgroundGridPattern::triangle:
        return line_vertices(grid_line_count(viewport_height, screen_spacing))
            + diagonal_vertices(1.7320508075688772, screen_spacing * 2.0)
                * 2U;
    case BackgroundGridPattern::solid:
        return 0U;
    }
    return 0U;
}

inline BackgroundGridPlan plan_background_grid(
    const BackgroundGridPattern pattern,
    const double viewport_width,
    const double viewport_height,
    const double zoom,
    const double base_world_spacing = grid_world_spacing,
    const std::size_t vertex_budget = 250'000U) noexcept
{
    BackgroundGridPlan plan{base_world_spacing, 1.0, 0U};
    if (pattern == BackgroundGridPattern::solid
        || !(zoom > 0.0) || !(base_world_spacing > 0.0)
        || !(viewport_width > 0.0) || !(viewport_height > 0.0)
        || !std::isfinite(zoom) || !std::isfinite(base_world_spacing)
        || !std::isfinite(viewport_width) || !std::isfinite(viewport_height)) {
        return plan;
    }

    // The lattice is fixed in board coordinates and scales with the camera.
    // When too many marks are visible, retain every fifth mark on that same
    // lattice rather than moving it or enlarging the marks themselves.
    double screen_spacing = base_world_spacing * zoom;
    while (estimate_background_grid_vertices(
               pattern, viewport_width, viewport_height, screen_spacing)
            > vertex_budget
           && screen_spacing <= std::numeric_limits<double>::max() / 5.0) {
        screen_spacing *= 5.0;
        plan.world_spacing *= 5.0;
    }

    plan.estimated_vertices = estimate_background_grid_vertices(
        pattern, viewport_width, viewport_height, screen_spacing);
    return plan;
}

} // namespace sawer
