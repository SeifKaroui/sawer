#pragma once
#include "renderer/BackgroundGrid.hpp"
#include "document/Object.hpp"
#include <array>
namespace sawer {
struct BackgroundParameters final {
    std::array<float, 4> board, minor, major, axis, dots, view, scale, phase, axes, marks, board_rect;
};
static_assert(sizeof(BackgroundParameters) == 11U * 16U);
inline BackgroundParameters background_parameters(const BackgroundGridPattern pattern,
    const Color background, const std::optional<Color> ink_color, const Vec2d camera,
    const Vec2d viewport, const double zoom, const Vec2d drawable)
{
    BackgroundParameters p{};
    const auto color = [](const Color c) {
        return std::array<float, 4>{c.red / 255.0F, c.green / 255.0F, c.blue / 255.0F, 1.0F};
    };
    p.board = color(background);
    const float luminance = p.board[0] * 0.299F + p.board[1] * 0.587F + p.board[2] * 0.114F;
    const auto ink = ink_color ? color(*ink_color) : color(luminance > 0.5F
        ? Color{0, 0, 0, 255} : Color{255, 255, 255, 255});
    const auto mix = [&](const float strength) {
        auto result = p.board;
        for (std::size_t component = 0; component < 3U; ++component)
            result[component] += (ink[component] - result[component]) * strength;
        return result;
    };
    p.minor = mix(ink_color ? 0.65F : 0.15F);
    p.major = mix(ink_color ? 0.84F : 0.32F);
    p.axis = mix(ink_color ? 1.0F : 0.48F);
    p.dots = mix(ink_color ? 0.78F : 0.26F);
    const auto plan = plan_background_grid(pattern, viewport.x, viewport.y, zoom);
    const double spacing = plan.world_spacing * (pattern == BackgroundGridPattern::graph ? 0.2
        : (pattern == BackgroundGridPattern::narrow_rule ? 0.5 : 1.0));
    const double diagonal = plan.world_spacing * (pattern == BackgroundGridPattern::triangle ? 2.0 : 1.0);
    const double slope = pattern == BackgroundGridPattern::triangle ? 1.7320508 : 1.0;
    p.view = {static_cast<float>(viewport.x), static_cast<float>(viewport.y), static_cast<float>(pattern), 0.0F};
    p.scale = {static_cast<float>(drawable.x / viewport.x), static_cast<float>(drawable.y / viewport.y),
        static_cast<float>(spacing * zoom), static_cast<float>(grid_world_spacing * zoom)};
    p.phase = {static_cast<float>(std::remainder(camera.x, spacing * 5.0) * zoom),
        static_cast<float>(std::remainder(camera.y, spacing * 5.0) * zoom),
        static_cast<float>(std::remainder(camera.y - slope * camera.x, diagonal) * zoom),
        static_cast<float>(std::remainder(camera.y + slope * camera.x, diagonal) * zoom)};
    const double base = grid_world_spacing * zoom;
    p.axes = {static_cast<float>(camera.x * zoom), static_cast<float>(camera.y * zoom),
        static_cast<float>(base * 0.018), static_cast<float>(base * 0.032)};
    p.marks = {static_cast<float>(base * 0.05), static_cast<float>(base * 0.045),
        static_cast<float>(diagonal * zoom), 0.0F};
    p.board_rect = {static_cast<float>((-board_half_extent - camera.x) * zoom + viewport.x * 0.5),
        static_cast<float>((-board_half_extent - camera.y) * zoom + viewport.y * 0.5),
        static_cast<float>((board_half_extent - camera.x) * zoom + viewport.x * 0.5),
        static_cast<float>((board_half_extent - camera.y) * zoom + viewport.y * 0.5)};
    return p;
}
} // namespace sawer
