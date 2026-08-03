#include "ui/HomeView.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

namespace {

std::vector<sawer::HomeBoard> boards(const std::size_t count)
{
    std::vector<sawer::HomeBoard> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        result.push_back({
            "Board " + std::to_string(index + 1U),
            "Today",
            {},
            "C:/Boards/Board " + std::to_string(index + 1U) + ".sawer",
        });
    }
    return result;
}

} // namespace

TEST_CASE("home gallery uses a centered responsive content grid")
{
    sawer::HomeView home;
    home.update(
        1280.0, 720.0, 1.0, sawer::Theme::light, boards(6U));

    REQUIRE(home.controls().size() == 9U);
    REQUIRE(home.rename_buttons().size() == 6U);
    REQUIRE(home.controls().front().bounds.x >= home.content_bounds().x);
    REQUIRE(home.controls().front().bounds.width >= 228.0);
    REQUIRE(home.controls().front().bounds.width <= 296.0);
    REQUIRE(home.controls()[4U].bounds.y > home.controls().front().bounds.y);

    const auto& open_board = home.controls()[6U];
    REQUIRE(open_board.action == sawer::UiAction::open_board);
    const auto& new_board = home.controls()[7U];
    REQUIRE(new_board.action == sawer::UiAction::home_new_board);
    REQUIRE(home.controls()[8U].action == sawer::UiAction::toggle_theme);
    REQUIRE(new_board.bounds.x > home.content_bounds().x
        + home.content_bounds().width * 0.5);
    const auto hero = home.header_bounds();
    REQUIRE(hero.width == home.content_bounds().width);
    REQUIRE(hero.contains({
        new_board.bounds.x + new_board.bounds.width * 0.5,
        new_board.bounds.y + new_board.bounds.height * 0.5,
    }));
    REQUIRE(hero.contains({
        home.heading_bounds().x,
        home.heading_bounds().y,
    }));
    REQUIRE(home.heading_bounds().y + home.heading_bounds().height
        <= hero.y + hero.height);
    REQUIRE(hero.y + hero.height < home.grid_top());

    home.update(
        620.0, 720.0, 1.0, sawer::Theme::light, boards(2U));
    const auto& compact_new = home.controls()[3U];
    REQUIRE(compact_new.bounds.x >= home.content_bounds().x);
    REQUIRE(compact_new.bounds.y > home.content_bounds().y);
    REQUIRE(home.grid_top() > compact_new.bounds.y);
    const auto compact_hero = home.header_bounds();
    REQUIRE(compact_hero.x == home.content_bounds().x);
    REQUIRE(compact_hero.width == home.content_bounds().width);
    REQUIRE(compact_hero.contains({
        compact_new.bounds.x + compact_new.bounds.width * 0.5,
        compact_new.bounds.y + compact_new.bounds.height * 0.5,
    }));

    home.update(
        320.0, 600.0, 1.0, sawer::Theme::light, {});
    const auto narrow_open = home.controls()[0U].bounds;
    const auto narrow_new = home.controls()[1U].bounds;
    REQUIRE(narrow_open.x >= home.content_bounds().x);
    REQUIRE(narrow_new.x + narrow_new.width
        <= home.content_bounds().x + home.content_bounds().width);
}

TEST_CASE("home relayout preserves cached recent data and interaction state")
{
    sawer::HomeView home;
    home.update(
        1280.0, 720.0, 1.0, sawer::Theme::light, boards(6U));

    const auto original_card = home.controls().front().bounds;
    home.set_pointer({
        original_card.x + original_card.width * 0.5,
        original_card.y + original_card.height * 0.5,
    });
    home.tick(0.1);
    const double hover = home.animation(0U).hover;

    home.relayout(900.0, 640.0, 1.0, sawer::Theme::dark);

    REQUIRE(home.boards().size() == 6U);
    REQUIRE(home.boards().front().name == "Board 1");
    REQUIRE(home.controls().size() == 9U);
    REQUIRE(home.controls().front().bounds.width != original_card.width);
    REQUIRE(home.animation(0U).hover == hover);
    REQUIRE(home.theme() == sawer::Theme::dark);
}

TEST_CASE("home keeps narrow high-DPI actions reachable")
{
    sawer::HomeView home;
    home.update(
        480.0, 420.0, 2.0, sawer::Theme::dark, boards(2U),
        "Could not open the selected board.");

    REQUIRE(home.scale() == Catch::Approx(0.82));
    for (const auto& control : home.controls()) {
        REQUIRE(control.bounds.x >= 0.0);
        REQUIRE(control.bounds.x + control.bounds.width <= 480.01);
        if (control.action != sawer::UiAction::home_open_board) {
            REQUIRE(control.bounds.width >= 32.0);
            REQUIRE(control.bounds.height >= 32.0);
        }
    }
    for (const auto bounds : home.rename_buttons()) {
        REQUIRE(bounds.width >= 32.0);
        REQUIRE(bounds.height >= 32.0);
    }
    REQUIRE(home.controls().front().bounds.y
        + home.controls().front().bounds.height <= 420.01);
}

TEST_CASE("home keyboard focus reaches and reveals every card")
{
    sawer::HomeView home;
    home.update(
        1280.0, 720.0, 1.0, sawer::Theme::light, boards(20U));

    for (std::size_t index = 0U; index < 13U; ++index) {
        home.focus_next();
    }
    REQUIRE(home.focused_index() == 12U);
    REQUIRE(home.focused_control() == &home.controls()[12U]);
    REQUIRE(home.top_row() > 0U);
    REQUIRE(home.controls()[12U].bounds.y >= home.grid_top());

    home.focus_next(true);
    REQUIRE(home.focused_index() == 11U);
    home.clear_focus();
    REQUIRE_FALSE(home.focused_index().has_value());
    REQUIRE(home.focused_control() == nullptr);
}

TEST_CASE("home empty recent state keeps both file actions visible")
{
    sawer::HomeView home;
    home.update(
        1000.0, 700.0, 1.0, sawer::Theme::dark, {});
    REQUIRE(home.panels().size() == 1U);
    REQUIRE(home.controls().size() == 3U);
    REQUIRE(home.panels().front().bounds.y > home.grid_top());
    REQUIRE(home.controls()[0U].action == sawer::UiAction::open_board);
    REQUIRE(home.controls()[1U].action == sawer::UiAction::home_new_board);
    REQUIRE(home.controls()[2U].action == sawer::UiAction::toggle_theme);
    REQUIRE_FALSE(home.controls()[0U].selected);
    REQUIRE(home.controls()[1U].selected);
}

TEST_CASE("home cards ease hover press and rename feedback")
{
    sawer::HomeView home;
    home.update(
        1000.0, 700.0, 1.0, sawer::Theme::light, boards(1U));

    const auto card = home.controls().front().bounds;
    const sawer::Vec2d center{
        card.x + card.width * 0.5,
        card.y + card.height * 0.5,
    };
    home.set_pointer(center);
    home.tick(0.1);
    REQUIRE(home.animation(0U).hover > 0.5);

    home.pointer_down(center);
    home.tick(0.1);
    REQUIRE(home.animation(0U).press > 0.5);

    const auto rename = home.rename_buttons().front();
    REQUIRE(home.pointer_up(center));
    home.set_pointer({
        rename.x + rename.width * 0.5,
        rename.y + rename.height * 0.5,
    });
    home.tick(0.1);
    REQUIRE(home.rename_hover(0U) > 0.5);
}

TEST_CASE("home activates only a matching press and release target")
{
    sawer::HomeView home;
    home.update(
        1000.0, 700.0, 1.0, sawer::Theme::light, boards(1U));

    const auto card = home.controls().front().bounds;
    const sawer::Vec2d card_center{
        card.x + card.width * 0.5,
        card.y + card.height * 0.5,
    };
    home.pointer_down(card_center);
    REQUIRE_FALSE(home.pointer_up({card.x - 10.0, card.y - 10.0}));

    home.pointer_down(card_center);
    REQUIRE(home.pointer_up(card_center));

    const auto rename = home.rename_buttons().front();
    const sawer::Vec2d rename_center{
        rename.x + rename.width * 0.5,
        rename.y + rename.height * 0.5,
    };
    home.pointer_down(rename_center);
    REQUIRE_FALSE(home.pointer_up(card_center));
    home.pointer_down(rename_center);
    REQUIRE(home.pointer_up(rename_center));

    // Entrance motion changes the visible hit region along with the card.
    home.play_entrance();
    home.tick(0.1);
    const double offset = home.control_offset(0U);
    const sawer::Vec2d animated_center{
        card_center.x,
        card_center.y + offset,
    };
    home.pointer_down(animated_center);
    REQUIRE(home.pointer_up(animated_center));
}

TEST_CASE("home error status takes layout space without covering cards")
{
    sawer::HomeView home;
    home.update(
        1000.0,
        700.0,
        1.0,
        sawer::Theme::dark,
        boards(2U),
        "The selected file is unavailable.");

    REQUIRE(home.error_message() == "The selected file is unavailable.");
    REQUIRE(home.status_bounds().width > 1.0);
    REQUIRE(home.status_bounds().y > home.heading_bounds().y);
    REQUIRE(home.status_bounds().y + home.status_bounds().height
        < home.grid_top());
    REQUIRE(home.controls().front().bounds.y >= home.grid_top());
}

TEST_CASE("home gallery scrolls by complete rows and exposes its position")
{
    sawer::HomeView home;
    home.update(
        1280.0, 720.0, 1.0, sawer::Theme::light, boards(20U));

    REQUIRE(home.maximum_top_row() > 0U);
    REQUIRE(home.top_row() == 0U);
    REQUIRE(home.scrollbar_track().height > 1.0);
    const double first_y = home.controls().front().bounds.y;

    home.scroll_rows(1);
    REQUIRE(home.top_row() == 1U);
    REQUIRE(home.controls().front().bounds.y < first_y);
    REQUIRE(home.controls().front().bounds.y
            + home.controls().front().bounds.height
        <= home.grid_top());
    // Four columns fit at this viewport, so the first card on the next row is
    // now aligned exactly to the top of the gallery.
    REQUIRE(home.controls()[4U].bounds.y == home.grid_top());
    REQUIRE_FALSE(home.board_at({
        home.controls().front().bounds.x + 4.0,
        home.heading_bounds().y + 4.0,
    }).has_value());

    home.play_entrance();
    home.tick(0.1);
    const auto old_row_overlap = home.board_at({
        home.controls().front().bounds.x + 4.0,
        home.grid_top() + 2.0,
    });
    REQUIRE((!old_row_overlap.has_value() || *old_row_overlap != 0U));

    home.scroll_rows(100);
    REQUIRE(home.top_row() == home.maximum_top_row());
    REQUIRE(home.scrollbar_thumb().y > home.scrollbar_track().y);

    auto replacement = boards(20U);
    replacement[1U].path = "C:/Other/Board 2.sawer";
    home.update(
        1280.0, 720.0, 1.0, sawer::Theme::light,
        std::move(replacement));
    REQUIRE(home.top_row() == 0U);
}
