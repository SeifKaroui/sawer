#pragma once

#include "document/Object.hpp"
#include "ui/Toolbar.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace sawer {

// Display-only mapping. Exact palette matches protect intentional custom tints.
inline constexpr Color dark_board_paper{32, 36, 44, 255};
inline constexpr Color dark_board_ink{235, 240, 248, 255};
inline constexpr std::array<Color, 7> board_theme_sources{{
    {255, 255, 255, 255}, {235, 240, 248, 255},
    {240, 242, 247, 255}, {235, 238, 242, 255},
    {0, 0, 0, 255}, {30, 34, 42, 255}, {32, 36, 44, 255},
}};

inline bool adaptive_board_background(const Color color) noexcept
{
    for (std::size_t index = 0; index < 4; ++index) {
        const auto source = board_theme_sources[index];
        if (color.red == source.red && color.green == source.green && color.blue == source.blue)
            return true;
    }
    return false;
}

inline double board_theme_amount(const Toolbar& toolbar) noexcept
{
    if (!adaptive_board_background(toolbar.background_color())) return 0.0;
    const double t = std::clamp(toolbar.theme_transition(), 0.0, 1.0);
    const double eased = t * t * (3.0 - 2.0 * t);
    const double previous = toolbar.previous_theme() == Theme::dark ? 1.0 : 0.0;
    const double current = toolbar.theme() == Theme::dark ? 1.0 : 0.0;
    return previous + (current - previous) * eased;
}

inline Color board_display_color(const Color color, const double amount) noexcept
{
    for (std::size_t index = 0; index < board_theme_sources.size(); ++index) {
        const auto source = board_theme_sources[index];
        if (color.red != source.red || color.green != source.green || color.blue != source.blue) continue;
        const auto target = index < 4 ? dark_board_paper : dark_board_ink;
        const auto channel = [amount](const std::uint8_t from, const std::uint8_t to) {
            return static_cast<std::uint8_t>(std::lround(from + (to - from) * std::clamp(amount, 0.0, 1.0)));
        };
        return {channel(color.red, target.red), channel(color.green, target.green),
            channel(color.blue, target.blue), color.alpha};
    }
    return color;
}

// Identical source/target pairs for the retained and streamed GPU vertex paths.
struct BoardThemeParameters final {
    std::array<std::array<float, 4>, 7> sources, targets;
    std::array<float, 4> state;
};
static_assert(sizeof(BoardThemeParameters) == 15U * 16U);

inline BoardThemeParameters board_theme_parameters(const Toolbar& toolbar) noexcept
{
    BoardThemeParameters result{};
    const auto rgb = [](const Color color) {
        return std::array<float, 4>{color.red / 255.0F, color.green / 255.0F, color.blue / 255.0F, 1.0F};
    };
    for (std::size_t index = 0; index < board_theme_sources.size(); ++index) {
        result.sources[index] = rgb(board_theme_sources[index]);
        result.targets[index] = rgb(index < 4 ? dark_board_paper : dark_board_ink);
    }
    result.state[0] = static_cast<float>(board_theme_amount(toolbar));
    return result;
}

} // namespace sawer
