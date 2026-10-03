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
    REQUIRE(
        home.controls()[4U].bounds.y
            - (home.controls().front().bounds.y
                + home.controls().front().bounds.height)
        == Catch::Approx(20.0));
    const auto name_bounds = home.board_name_bounds(0U);
    REQUIRE(name_bounds.x
        == Catch::Approx(home.controls().front().bounds.x + 16.0));
    REQUIRE(name_bounds.width
        == Catch::Approx(home.rename_buttons().front().x - 10.0 - name_bounds.x));

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

TEST_CASE("home card feedback keeps edge targets fixed and settles on exit")
{
    for (const auto theme : {sawer::Theme::light, sawer::Theme::dark}) {
        for (const double scale : {0.85, 1.0, 1.5}) {
            sawer::HomeView home;
            home.update(1280.0, 900.0, scale, theme, boards(2U));
            const auto card = home.controls().front().bounds;
            const auto preview = home.board_preview_bounds(0U);
            const auto rename = home.rename_buttons().front();
            const sawer::Vec2d edge{card.x + 1.0, card.y + 1.0};

            home.set_pointer(edge);
            for (int frame = 0; frame < 12; ++frame) home.tick(0.1);
            REQUIRE(home.animation(0U).hover == 1.0);
            REQUIRE(home.board_at(edge) == 0U);
            home.pointer_down(edge);
            for (int frame = 0; frame < 12; ++frame) home.tick(0.1);
            REQUIRE(home.animation(0U).press == 1.0);
            REQUIRE(home.pointer_up(edge));
            REQUIRE(home.controls().front().bounds == card);
            REQUIRE(home.board_preview_bounds(0U) == preview);
            REQUIRE(home.rename_buttons().front() == rename);
            REQUIRE(home.control_offset(0U) == 0.0);

            home.clear_pointer();
            home.clear_focus();
            for (int frame = 0; frame < 12; ++frame) home.tick(0.1);
            REQUIRE(home.animation(0U).hover == 0.0);
            REQUIRE(home.animation(0U).press == 0.0);
        }
    }
}

TEST_CASE("home opens without staged card motion")
{
    sawer::HomeView home;
    home.update(
        1000.0, 700.0, 1.0, sawer::Theme::light, boards(8U));

    home.play_entrance();

    REQUIRE(home.reveal() == 1.0);
    for (std::size_t index = 0U; index < home.controls().size(); ++index) {
        REQUIRE(home.entrance(index) == 1.0);
        REQUIRE(home.control_offset(index) == 0.0);
    }
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

TEST_CASE("home shows file activity without treating it as an error")
{
    sawer::HomeView home;
    home.update(
        1280.0, 720.0, 1.0, sawer::Theme::light, {}, {},
        "Opening board.sawer...");

    REQUIRE(home.error_message().empty());
    REQUIRE(home.status_message() == "Opening board.sawer...");
    REQUIRE(home.status_bounds().width > 1.0);
    REQUIRE(home.grid_top()
        > home.status_bounds().y + home.status_bounds().height);
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

TEST_CASE("home dates use local calendar days across month and year boundaries")
{
    using namespace std::chrono;
    const year_month_day today{year{2026}, month{10}, day{1}};
    REQUIRE(sawer::HomeView::format_date("2026-10-01 17:21", today) == "Today 17:21");
    REQUIRE(sawer::HomeView::format_date("2026-09-30 23:59", today) == "Yesterday 23:59");
    REQUIRE(sawer::HomeView::format_date("2026-09-19 14:20", today) == "19 Sep 2026");
    REQUIRE(sawer::HomeView::format_date("2025-12-31 12:00",
        year{2026}/January/1) == "Yesterday 12:00");
    REQUIRE(sawer::HomeView::format_date("2024-02-29 12:00",
        year{2024}/March/1) == "Yesterday 12:00");
    REQUIRE(sawer::HomeView::format_date("2026-10-02 01:00", today) == "2 Oct 2026");
    for (const std::string_view invalid : {"", "Today", "2026-02-30 12:00",
             "2026-13-01 12:00", "2026-10-01 24:00", "2026-10-01 12:60"}) {
        REQUIRE(sawer::HomeView::format_date(invalid, today) == invalid);
    }
}

TEST_CASE("home footer actions leave previews and Unicode names clear at every scale")
{
    sawer::HomeView home;
    for (const double width : {320.0, 480.0, 760.0, 1280.0, 1920.0}) {
        for (const double scale : {1.0, 1.5}) {
            auto recent = boards(12U);
            recent[0].name = "Long Unicode board \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
            home.update(width, 720.0, scale, sawer::Theme::dark, std::move(recent));
            const auto card = home.controls().front().bounds;
            const auto rename = home.rename_buttons().front();
            const auto name = home.board_name_bounds(0U);
            const auto date = home.board_date_bounds(0U);
            const auto preview = home.board_preview_bounds(0U);
            const double footer_top = card.y + card.height - sawer::HomeView::card_text_area * home.scale();
            REQUIRE(rename.y >= footer_top);
            REQUIRE(rename.y + rename.height <= card.y + card.height);
            REQUIRE(rename.width >= 32.0);
            REQUIRE(name.width > 0.0);
            REQUIRE(name.x + name.width < rename.x);
            REQUIRE(date.x + date.width < rename.x);
            REQUIRE(date.y + date.height < card.y + card.height);
            REQUIRE(preview.y + preview.height < footer_top);
            REQUIRE(preview.width / preview.height == Catch::Approx(256.0 / 144.0));
            REQUIRE(preview.x + preview.width * 0.5 == Catch::Approx(card.x + card.width * 0.5));
            REQUIRE(preview.y + preview.height * 0.5 == Catch::Approx(card.y + (footer_top - card.y) * 0.5));
            const sawer::Vec2d rename_center{rename.x + rename.width * 0.5, rename.y + rename.height * 0.5};
            home.pointer_down(rename_center);
            REQUIRE(home.pointer_up(rename_center));
            REQUIRE(home.rename_at(rename_center) == 0U);
        }
    }
}

TEST_CASE("home timestamp tooltip waits for hover and follows scroll and layout")
{
    sawer::HomeView home;
    auto recent = boards(20U);
    for (auto& board : recent) board.date = "2026-09-19 14:20";
    home.update(1280.0, 720.0, 1.0, sawer::Theme::light, std::move(recent));
    REQUIRE(home.boards().front().date == "2026-09-19 14:20");
    REQUIRE(home.boards().front().display_date != home.boards().front().date);
    const auto date = home.board_date_bounds(0U);
    const sawer::Vec2d point{date.x + 4.0, date.y + 4.0};
    home.set_pointer(point);
    for (int frame = 0; frame < 4; ++frame) home.tick(0.1);
    REQUIRE_FALSE(home.date_tooltip().has_value());
    home.set_pointer({point.x + 1.0, point.y});
    home.tick(0.1);
    REQUIRE(home.date_tooltip() == 0U);
    for (int frame = 0; frame < 20; ++frame) home.tick(0.1);
    REQUIRE_FALSE(home.animating());
    home.pointer_down(point);
    REQUIRE_FALSE(home.date_tooltip().has_value());
    REQUIRE(home.pointer_up(point));
    for (int frame = 0; frame < 5; ++frame) home.tick(0.1);
    REQUIRE(home.date_tooltip() == 0U);
    home.scroll_rows(1);
    REQUIRE_FALSE(home.date_tooltip().has_value());
    for (int frame = 0; frame < 5; ++frame) home.tick(0.1);
    REQUIRE(home.date_tooltip() == 4U);
    home.relayout(480.0, 720.0, 1.5, sawer::Theme::dark);
    REQUIRE_FALSE(home.date_tooltip().has_value());
    home.clear_pointer();
    REQUIRE_FALSE(home.date_tooltip().has_value());
}
