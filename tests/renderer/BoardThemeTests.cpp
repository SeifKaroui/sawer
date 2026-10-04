#include "renderer/BoardTheme.hpp"
#include "renderer/BackgroundParameters.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("board themes adapt exact neutral colors without changing canonical colors")
{
    using namespace sawer;
    for (std::size_t index = 0; index < board_theme_sources.size(); ++index) {
        const auto original = board_theme_sources[index];
        REQUIRE(board_display_color(original, 0.0) == original);
        REQUIRE(board_display_color(original, 1.0) == (index < 4 ? dark_board_paper : dark_board_ink));
        auto translucent = original;
        translucent.alpha = 83;
        REQUIRE(board_display_color(translucent, 1.0).alpha == 83);
    }
    for (const Color color : {Color{239, 92, 92, 255}, Color{82, 145, 244, 255},
            Color{245, 232, 150, 255}, Color{254, 255, 255, 255}, Color{31, 34, 42, 255}}) {
        REQUIRE(board_display_color(color, 1.0) == color);
        REQUIRE_FALSE(adaptive_board_background(color));
    }
}

TEST_CASE("dark board grids use the displayed paper for automatic contrast")
{
    using namespace sawer;
    const auto background = board_display_color({255, 255, 255, 255}, 1.0);
    const auto parameters = background_parameters(BackgroundGridPattern::dot,
        background, std::nullopt, {0, 0}, {1280, 720}, 1.0, {1280, 720});
    REQUIRE(background == dark_board_paper);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        REQUIRE(parameters.dots[channel] > parameters.board[channel]);
        REQUIRE(parameters.axis[channel] > parameters.dots[channel]);
    }
}

TEST_CASE("board theme transitions restore colors and respect fixed backgrounds")
{
    using namespace sawer;
    Toolbar toolbar;
    const Color white{255, 255, 255, 255};
    const auto update = [&](const Color background) {
        toolbar.update(1280, 720, 1, Tool::pencil, {}, false, false, false,
            1, "Untitled", false, {}, BackgroundStyle::solid, background);
    };
    update(white);
    REQUIRE(board_theme_amount(toolbar) == 0.0);
    toolbar.toggle_theme();
    REQUIRE(board_theme_amount(toolbar) == 0.0);
    toolbar.tick(0.1);
    REQUIRE(board_theme_amount(toolbar) > 0.0);
    REQUIRE(board_theme_amount(toolbar) < 1.0);
    for (int frame = 0; frame < 4; ++frame) toolbar.tick(0.1);
    REQUIRE(board_theme_amount(toolbar) == 1.0);
    REQUIRE(toolbar.background_color() == white);
    REQUIRE(board_display_color(white, board_theme_amount(toolbar)) == dark_board_paper);
    update({245, 232, 150, 255});
    REQUIRE(board_theme_amount(toolbar) == 0.0);
    update(dark_board_paper);
    REQUIRE(board_theme_amount(toolbar) == 0.0);
    update(white);
    toolbar.toggle_theme();
    for (int frame = 0; frame < 4; ++frame) toolbar.tick(0.1);
    REQUIRE(board_theme_amount(toolbar) == 0.0);
    REQUIRE(board_display_color(white, board_theme_amount(toolbar)) == white);
}

TEST_CASE("Home preview themes ease in both directions with the gallery palette")
{
    using namespace sawer;
    HomeView home;
    home.update(1280, 720, 1, Theme::light, {});
    REQUIRE(board_theme_amount(home) == 0.0);
    home.relayout(1280, 720, 1, Theme::dark);
    REQUIRE(board_theme_amount(home) == 0.0);
    home.tick(0.1);
    REQUIRE(board_theme_amount(home) > 0.0);
    REQUIRE(board_theme_amount(home) < 1.0);
    for (int frame = 0; frame < 4; ++frame) home.tick(0.1);
    REQUIRE(board_theme_amount(home) == 1.0);
    home.relayout(1280, 720, 1, Theme::light);
    REQUIRE(board_theme_amount(home) == 1.0);
    for (int frame = 0; frame < 4; ++frame) home.tick(0.1);
    REQUIRE(board_theme_amount(home) == 0.0);
}
