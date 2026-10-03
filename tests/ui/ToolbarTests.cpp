#include "ui/Toolbar.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <set>

TEST_CASE("toolbar exposes drawing controls and consumes its bounds")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0,
        720.0,
        1.25,
        sawer::Tool::rectangle,
        {},
        true,
        false,
        true,
        1.0,
        "ideas.sawer",
        true,
        {},
        sawer::BackgroundStyle::square,
        {});

    const auto* const rectangle = toolbar.find(sawer::UiAction::rectangle);
    REQUIRE(rectangle != nullptr);
    REQUIRE(rectangle->selected);

    const sawer::Vec2d center{
        rectangle->bounds.x + rectangle->bounds.width * 0.5,
        rectangle->bounds.y + rectangle->bounds.height * 0.5,
    };
    REQUIRE(toolbar.contains(center));
    REQUIRE_FALSE(toolbar.contains({20.0, toolbar.height() + 1.0}));
    REQUIRE(toolbar.action_at(center) == sawer::UiAction::rectangle);
}

TEST_CASE("toolbar eases hover press and selection states")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::square, {});
    const auto* const pencil = toolbar.find(sawer::UiAction::pencil);
    REQUIRE(pencil != nullptr);
    const sawer::Vec2d center{
        pencil->bounds.x + pencil->bounds.width * 0.5,
        pencil->bounds.y + pencil->bounds.height * 0.5,
    };

    toolbar.set_pointer(center);
    toolbar.tick(0.1);
    REQUIRE(toolbar.animation(sawer::UiAction::pencil).hover > 0.5);
    REQUIRE(toolbar.animation(sawer::UiAction::pencil).selected > 0.5);

    toolbar.pointer_down(center);
    toolbar.tick(0.1);
    REQUIRE(toolbar.animation(sawer::UiAction::pencil).press > 0.5);

    REQUIRE(toolbar.pointer_up(center) == sawer::UiAction::pencil);
    toolbar.clear_pointer();
    toolbar.tick(0.1);
    REQUIRE(toolbar.animation(sawer::UiAction::pencil).press < 0.5);
    REQUIRE(toolbar.animation(sawer::UiAction::pencil).hover < 0.5);
}

TEST_CASE("selection control activates in the editing milestone")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::square, {});

    const auto* const select = toolbar.find(sawer::UiAction::select);
    REQUIRE(select != nullptr);
    REQUIRE(select->enabled);
    REQUIRE(toolbar.action_at({
        select->bounds.x + 1.0,
        select->bounds.y + 1.0,
    }) == sawer::UiAction::select);
}

TEST_CASE("toolbar scales and reports palette choices")
{
    sawer::Toolbar toolbar;
    sawer::Style style;
    style.stroke_width = 5.0;
    toolbar.update(
        3840.0, 2160.0, 2.0, sawer::Tool::line, style, false, false, false, 2.0,
        "board.sawer", false, {}, sawer::BackgroundStyle::square, {});

    REQUIRE(toolbar.scale() == 1.5);
    REQUIRE(toolbar.zoom() == 2.0);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_thin) == 3.0);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_regular) == 5.0);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_bold) == 9.0);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_heavy) == 18.0);
    const auto* const regular_width =
        toolbar.find(sawer::UiAction::width_regular);
    REQUIRE(regular_width != nullptr);
    REQUIRE(regular_width->selected);
    REQUIRE(regular_width->tooltip == "Regular - 5 px");
    REQUIRE(toolbar.find(sawer::UiAction::width_thin)->tooltip == "Fine - 3 px");
    REQUIRE(toolbar.find(sawer::UiAction::width_bold)->tooltip == "Bold - 9 px");
    REQUIRE(toolbar.find(sawer::UiAction::width_heavy)->tooltip == "Heavy - 18 px");
    REQUIRE(
        sawer::Toolbar::color_for(sawer::UiAction::color_blue).blue == 244U);

    const auto original = toolbar.theme();
    toolbar.toggle_theme();
    REQUIRE(toolbar.theme() != original);
}

TEST_CASE("flat drawing workspace separates document tools and view controls")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    REQUIRE(toolbar.find(sawer::UiAction::pencil)->icon
        == sawer::UiIcon::pencil);
    REQUIRE(toolbar.find(sawer::UiAction::hand)->icon
        == sawer::UiIcon::hand);
    REQUIRE(toolbar.find(sawer::UiAction::go_home)->icon
        == sawer::UiIcon::home);
    REQUIRE(toolbar.find(sawer::UiAction::file_menu) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::new_board) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::open_board) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save_as) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::undo)->icon
        == sawer::UiIcon::undo);
    REQUIRE(toolbar.find(sawer::UiAction::preferences_menu)->icon
        == sawer::UiIcon::settings);
    REQUIRE(toolbar.find(sawer::UiAction::format_background) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::toggle_theme)->icon == sawer::UiIcon::moon);
    REQUIRE(toolbar.find(sawer::UiAction::about) == nullptr);
    const auto app_bar = toolbar.panels().front().bounds;
    REQUIRE(app_bar.x == 0.0);
    REQUIRE(app_bar.y == 0.0);
    REQUIRE(app_bar.width == 1280.0);
    REQUIRE(app_bar.height == toolbar.height());
    REQUIRE(toolbar.height() == 72.0);

    const auto* const home = toolbar.find(sawer::UiAction::go_home);
    const auto* const select = toolbar.find(sawer::UiAction::select);
    const auto* const hand = toolbar.find(sawer::UiAction::hand);
    REQUIRE(home->bounds.width == 40.0);
    REQUIRE(select->bounds.width == 40.0);
    REQUIRE(toolbar.panels()[1U].bounds.height == 56.0);
    REQUIRE(home->bounds.y < toolbar.height());
    REQUIRE(select->bounds.y == home->bounds.y);
    REQUIRE(select->bounds.y == hand->bounds.y);
    REQUIRE(select->bounds.x < hand->bounds.x);
    const auto tool_panel = toolbar.panels()[1U].bounds;
    for (const auto action : {sawer::UiAction::undo, sawer::UiAction::redo}) {
        const auto* control = toolbar.find(action);
        const sawer::Vec2d center{control->bounds.x + control->bounds.width * 0.5,
            control->bounds.y + control->bounds.height * 0.5};
        REQUIRE(tool_panel.contains(center));
        REQUIRE_FALSE(control->enabled);
        REQUIRE_FALSE(toolbar.action_at(center).has_value());
        REQUIRE(toolbar.contains(center));
    }
    REQUIRE(tool_panel.x + tool_panel.width * 0.5 == Catch::Approx(640.0));
    REQUIRE(
        tool_panel.y == Catch::Approx(8.0));
    REQUIRE(toolbar.find(sawer::UiAction::zoom_menu)->bounds.y
        > toolbar.viewport_height() * 0.5);

    const auto* const rename = toolbar.find(sawer::UiAction::rename_board);
    REQUIRE(rename != nullptr);
    REQUIRE(rename->icon == sawer::UiIcon::none);
    REQUIRE(toolbar.filename_bounds().contains({
        rename->bounds.x + rename->bounds.width * 0.5,
        rename->bounds.y + rename->bounds.height * 0.5,
    }));
    REQUIRE(toolbar.filename_bounds().y < toolbar.height());
    const auto* rename_button = toolbar.find(sawer::UiAction::rename_button);
    REQUIRE(rename_button != nullptr);
    REQUIRE(rename_button->icon == sawer::UiIcon::rename);
    REQUIRE(rename_button->bounds.x > rename->bounds.x + rename->bounds.width);

    toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "a-longer-board-name.sawer", false, {},
        sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.settings_open());
    REQUIRE(toolbar.settings_page() == sawer::SettingsPage::canvas);
    REQUIRE(toolbar.find(sawer::UiAction::grid_dot)->selected);
}

TEST_CASE("about flyout is responsive and its license scroll is bounded")
{
    sawer::Toolbar toolbar;
    toolbar.toggle_settings_panel(sawer::SettingsPage::preferences);
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    const auto* const about = toolbar.find(sawer::UiAction::about);
    REQUIRE(about != nullptr);
    REQUIRE(about->bounds.width >= 32.0);

    toolbar.toggle_settings_panel(sawer::SettingsPage::about);
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    REQUIRE(toolbar.settings_open());
    REQUIRE(toolbar.settings_page() == sawer::SettingsPage::about);
    REQUIRE(toolbar.settings_scrim_visible());
    REQUIRE(toolbar.find(sawer::UiAction::settings_close) != nullptr);
    const auto panel = toolbar.panels()[toolbar.panels().size() - 2U].bounds;
    REQUIRE(panel.x >= 0.0);
    REQUIRE(panel.y >= 0.0);
    REQUIRE(panel.x + panel.width <= toolbar.viewport_width() + 0.01);
    REQUIRE(panel.y + panel.height <= toolbar.viewport_height() + 0.01);

    toolbar.scroll_about(0.25);
    REQUIRE(toolbar.about_scroll() == Catch::Approx(0.25));
    toolbar.scroll_about(2.0);
    REQUIRE(toolbar.about_scroll() == 1.0);
    toolbar.set_about_scroll(-1.0);
    REQUIRE(toolbar.about_scroll() == 0.0);

    toolbar.update(
        420.0, 360.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    const auto narrow =
        toolbar.panels()[toolbar.panels().size() - 2U].bounds;
    REQUIRE(narrow.x >= 0.0);
    REQUIRE(narrow.y >= 0.0);
    REQUIRE(narrow.x + narrow.width <= toolbar.viewport_width() + 0.01);
    REQUIRE(narrow.y + narrow.height <= toolbar.viewport_height() + 0.01);
}

TEST_CASE("filename pill expands while editing and compacts after commit")
{
    sawer::Toolbar toolbar;
    const std::string long_name(64U, 'A');
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, long_name, false, {}, sawer::BackgroundStyle::dot, {}, {}, {},
        false);
    const double completed_width = toolbar.filename_bounds().width;
    const double completed_icon_x =
        toolbar.find(sawer::UiAction::rename_board)->bounds.x;

    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, long_name, false, {}, sawer::BackgroundStyle::dot, {}, {},
        {}, true, long_name.size(), 0U);

    REQUIRE(toolbar.filename_editing());
    REQUIRE(toolbar.filename_cursor() == long_name.size());
    REQUIRE(toolbar.filename_anchor() == 0U);
    REQUIRE(toolbar.filename_bounds().width == completed_width);
    REQUIRE(toolbar.find(sawer::UiAction::rename_board)->bounds.x
        == completed_icon_x);
    REQUIRE(toolbar.filename_bounds().width <= 620.0);
    REQUIRE(toolbar.find(sawer::UiAction::rename_board)->tooltip.empty());
}

TEST_CASE("document header preserves full Unicode names in its delayed tooltip")
{
    const std::string name = "Résumé — مخطط — 計画 — a long portable board filename.sawer";
    for (const double width : {480.0, 1280.0}) {
        for (const double dpi : {1.0, 1.5, 2.0}) {
            sawer::Toolbar toolbar;
            toolbar.update(width, 720.0, dpi, sawer::Tool::pencil, {}, false,
                false, false, 1.0, name, false, {}, sawer::BackgroundStyle::dot, {});
            const auto* rename = toolbar.find(sawer::UiAction::rename_board);
            REQUIRE(rename != nullptr);
            REQUIRE(rename->tooltip == name);
            const auto file = toolbar.find(sawer::UiAction::file_menu)->bounds;
            REQUIRE(rename->bounds.x - file.x - file.width == Catch::Approx(16.0 * toolbar.scale()));
            const sawer::Vec2d point{rename->bounds.x + rename->bounds.width * 0.5,
                rename->bounds.y + rename->bounds.height * 0.5};
            toolbar.set_pointer(point);
            REQUIRE(toolbar.tooltip_control() == nullptr);
            for (int tick = 0; tick < 5; ++tick) toolbar.tick(0.1);
            REQUIRE(toolbar.tooltip_control() == rename);
            REQUIRE_FALSE(toolbar.animating());
            toolbar.pointer_down(point);
            REQUIRE(toolbar.pointer_up(point) == sawer::UiAction::rename_board);
            REQUIRE(toolbar.tooltip_control() == nullptr);
        }
    }
}

TEST_CASE("rename controls stay aligned while names and save status change")
{
    for (const double width : {320.0, 480.0, 1280.0, 1920.0}) {
        for (const double dpi : {1.0, 1.5}) {
            sawer::Toolbar toolbar;
            const auto layout = [&](const std::string& name, bool dirty, bool editing) {
                toolbar.update(width, 720.0, dpi, sawer::Tool::pencil, {},
                    false, false, false, 1.0, name, dirty, {},
                    sawer::BackgroundStyle::dot, {}, std::nullopt, {},
                    editing, name.size(), 0U, true);
            };
            layout("Board", false, false);
            const auto title = toolbar.filename_bounds();
            const auto file = toolbar.find(sawer::UiAction::file_menu)->bounds;
            REQUIRE(toolbar.find(sawer::UiAction::file_menu)->icon == sawer::UiIcon::chevron_down);
            const auto* rename = toolbar.find(sawer::UiAction::rename_button);
            REQUIRE((rename != nullptr) == (width >= 480.0));
            std::optional<sawer::UiRect> button;
            if (rename != nullptr) {
                button = rename->bounds;
                REQUIRE(rename->icon == sawer::UiIcon::rename);
                REQUIRE(rename->bounds.width >= 32.0);
                REQUIRE(rename->bounds.height >= 32.0);
                REQUIRE(rename->bounds.x > title.x + title.width);
                REQUIRE(rename->bounds.x + rename->bounds.width
                    < toolbar.find(sawer::UiAction::toggle_theme)->bounds.x);
                const sawer::Vec2d center{button->x + button->width * 0.5,
                    button->y + button->height * 0.5};
                toolbar.pointer_down(center);
                REQUIRE(toolbar.pointer_up({title.x + title.width * 0.5,
                    title.y + title.height * 0.5}) == std::nullopt);
                toolbar.pointer_down(center);
                REQUIRE(toolbar.pointer_up(center) == sawer::UiAction::rename_button);
            }
            for (bool editing : {false, true}) {
                layout("Résumé — مخطط — 計画 — " + std::string(180U, 'W'), true, editing);
                REQUIRE(toolbar.filename_bounds() == title);
                REQUIRE(toolbar.find(sawer::UiAction::file_menu)->bounds == file);
                if (button) {
                    rename = toolbar.find(sawer::UiAction::rename_button);
                    REQUIRE(rename->bounds == *button);
                    REQUIRE(rename->enabled == !editing);
                }
            }
        }
    }
}

TEST_CASE("File exposes rename with a complete isolated mouse target")
{
    for (const double width : {280.0, 320.0, 480.0, 1280.0}) {
        for (const bool saved : {false, true}) {
            sawer::Toolbar toolbar;
            toolbar.toggle_settings_panel(sawer::SettingsPage::file);
            toolbar.update(width, 420.0, 1.5, sawer::Tool::pencil, {},
                false, false, false, 1.0, "Board", false, {},
                sawer::BackgroundStyle::dot, {}, std::nullopt, {},
                false, 0U, 0U, saved);
            const auto* rename = toolbar.find(sawer::UiAction::rename_file);
            REQUIRE(rename != nullptr);
            REQUIRE(rename->enabled);
            REQUIRE(rename->label == "Rename...");
            REQUIRE(rename->tooltip == "Rename board  F2");
            REQUIRE(toolbar.is_settings_control(rename->action));
            const auto menu = toolbar.panels()[toolbar.panels().size() - 2U].bounds;
            const auto file = toolbar.find(sawer::UiAction::file_menu)->bounds;
            REQUIRE(menu.x <= file.x);
            REQUIRE(menu.x + menu.width <= width - 12.0 * toolbar.scale() + 0.01);
            if (file.x + menu.width <= width - 12.0 * toolbar.scale()) {
                REQUIRE(menu.x == Catch::Approx(file.x));
            } else {
                REQUIRE(menu.x + menu.width
                    == Catch::Approx(width - 12.0 * toolbar.scale()));
            }
            REQUIRE(menu.y >= file.y + file.height);
            REQUIRE(rename->bounds.width >= 32.0);
            REQUIRE(rename->bounds.height >= 32.0);
            REQUIRE(menu.contains({rename->bounds.x, rename->bounds.y}));
            REQUIRE(menu.contains({rename->bounds.x + rename->bounds.width,
                rename->bounds.y + rename->bounds.height}));
            const auto open = toolbar.find(sawer::UiAction::open_board)->bounds;
            const auto save = toolbar.find(saved ? sawer::UiAction::save_copy
                : sawer::UiAction::save_as)->bounds;
            REQUIRE(open.y + open.height < rename->bounds.y);
            REQUIRE(rename->bounds.y + rename->bounds.height < save.y);
            const sawer::Vec2d center{rename->bounds.x + rename->bounds.width * 0.5,
                rename->bounds.y + rename->bounds.height * 0.5};
            toolbar.pointer_down(center);
            REQUIRE(toolbar.pointer_up(center) == sawer::UiAction::rename_file);
        }
    }
}

TEST_CASE("drawing workspace gives save errors a consumed inline surface")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {},
        false, false, false, 1.0, "Untitled", true,
        "The destination is read-only.",
        sawer::BackgroundStyle::dot, {});

    const sawer::UiRect error = toolbar.error_bounds();
    REQUIRE(error.width > 1.0);
    REQUIRE(error.y > toolbar.height());
    REQUIRE(toolbar.contains({
        error.x + error.width * 0.5,
        error.y + error.height * 0.5,
    }));
    REQUIRE(toolbar.filename_bounds().y < toolbar.height());

    toolbar.update(
        480.0, 420.0, 1.0, sawer::Tool::pencil, {},
        false, false, false, 1.0, "Untitled", true,
        "A very long error message that must remain inside the workspace.",
        sawer::BackgroundStyle::dot, {});
    const sawer::UiRect narrow_error = toolbar.error_bounds();
    const double narrow_margin = 12.0 * toolbar.scale();
    REQUIRE(narrow_error.x >= narrow_margin);
    REQUIRE(
        narrow_error.x + narrow_error.width
        <= 480.0 - narrow_margin);
    const auto zoom = toolbar.find(sawer::UiAction::zoom_out)->bounds;
    REQUIRE(narrow_error.x + narrow_error.width < zoom.x);
    const auto tools = toolbar.panels()[1U].bounds;
    REQUIRE(tools.y + tools.height < narrow_error.y);
}

TEST_CASE("drawing workspace distinguishes activity from errors")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, "Untitled", false, {}, sawer::BackgroundStyle::dot, {},
        std::nullopt, {}, false, 0U, 0U, false, false, 0.0, false, false,
        {}, sawer::StyleColorTarget::stroke, false, {}, "Opening board...");

    REQUIRE(toolbar.error_message().empty());
    REQUIRE(toolbar.error_bounds().width == 0.0);
    REQUIRE(toolbar.status_message() == "Opening board...");
    REQUIRE(toolbar.status_bounds().width > 1.0);
    REQUIRE(toolbar.contains({
        toolbar.status_bounds().x + 4.0,
        toolbar.status_bounds().y + 4.0,
    }));
}

TEST_CASE("hand tool is exposed as a selectable pan control")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::hand, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    const auto* const hand = toolbar.find(sawer::UiAction::hand);
    REQUIRE(hand != nullptr);
    REQUIRE(hand->selected);
    REQUIRE(hand->tooltip.find("hold Space") != std::string_view::npos);
    REQUIRE(toolbar.action_at({
        hand->bounds.x + hand->bounds.width * 0.5,
        hand->bounds.y + hand->bounds.height * 0.5,
    }) == sawer::UiAction::hand);
}

TEST_CASE("mouse focus does not pin a tooltip after hover exit")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::hand, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    const auto* const hand = toolbar.find(sawer::UiAction::hand);
    REQUIRE(hand != nullptr);
    const sawer::Vec2d center{
        hand->bounds.x + hand->bounds.width * 0.5,
        hand->bounds.y + hand->bounds.height * 0.5,
    };
    toolbar.pointer_down(center);
    static_cast<void>(toolbar.pointer_up(center));
    toolbar.clear_pointer();

    REQUIRE(toolbar.focused_control() == hand);
    REQUIRE(toolbar.focused_tooltip_control() == nullptr);

    toolbar.focus_next();
    REQUIRE(toolbar.focused_tooltip_control() != nullptr);
}

TEST_CASE("pencil style panel contains only color and width controls")
{
    for (const double scale : {1.0, 1.5, 2.0}) {
        sawer::Toolbar toolbar;
        toolbar.update(1920.0, 1440.0, scale, sawer::Tool::pencil, {}, false, false,
            false, 1.0, "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
        REQUIRE(toolbar.find(sawer::UiAction::color_white) != nullptr);
        REQUIRE(toolbar.find(sawer::UiAction::width_cycle) != nullptr);
        const auto* last = toolbar.find(sawer::UiAction::width_increase);
        REQUIRE(last != nullptr);
        for (const auto& control : toolbar.controls()) {
            if (toolbar.is_property_control(control.action)) {
                REQUIRE(control.bounds.y + control.bounds.height
                    <= last->bounds.y + last->bounds.height);
            }
        }
        // Pencil now has the same style controls as Line.
        const auto pencil_panel = toolbar.properties_bounds();
        toolbar.update(1920.0, 1440.0, scale, sawer::Tool::line, {}, false, false,
            false, 1.0, "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
        REQUIRE(toolbar.properties_bounds() == pencil_panel);
    }
}

TEST_CASE("canvas settings expose patterns grid colors and custom colors")
{
    sawer::Toolbar toolbar;
    toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
    toolbar.set_settings_page(sawer::SettingsPage::canvas);
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::diamond,
        {240U, 242U, 247U, 255U}, std::nullopt);

    REQUIRE(toolbar.find(sawer::UiAction::grid_diamond)->selected);
    REQUIRE(toolbar.find(sawer::UiAction::grid_color_auto)->selected);
    const auto* const background_custom =
        toolbar.find(sawer::UiAction::edit_background_custom);
    const auto* const grid_custom =
        toolbar.find(sawer::UiAction::edit_grid_custom);
    REQUIRE(background_custom != nullptr);
    REQUIRE(grid_custom != nullptr);
    REQUIRE(background_custom->icon == sawer::UiIcon::custom_color);
    REQUIRE(grid_custom->icon == sawer::UiIcon::custom_color);
    REQUIRE(background_custom->bounds.width > background_custom->bounds.height);
    REQUIRE(background_custom->label == "Custom");
    REQUIRE(background_custom->accent.has_value());
    REQUIRE(grid_custom->bounds.width == grid_custom->bounds.height);
    REQUIRE_FALSE(toolbar.settings_scrim_visible());

    toolbar.begin_custom_color(
        sawer::CustomColorTarget::background, {255U, 0U, 0U, 255U});
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::diamond,
        {255U, 0U, 0U, 255U}, std::nullopt);
    const auto* const hue = toolbar.find(sawer::UiAction::custom_hue_field);
    const auto* const field = toolbar.find(sawer::UiAction::custom_sv_field);
    REQUIRE(hue != nullptr);
    REQUIRE(field != nullptr);
    REQUIRE(toolbar.update_custom_color(
        sawer::UiAction::custom_hue_field,
        {hue->bounds.x + hue->bounds.width * 0.5, hue->bounds.y})
        .has_value());
    const auto custom = toolbar.update_custom_color(
        sawer::UiAction::custom_sv_field,
        {field->bounds.x + field->bounds.width * 0.7,
         field->bounds.y + field->bounds.height * 0.2});
    REQUIRE(custom.has_value());
    REQUIRE(toolbar.settings_page() == sawer::SettingsPage::color_editor);
    REQUIRE_FALSE(toolbar.settings_scrim_visible());
}

TEST_CASE("toolbar activates only when press and release target match")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    const auto* const pencil = toolbar.find(sawer::UiAction::pencil);
    const auto* const line = toolbar.find(sawer::UiAction::line);
    REQUIRE(pencil != nullptr);
    REQUIRE(line != nullptr);
    const sawer::Vec2d pencil_center{
        pencil->bounds.x + pencil->bounds.width * 0.5,
        pencil->bounds.y + pencil->bounds.height * 0.5,
    };
    const sawer::Vec2d line_center{
        line->bounds.x + line->bounds.width * 0.5,
        line->bounds.y + line->bounds.height * 0.5,
    };

    toolbar.pointer_down(pencil_center);
    REQUIRE_FALSE(toolbar.pointer_up(line_center).has_value());
    toolbar.pointer_down(pencil_center);
    REQUIRE(toolbar.pointer_up(pencil_center) == sawer::UiAction::pencil);
}

TEST_CASE("context rail follows the active tool while color stays available")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::hand, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.find(sawer::UiAction::color_amber) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::width_thin) == nullptr);
    toolbar.toggle_settings_panel(sawer::SettingsPage::view);
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::hand, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.find(sawer::UiAction::zoom_menu)->selected);
    REQUIRE(toolbar.find(sawer::UiAction::zoom_fit_content) != nullptr);
    REQUIRE_FALSE(toolbar.find(sawer::UiAction::zoom_fit_selection)->enabled);
    toolbar.close_settings_panel();

    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::line, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.find(sawer::UiAction::color_amber) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::toggle_fill) == nullptr);

    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.find(sawer::UiAction::color_amber) != nullptr);

    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::select, {}, true, false, true, 1.0,
        "board", true, {}, sawer::BackgroundStyle::dot, {}, std::nullopt, {},
        false, 0U, 0U, true, true);
    REQUIRE(toolbar.find(sawer::UiAction::fill_none) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::copy_selection) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::cut_selection) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::paste) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::duplicate) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::delete_selection) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::duplicate)->bounds.y
        > toolbar.height());
    REQUIRE(toolbar.find(sawer::UiAction::save) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save_as) == nullptr);
}

TEST_CASE("zoom popup fits its actions and stays separated from the zoom bar")
{
    for (const auto size : {sawer::Vec2d{480.0, 420.0}, sawer::Vec2d{1280.0, 720.0},
             sawer::Vec2d{3840.0, 2160.0}}) {
        for (const double dpi : {1.0, 1.5, 2.0}) {
            for (const bool has_selection : {false, true}) {
                CAPTURE(size.x, size.y, dpi, has_selection);
                sawer::Toolbar toolbar;
                toolbar.toggle_settings_panel(sawer::SettingsPage::view);
                toolbar.update(size.x, size.y, dpi, sawer::Tool::select, {}, false,
                    false, has_selection, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
                const auto popup = toolbar.panels()[toolbar.panels().size() - 2U].bounds;
                const auto zoom = toolbar.find(sawer::UiAction::zoom_menu)->bounds;
                const auto zoom_in = toolbar.find(sawer::UiAction::zoom_in)->bounds;
                const auto* reset = toolbar.find(sawer::UiAction::zoom_reset);
                const auto* fit = toolbar.find(sawer::UiAction::zoom_fit_selection);
                REQUIRE(reset->label == "Reset to 100%");
                REQUIRE(popup.height < 240.0 * toolbar.scale());
                REQUIRE(popup.y + popup.height + 12.0 * toolbar.scale()
                    <= zoom.y - 8.0 * toolbar.scale() + 0.01);
                REQUIRE(popup.x + popup.width
                    == Catch::Approx(zoom_in.x + zoom_in.width + 8.0 * toolbar.scale()));
                REQUIRE(popup.y + popup.height - fit->bounds.y - fit->bounds.height
                    == Catch::Approx(16.0 * toolbar.scale()));
                REQUIRE(fit->enabled == has_selection);
                for (const auto action : {sawer::UiAction::zoom_reset,
                         sawer::UiAction::zoom_fit_content, sawer::UiAction::zoom_fit_selection}) {
                    const auto bounds = toolbar.find(action)->bounds;
                    REQUIRE(bounds.height >= 32.0);
                    REQUIRE(popup.contains({bounds.x, bounds.y}));
                    REQUIRE(popup.contains({bounds.x + bounds.width, bounds.y + bounds.height}));
                    const sawer::Vec2d center{bounds.x + bounds.width * 0.5,
                        bounds.y + bounds.height * 0.5};
                    REQUIRE(toolbar.contains(center));
                    if (action != sawer::UiAction::zoom_fit_selection || has_selection) {
                        toolbar.pointer_down(center);
                        REQUIRE(toolbar.pointer_up(center) == action);
                    } else {
                        REQUIRE_FALSE(toolbar.action_at(center).has_value());
                    }
                }
            }
        }
    }
}

TEST_CASE("inline palette stays in the middle left options panel")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    const auto* const pencil = toolbar.find(sawer::UiAction::pencil);
    const auto* const first_color =
        toolbar.find(sawer::UiAction::color_white);
    const auto* const last_color =
        toolbar.find(sawer::UiAction::color_black);
    const auto* const first_width =
        toolbar.find(sawer::UiAction::width_thin);
    const auto* const custom =
        toolbar.find(sawer::UiAction::edit_stroke_custom);
    REQUIRE(pencil != nullptr);
    REQUIRE(first_color != nullptr);
    REQUIRE(last_color != nullptr);
    REQUIRE(first_width != nullptr);
    REQUIRE(custom != nullptr);
    REQUIRE(custom->icon == sawer::UiIcon::custom_color);
    REQUIRE(toolbar.panels().size() >= 4U);

    const sawer::UiRect tool_panel = toolbar.panels()[1U].bounds;
    const sawer::UiRect context_panel = toolbar.panels()[2U].bounds;
    REQUIRE(context_panel.x == Catch::Approx(16.0));
    REQUIRE(last_color->bounds.x >= first_color->bounds.x);
    REQUIRE(last_color->bounds.y > first_color->bounds.y);
    REQUIRE(context_panel.y > tool_panel.y + tool_panel.height);
    REQUIRE(context_panel.x + context_panel.width < tool_panel.x);
    const std::size_t panel_count_before_color_editor =
        toolbar.panels().size();

    for (const sawer::UiAction action : {
             sawer::UiAction::width_thin,
             sawer::UiAction::width_heavy,
         }) {
        const auto* const control = toolbar.find(action);
        REQUIRE(control != nullptr);
        REQUIRE(context_panel.contains({
            control->bounds.x + control->bounds.width * 0.5,
            control->bounds.y + control->bounds.height * 0.5,
        }));
    }
    for (const sawer::UiAction action : {
             sawer::UiAction::color_white,
             sawer::UiAction::color_black,
             sawer::UiAction::edit_stroke_custom,
         }) {
        const auto* const control = toolbar.find(action);
        REQUIRE(control != nullptr);
        REQUIRE(context_panel.contains({
            control->bounds.x + control->bounds.width * 0.5,
            control->bounds.y + control->bounds.height * 0.5,
        }));
    }

    const sawer::UiRect old_pencil_bounds = pencil->bounds;
    toolbar.begin_custom_color(
        sawer::CustomColorTarget::stroke, {24U, 110U, 190U, 255U});
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.settings_open());
    REQUIRE(toolbar.settings_page() == sawer::SettingsPage::color_editor);
    REQUIRE_FALSE(toolbar.settings_scrim_visible());
    REQUIRE(toolbar.style_color_editor_open());
    REQUIRE(
        toolbar.custom_color_target()
        == sawer::CustomColorTarget::stroke);
    REQUIRE(toolbar.find(sawer::UiAction::pencil) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::color_black) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::width_thin) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::rename_board) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::custom_hue_field) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::custom_sv_field) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::custom_color_done) != nullptr);
    REQUIRE(
        toolbar.panels().size()
        == panel_count_before_color_editor + 1U);
    const sawer::UiRect editor =
        toolbar.panels()[toolbar.panels().size() - 2U].bounds;
    REQUIRE(
        editor.x > context_panel.x + context_panel.width);
    REQUIRE(
        editor.y >= toolbar.height());
    const auto old_pencil_action = toolbar.action_at({
        old_pencil_bounds.x + old_pencil_bounds.width * 0.5,
        old_pencil_bounds.y + old_pencil_bounds.height * 0.5,
    });
    REQUIRE(old_pencil_action == sawer::UiAction::pencil);
}

TEST_CASE(
    "custom stroke color popover leaves uncovered chrome accessible")
{
    sawer::Toolbar toolbar;
    if (!toolbar.properties_open()) toolbar.toggle_properties();
    toolbar.update(
        436.0, 460.0, 1.0, sawer::Tool::line, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});
    const sawer::UiRect old_palette =
        toolbar.find(sawer::UiAction::color_black)->bounds;
    const sawer::UiRect old_width =
        toolbar.find(sawer::UiAction::width_thin)->bounds;
    const std::size_t initial_panel_count = toolbar.panels().size();

    toolbar.begin_custom_color(
        sawer::CustomColorTarget::stroke, {124U, 64U, 220U, 255U});
    toolbar.update(
        436.0, 460.0, 1.0, sawer::Tool::line, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    REQUIRE_FALSE(toolbar.settings_scrim_visible());
    REQUIRE(toolbar.style_color_editor_open());
    REQUIRE(toolbar.panels().size() == initial_panel_count + 1U);
    REQUIRE(toolbar.filename_bounds().width > 0.0);
    REQUIRE(toolbar.filename_bounds().height > 0.0);
    const sawer::UiRect editor =
        toolbar.panels()[toolbar.panels().size() - 2U].bounds;
    REQUIRE(editor.x >= 0.0);
    REQUIRE(editor.y >= 0.0);
    REQUIRE(editor.x + editor.width <= toolbar.viewport_width());
    REQUIRE(editor.y + editor.height <= toolbar.viewport_height());
    REQUIRE(toolbar.find(sawer::UiAction::line) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::color_black) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::width_thin) != nullptr);
    const auto old_palette_action = toolbar.action_at({
        old_palette.x + old_palette.width * 0.5,
        old_palette.y + old_palette.height * 0.5,
    });
    REQUIRE(old_palette_action == sawer::UiAction::color_black);
    const auto old_width_action = toolbar.action_at({
        old_width.x + old_width.width * 0.5,
        old_width.y + old_width.height * 0.5,
    });
    REQUIRE(old_width_action == sawer::UiAction::width_thin);
    toolbar.set_pointer({
        old_width.x + old_width.width * 0.5,
        old_width.y + old_width.height * 0.5,
    });
    const auto* const hovered = toolbar.hovered_control();
    REQUIRE(hovered != nullptr);
    REQUIRE(hovered->action == sawer::UiAction::width_thin);
    REQUIRE(toolbar.find(sawer::UiAction::settings_close) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::custom_color_done) != nullptr);
}

TEST_CASE("toolbar keyboard focus skips disabled controls")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::hand, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {});

    toolbar.focus_next();
    REQUIRE(toolbar.focused_control() != nullptr);
    REQUIRE(toolbar.focused_control()->enabled);
    const auto first = toolbar.focused_control()->action;
    toolbar.focus_next();
    REQUIRE(toolbar.focused_control() != nullptr);
    REQUIRE(toolbar.focused_control()->action != first);
    toolbar.clear_focus();
    REQUIRE(toolbar.focused_control() == nullptr);
}

TEST_CASE("settings flyouts isolate covered controls and keyboard focus")
{
    sawer::Toolbar toolbar;
    toolbar.update(
        480.0, 420.0, 2.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "A very long board title", false, {}, sawer::BackgroundStyle::dot, {});
    const auto rename = toolbar.find(sawer::UiAction::rename_board)->bounds;

    toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
    toolbar.update(
        480.0, 420.0, 2.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "A very long board title", false, {}, sawer::BackgroundStyle::dot, {});

    const auto settings =
        toolbar.panels()[toolbar.panels().size() - 2U].bounds;
    REQUIRE(settings.x >= 0.0);
    REQUIRE(settings.y >= 0.0);
    REQUIRE(settings.x + settings.width <= toolbar.viewport_width() + 0.01);
    REQUIRE(settings.y + settings.height <= toolbar.viewport_height() + 0.01);
    for (const auto& control : toolbar.controls()) {
        if (!toolbar.is_settings_control(control.action)) {
            continue;
        }
        REQUIRE(settings.contains({
            control.bounds.x + control.bounds.width * 0.5,
            control.bounds.y + control.bounds.height * 0.5,
        }));
    }
    if (settings.contains({
            rename.x + rename.width * 0.5,
            rename.y + rename.height * 0.5,
        })) {
        REQUIRE(toolbar.action_at({
                    rename.x + rename.width * 0.5,
                    rename.y + rename.height * 0.5,
                })
            != sawer::UiAction::rename_board);
    }

    toolbar.focus_next();
    REQUIRE(toolbar.focused_control() != nullptr);
    REQUIRE(
        toolbar.focused_control()->action
        == sawer::UiAction::settings_close);
    REQUIRE(toolbar.find(sawer::UiAction::settings_close)->bounds.width >= 32.0);
    REQUIRE(toolbar.find(sawer::UiAction::bg_color_0)->bounds.width >= 32.0);
}

TEST_CASE("selection context remains inside a narrow viewport")
{
    sawer::Toolbar toolbar;
    if (!toolbar.properties_open()) toolbar.toggle_properties();
    toolbar.update(
        480.0, 720.0, 1.0, sawer::Tool::select, {}, true, true, true, 1.0,
        "board", true, {}, sawer::BackgroundStyle::dot, {}, std::nullopt, {},
        false, 0U, 0U, true, true);

    for (const auto& control : toolbar.controls()) {
        REQUIRE(control.bounds.x >= 0.0);
        REQUIRE(control.bounds.x + control.bounds.width
            <= toolbar.viewport_width() + 0.01);
    }

    const sawer::UiRect tool_panel = toolbar.panels()[1U].bounds;
    const sawer::UiRect context_panel = toolbar.panels()[2U].bounds;
    REQUIRE(context_panel.x == Catch::Approx(16.0 * toolbar.scale()));
    REQUIRE(context_panel.y >= tool_panel.y + tool_panel.height);
    REQUIRE(context_panel.x + context_panel.width
        <= toolbar.viewport_width() + 0.01);
    for (const sawer::UiAction action : {
             sawer::UiAction::fill_none,
             sawer::UiAction::width_heavy,
             sawer::UiAction::duplicate,
             sawer::UiAction::delete_selection,
        }) {
        const auto* const control = toolbar.find(action);
        REQUIRE(control != nullptr);
        REQUIRE(context_panel.contains({
            control->bounds.x + control->bounds.width * 0.5,
            control->bounds.y + control->bounds.height * 0.5,
        }));
    }
    REQUIRE(context_panel.contains({
        toolbar.find(sawer::UiAction::color_black)->bounds.x
            + toolbar.find(sawer::UiAction::color_black)->bounds.width * 0.5,
        toolbar.find(sawer::UiAction::color_black)->bounds.y
            + toolbar.find(sawer::UiAction::color_black)->bounds.height * 0.5,
    }));
}

TEST_CASE("drawing properties keep common controls in stable relative positions")
{
    sawer::Toolbar toolbar;
    std::optional<sawer::UiRect> color_bounds;
    std::optional<sawer::UiRect> width_bounds;
    for (const auto tool : {sawer::Tool::pencil, sawer::Tool::line,
             sawer::Tool::rectangle, sawer::Tool::ellipse}) {
        toolbar.update(1280.0, 720.0, 1.0, tool, {}, true, true, false,
            1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
        auto color = toolbar.find(sawer::UiAction::color_white)->bounds;
        auto width = toolbar.find(sawer::UiAction::width_cycle)->bounds;
        color.y -= toolbar.properties_bounds().y;
        width.y -= toolbar.properties_bounds().y;
        if (color_bounds.has_value()) REQUIRE(color == *color_bounds);
        if (width_bounds.has_value()) REQUIRE(width == *width_bounds);
        color_bounds = color;
        width_bounds = width;
        REQUIRE(toolbar.properties_bounds().width == Catch::Approx(248.0));
        const auto tools = toolbar.panels()[1U].bounds;
        const double options_top = tools.y + tools.height + 16.0 * toolbar.scale();
        REQUIRE(toolbar.properties_bounds().y
            == Catch::Approx(options_top + (720.0 - 64.0 * toolbar.scale() - options_top - 440.0 * toolbar.scale()) * 0.5));
    }
}

TEST_CASE("compact properties scroll inside their surface and reveal keyboard focus")
{
    sawer::Toolbar toolbar;
    toolbar.close_properties();
    toolbar.tick(0.1);
    toolbar.tick(0.1);
    sawer::DrawingSettings settings;
    const auto layout = [&] {
        toolbar.update(480.0, 420.0, 1.0, sawer::Tool::pencil, {}, false,
            false, false, 1.0, "Untitled", false, {},
            sawer::BackgroundStyle::dot, {}, std::nullopt, settings);
    };
    layout();
    REQUIRE(toolbar.compact_properties());
    REQUIRE(toolbar.properties_bounds().width == 0.0);
    REQUIRE(toolbar.find(sawer::UiAction::properties_menu) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::color_white) == nullptr);
    if (!toolbar.properties_open()) toolbar.toggle_properties();
    toolbar.tick(0.1);
    toolbar.tick(0.1);
    layout();
    REQUIRE(toolbar.properties_open());
    REQUIRE(toolbar.properties_scroll_limit() > 0.0);
    const auto panel = toolbar.properties_bounds();
    REQUIRE(panel.y + panel.height < 420.0);
    const auto old_trail = toolbar.find(sawer::UiAction::width_cycle)->bounds;
    const sawer::Vec2d old_center{old_trail.x + old_trail.width * 0.5,
        old_trail.y + old_trail.height * 0.5};
    REQUIRE_FALSE(toolbar.properties_clip().contains(old_center));
    REQUIRE(toolbar.action_at(old_center) != sawer::UiAction::width_cycle);
    toolbar.set_pointer(old_center);
    const auto hovered = toolbar.hovered_control();
    REQUIRE((hovered == nullptr || hovered->action != sawer::UiAction::width_cycle));
    toolbar.scroll_properties(10000.0);
    REQUIRE(toolbar.properties_scroll() == toolbar.properties_scroll_limit());
    const auto trail = toolbar.find(sawer::UiAction::width_cycle)->bounds;
    const sawer::Vec2d center{trail.x + trail.width * 0.5, trail.y + trail.height * 0.5};
    REQUIRE(toolbar.properties_clip().contains(center));
    REQUIRE(toolbar.action_at(center) == sawer::UiAction::width_cycle);
    toolbar.pointer_down(center);
    toolbar.scroll_properties(-10000.0);
    REQUIRE_FALSE(toolbar.pointer_up(center).has_value());
    REQUIRE(toolbar.properties_scroll() == 0.0);
    toolbar.scroll_properties(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(toolbar.properties_scroll() == 0.0);
    toolbar.clear_focus();
    bool reached_trail = false;
    for (std::size_t index = 0; index < toolbar.controls().size(); ++index) {
        toolbar.focus_next();
        const auto* focused = toolbar.focused_control();
        REQUIRE(focused != nullptr);
        if (toolbar.is_property_control(focused->action)) {
            REQUIRE(focused->bounds.y >= toolbar.properties_clip().y - 0.01);
            REQUIRE(focused->bounds.y + focused->bounds.height
                <= toolbar.properties_clip().y + toolbar.properties_clip().height + 0.01);
        }
        reached_trail |= focused->action == sawer::UiAction::width_cycle;
    }
    REQUIRE(reached_trail);
    // Rebuilding the layout must preserve the scroll position.
    const double scroll = toolbar.properties_scroll();
    layout();
    REQUIRE(toolbar.properties_scroll() == scroll);
    toolbar.close_properties();
    toolbar.tick(0.1);
    toolbar.tick(0.1);
    layout();
    REQUIRE(toolbar.find(sawer::UiAction::color_white) == nullptr);
}

TEST_CASE("header menus isolate actions without duplicate control identities")
{
    for (const bool saved : {false, true}) {
        for (const auto page : {sawer::SettingsPage::file, sawer::SettingsPage::preferences}) {
            sawer::Toolbar toolbar;
            toolbar.toggle_settings_panel(page);
            toolbar.update(480.0, 420.0, 1.5, sawer::Tool::pencil, {},
                false, false, false, 1.0, "Long Unicode board \u0628\u0648\u0631\u062f", true,
                {}, sawer::BackgroundStyle::dot, {}, std::nullopt, {},
                false, 0U, 0U, saved);
            std::set<sawer::UiAction> actions;
            for (const auto& control : toolbar.controls()) {
                REQUIRE(actions.insert(control.action).second);
            }
            REQUIRE_FALSE(toolbar.settings_scrim_visible());
            const auto* theme = toolbar.find(sawer::UiAction::toggle_theme);
            REQUIRE(theme != nullptr);
            REQUIRE(theme->bounds.y < toolbar.height());
            REQUIRE_FALSE(toolbar.is_settings_control(sawer::UiAction::toggle_theme));
            REQUIRE(theme->bounds.x + theme->bounds.width
                < toolbar.find(sawer::UiAction::preferences_menu)->bounds.x);
            REQUIRE(toolbar.find(sawer::UiAction::save) == nullptr);
            const auto action = page == sawer::SettingsPage::file
                ? (saved ? sawer::UiAction::save_copy : sawer::UiAction::save_as)
                : sawer::UiAction::format_background;
            const auto* row = toolbar.find(action);
            REQUIRE(row != nullptr);
            REQUIRE(toolbar.action_at({row->bounds.x + row->bounds.width * 0.5,
                row->bounds.y + row->bounds.height * 0.5}) == action);
            for (std::size_t index = 0; index < toolbar.controls().size(); ++index) {
                toolbar.focus_next();
                REQUIRE(toolbar.is_settings_control(toolbar.focused_control()->action));
            }
            toolbar.close_settings_panel();
            REQUIRE_FALSE(toolbar.settings_open());
        }
    }
}

TEST_CASE("workspace targets and header groups fit supported sizes")
{
    for (const auto size : std::array<sawer::Vec2d, 5>{{
             {480.0, 420.0}, {760.0, 540.0}, {1280.0, 720.0},
             {1920.0, 1080.0}, {3840.0, 2160.0}}}) {
        for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
            for (const auto tool : {sawer::Tool::pencil, sawer::Tool::rectangle,
                     sawer::Tool::select, sawer::Tool::hand}) {
                sawer::Toolbar toolbar;
                toolbar.update(size.x, size.y, dpi, tool, {}, true, true,
                    true, 1.0, std::string(120U, 'A'), true, {},
                    sawer::BackgroundStyle::dot, {}, std::nullopt, {},
                    true, 120U, 0U, true, true);
                for (const auto& control : toolbar.controls()) {
                    const sawer::Vec2d center{control.bounds.x + control.bounds.width * 0.5,
                        control.bounds.y + control.bounds.height * 0.5};
                    if (toolbar.is_property_control(control.action) && !toolbar.properties_clip().contains(center)) {
                        REQUIRE(toolbar.action_at(center) != control.action);
                        continue;
                    }
                    REQUIRE(control.bounds.width >= 32.0);
                    REQUIRE(control.bounds.height >= 32.0);
                    REQUIRE(control.bounds.x >= 0.0);
                    REQUIRE(control.bounds.y >= 0.0);
                    REQUIRE(control.bounds.x + control.bounds.width <= size.x + 0.01);
                    REQUIRE(control.bounds.y + control.bounds.height <= size.y + 0.01);
                }
                const auto title = toolbar.filename_bounds();
                const auto utility = toolbar.find(sawer::UiAction::toggle_theme)->bounds;
                REQUIRE(title.x + title.width + 8.0 * toolbar.scale() <= utility.x);
                const auto file = toolbar.find(sawer::UiAction::file_menu)->bounds;
                REQUIRE(file.x + file.width < title.x);
                const auto zoom = toolbar.find(sawer::UiAction::zoom_in)->bounds;
                REQUIRE(zoom.x + zoom.width < size.x);
            }
        }
    }
}

TEST_CASE("top tool bar stays centered and clear of middle left options")
{
    for (const auto size : std::array<sawer::Vec2d, 4>{{
             {480.0, 420.0}, {760.0, 540.0}, {1280.0, 720.0}, {3840.0, 2160.0}}}) {
        for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
            sawer::Toolbar toolbar;
            if (!toolbar.properties_open()) toolbar.toggle_properties();
            std::optional<sawer::UiRect> initial_tools;
            for (const auto tool : {sawer::Tool::pencil, sawer::Tool::line,
                     sawer::Tool::rectangle, sawer::Tool::ellipse,
                     sawer::Tool::select, sawer::Tool::hand}) {
                toolbar.update(size.x, size.y, dpi, tool, {}, true, true,
                    false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
                const auto tools = toolbar.panels()[1U].bounds;
                if (initial_tools.has_value()) REQUIRE(tools == *initial_tools);
                initial_tools = tools;
                REQUIRE(tools.x + tools.width * 0.5 == Catch::Approx(size.x * 0.5));
                REQUIRE((tools.y < toolbar.height() || tools.y >= toolbar.height() + 12.0 * toolbar.scale()));
                double previous_right = tools.x;
                for (const auto action : {sawer::UiAction::select, sawer::UiAction::hand,
                         sawer::UiAction::pencil, sawer::UiAction::line,
                         sawer::UiAction::rectangle, sawer::UiAction::ellipse,
                         sawer::UiAction::undo, sawer::UiAction::redo}) {
                    const auto* control = toolbar.find(action);
                    REQUIRE(control != nullptr);
                    REQUIRE(control->bounds.x > previous_right);
                    REQUIRE(control->bounds.y == Catch::Approx(tools.y + 8.0 * toolbar.scale()));
                    REQUIRE(control->bounds.x + control->bounds.width < tools.x + tools.width);
                    const sawer::Vec2d center{control->bounds.x + control->bounds.width * 0.5,
                        control->bounds.y + control->bounds.height * 0.5};
                    REQUIRE(toolbar.action_at(center) == action);
                    previous_right = control->bounds.x + control->bounds.width;
                }
                const auto options = toolbar.properties_bounds();
                if (options.width > 0.0) {
                    REQUIRE(options.x == Catch::Approx(16.0 * toolbar.scale()));
                    REQUIRE(options.y >= tools.y + tools.height + 16.0 * toolbar.scale());
                    const double available_top = tools.y + tools.height + 16.0 * toolbar.scale();
                    const double available_bottom = size.y - 64.0 * toolbar.scale();
                    REQUIRE(options.y
                        == Catch::Approx(available_top + std::max(0.0, available_bottom - available_top - 440.0 * toolbar.scale()) * 0.5));
                    REQUIRE(options.y + options.height <= available_bottom + 0.01);
                }
                if (toolbar.compact_properties()) {
                    const auto* toggle = toolbar.find(sawer::UiAction::properties_menu);
                    if (tool == sawer::Tool::hand || tool == sawer::Tool::select) {
                        REQUIRE(toggle == nullptr);
                    } else {
                        REQUIRE(toggle != nullptr);
                        REQUIRE(toggle->enabled);
                    }
                }
            }
        }
    }
}

TEST_CASE("color tiles use consistent mouse targets and selection states")
{
    for (const double dpi : {1.0, 1.25, 1.5}) {
        sawer::Toolbar toolbar;
        if (!toolbar.properties_open()) toolbar.toggle_properties();
        sawer::Style style;
        for (const auto selected : {sawer::UiAction::color_white,
                 sawer::UiAction::color_amber, sawer::UiAction::color_black}) {
            style.stroke = sawer::Toolbar::color_for(selected);
            toolbar.update(1280.0, 720.0, dpi, sawer::Tool::pencil, style,
                false, false, false, 1.0, "Board", false, {},
                sawer::BackgroundStyle::dot, {});
            const auto* first = toolbar.find(sawer::UiAction::color_white);
            const auto* custom = toolbar.find(sawer::UiAction::edit_stroke_custom);
            REQUIRE(first != nullptr);
            REQUIRE(custom != nullptr);
            REQUIRE(first->bounds.height >= 40.0);
            REQUIRE(custom->bounds.height == first->bounds.height);
            REQUIRE(custom->bounds.width == first->bounds.width);
            REQUIRE_FALSE(custom->selected);
            REQUIRE(toolbar.find(selected)->selected);
            for (const auto color : {sawer::UiAction::color_white,
                     sawer::UiAction::color_red, sawer::UiAction::color_amber,
                     sawer::UiAction::color_green, sawer::UiAction::color_blue,
                     sawer::UiAction::color_violet, sawer::UiAction::color_black}) {
                const auto* tile = toolbar.find(color);
                REQUIRE(tile->bounds.height == first->bounds.height);
                REQUIRE(tile->bounds.width == first->bounds.width);
                REQUIRE(tile->selected == (color == selected));
                REQUIRE_FALSE(tile->tooltip.empty());
            }
        }
        style.stroke = {21U, 120U, 152U, 255U};
        toolbar.update(1280.0, 720.0, dpi, sawer::Tool::pencil, style,
            false, false, false, 1.0, "Board", false, {},
            sawer::BackgroundStyle::dot, {});
        const auto* custom = toolbar.find(sawer::UiAction::edit_stroke_custom);
        REQUIRE(custom->selected);
        REQUIRE(custom->accent == style.stroke);
    }
}

TEST_CASE("color picker separates labels tracks preview and footer at every supported size")
{
    for (const auto size : std::array<sawer::Vec2d, 4>{{
             {480.0, 420.0}, {760.0, 540.0}, {1280.0, 720.0}, {3840.0, 2160.0}}}) {
        for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
            for (const auto target : {sawer::CustomColorTarget::stroke,
                     sawer::CustomColorTarget::fill, sawer::CustomColorTarget::background,
                     sawer::CustomColorTarget::grid}) {
                sawer::Toolbar toolbar;
                toolbar.begin_custom_color(target, {42U, 105U, 220U, 255U});
                toolbar.update(size.x, size.y, dpi, sawer::Tool::rectangle,
                    {}, false, false, false, 1.0, "Board", false,
                    {}, sawer::BackgroundStyle::dot, {});
                const auto panel = toolbar.panels()[toolbar.panels().size() - 2U].bounds;
                const auto sv = toolbar.find(sawer::UiAction::custom_sv_field)->bounds;
                const auto hue = toolbar.find(sawer::UiAction::custom_hue_field)->bounds;
                const auto done = toolbar.find(sawer::UiAction::custom_color_done)->bounds;
                const auto preview = toolbar.custom_color_preview_bounds();
                const double scale = toolbar.scale();
                REQUIRE(sv.height >= 96.0 * scale);
                REQUIRE(hue.height >= 32.0);
                REQUIRE(done.height >= 32.0);
                // A full label row plus padding separates the two gradients.
                REQUIRE(sv.y + sv.height + 32.0 * scale <= hue.y + 0.01);
                REQUIRE(hue.y + hue.height + 12.0 * scale <= preview.y + 0.01);
                REQUIRE(preview.y + preview.height + 12.0 * scale <= done.y + 0.01);
                for (const auto bounds : {sv, hue, preview, done}) {
                    REQUIRE(bounds.x >= panel.x);
                    REQUIRE(bounds.y >= panel.y);
                    REQUIRE(bounds.x + bounds.width <= panel.x + panel.width + 0.01);
                    REQUIRE(bounds.y + bounds.height <= panel.y + panel.height + 0.01);
                }
                const auto hue_track = toolbar.color_field_bounds(sawer::UiAction::custom_hue_field);
                REQUIRE(hue_track.height < hue.height);
                REQUIRE(hue.contains({hue_track.x, hue_track.y}));
                // The HEX portion of the preview is an editable control.
                const sawer::Vec2d preview_center{preview.x + preview.width * 0.5,
                    preview.y + preview.height * 0.5};
                REQUIRE(toolbar.contains(preview_center));
                REQUIRE(toolbar.action_at(preview_center) == sawer::UiAction::custom_hex_field);
            }
        }
    }
}

TEST_CASE("color picker pointer mapping matches the rendered tracks")
{
    sawer::Toolbar toolbar;
    toolbar.begin_custom_color(sawer::CustomColorTarget::stroke, {0U, 0U, 255U, 255U});
    toolbar.update(1280.0, 720.0, 1.0, sawer::Tool::line, {}, false,
        false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
    const auto hue = toolbar.color_field_bounds(sawer::UiAction::custom_hue_field);
    REQUIRE(toolbar.update_custom_color(sawer::UiAction::custom_hue_field,
        {hue.x + hue.width * 0.5, hue.y}).has_value());
    REQUIRE(toolbar.custom_hue() == Catch::Approx(0.5));
    const auto sv = toolbar.color_field_bounds(sawer::UiAction::custom_sv_field);
    const auto middle = toolbar.update_custom_color(sawer::UiAction::custom_sv_field,
        {sv.x + sv.width * 0.5, sv.y + sv.height * 0.5});
    REQUIRE(middle == sawer::Color{64U, 128U, 128U, 255U});
    REQUIRE(toolbar.update_custom_color(sawer::UiAction::custom_sv_field,
        {sv.x, sv.y}) == sawer::Color{255U, 255U, 255U, 255U});
    REQUIRE(toolbar.update_custom_color(sawer::UiAction::custom_sv_field,
        {sv.x + sv.width, sv.y + sv.height}) == sawer::Color{0U, 0U, 0U, 255U});
    REQUIRE(toolbar.update_custom_color(sawer::UiAction::custom_sv_field,
        {sv.x - 100.0, sv.y - 100.0}) == sawer::Color{255U, 255U, 255U, 255U});
    REQUIRE(toolbar.color_field_bounds(sawer::UiAction::pencil).width == 0.0);
}

TEST_CASE("style controls keep their anchor as tools change")
{
    sawer::Toolbar toolbar;
    const auto update = [&](sawer::Tool tool) {
        toolbar.update(1280.0, 900.0, 1.0, tool, {}, false, false, false,
            1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
    };
    update(sawer::Tool::line);
    const auto color = toolbar.find(sawer::UiAction::color_white)->bounds;
    const auto width = toolbar.find(sawer::UiAction::width_thin)->bounds;
    for (auto tool : {sawer::Tool::pencil, sawer::Tool::rectangle, sawer::Tool::ellipse}) {
        update(tool);
        REQUIRE(toolbar.find(sawer::UiAction::color_white)->bounds.y == color.y);
        REQUIRE(toolbar.find(sawer::UiAction::width_thin)->bounds.y == width.y);
    }
}

TEST_CASE("bottom style control focus outline fits inside the rendering clip")
{
    for (const double scale : {0.82, 1.0, 1.5}) {
        sawer::Toolbar toolbar;
        toolbar.update(1920.0, 1440.0, scale, sawer::Tool::pencil, {}, false,
            false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
        REQUIRE(toolbar.properties_scroll_limit() == 0.0);
        const auto* control = toolbar.find(sawer::UiAction::width_increase);
        REQUIRE(control != nullptr);
        const auto bounds = control->bounds;
        const double margin = 2.0 * toolbar.scale();
        const auto render_clip = toolbar.properties_render_clip();
        const sawer::Vec2d outline_bottom{
            bounds.x + bounds.width + margin, bounds.y + bounds.height + margin};
        REQUIRE(render_clip.contains({bounds.x - margin, bounds.y - margin}));
        REQUIRE(render_clip.contains(outline_bottom));
        REQUIRE(toolbar.properties_bounds().contains(outline_bottom));
        REQUIRE_FALSE(toolbar.properties_clip().contains(outline_bottom));
        REQUIRE(toolbar.action_at(outline_bottom) != sawer::UiAction::width_increase);
    }
}

TEST_CASE("style visibility survives resizing and header menus")
{
    sawer::Toolbar toolbar;
    const auto update = [&](double width) {
        toolbar.update(width, 720.0, 1.0, sawer::Tool::rectangle, {}, false,
            false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
    };
    for (double width : {1280.0, 480.0, 1280.0}) {
        update(width);
        REQUIRE(toolbar.properties_open());
        const auto* toggle = toolbar.find(sawer::UiAction::properties_menu);
        REQUIRE(toggle->icon == sawer::UiIcon::chevron_left);
        REQUIRE(toolbar.properties_bounds().contains({toggle->bounds.x, toggle->bounds.y}));
    }
    toolbar.toggle_settings_panel(sawer::SettingsPage::file);
    update(1280.0);
    REQUIRE(toolbar.properties_open());
    toolbar.close_settings_panel();
    toolbar.close_properties();
    toolbar.tick(0.1);
    toolbar.tick(0.1);
    for (double width : {480.0, 1280.0}) {
        update(width);
        REQUIRE_FALSE(toolbar.properties_open());
        REQUIRE(toolbar.find(sawer::UiAction::color_white) == nullptr);
    }
}

TEST_CASE("autosave feedback stays in document status and first save stays in File")
{
    sawer::Toolbar toolbar;
    const auto update = [&](bool has_file, bool dirty, std::string error = {}) {
        toolbar.update(1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false,
            false, false, 1.0, "Board", dirty, error, sawer::BackgroundStyle::dot,
            {}, std::nullopt, {}, false, 0U, 0U, has_file);
    };
    update(false, false);
    REQUIRE(toolbar.find(sawer::UiAction::save_as) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save) == nullptr);
    REQUIRE(toolbar.document_status() == "Not saved");
    const auto history_bounds = toolbar.find(sawer::UiAction::undo)->bounds;
    toolbar.toggle_settings_panel(sawer::SettingsPage::file);
    update(false, false);
    const auto* first_save = toolbar.find(sawer::UiAction::save_as);
    REQUIRE(first_save != nullptr);
    REQUIRE(first_save->enabled);
    REQUIRE(first_save->label == "Save...");
    const sawer::Vec2d center{first_save->bounds.x + first_save->bounds.width * 0.5,
        first_save->bounds.y + first_save->bounds.height * 0.5};
    REQUIRE(toolbar.is_settings_control(sawer::UiAction::save_as));
    toolbar.pointer_down(center);
    REQUIRE(toolbar.pointer_up(center) == sawer::UiAction::save_as);
    toolbar.close_settings_panel();

    update(true, true);
    REQUIRE(toolbar.document_status() == "Autosave pending");
    REQUIRE(toolbar.find(sawer::UiAction::save) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save_as) == nullptr);
    update(true, false);
    REQUIRE(toolbar.document_status() == "Saved");
    REQUIRE(toolbar.find(sawer::UiAction::undo)->bounds == history_bounds);

    update(true, true, "Write failed");
    REQUIRE(toolbar.document_status() == "Needs attention");
    REQUIRE(toolbar.error_bounds().width > 0.0);
    REQUIRE(toolbar.find(sawer::UiAction::save) == nullptr);
    update(true, false);
    REQUIRE(toolbar.document_status() == "Saved");
    toolbar.toggle_settings_panel(sawer::SettingsPage::file);
    update(true, false);
    REQUIRE(toolbar.find(sawer::UiAction::save_as) == nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save_copy)->label == "Save as...");
}

TEST_CASE("style panel retracts locally and keeps its header outside scrolling content")
{
    sawer::Toolbar toolbar;
    sawer::DrawingSettings settings;
    const auto update = [&] {
        toolbar.update(480.0, 420.0, 1.0, sawer::Tool::pencil, {}, false,
            false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot,
            {}, std::nullopt, settings);
    };
    update();
    const auto tools = toolbar.panels()[1U].bounds;
    const auto collapse = toolbar.find(sawer::UiAction::properties_menu)->bounds;
    const sawer::Vec2d collapse_center{collapse.x + collapse.width * 0.5,
        collapse.y + collapse.height * 0.5};
    REQUIRE_FALSE(tools.contains(collapse_center));
    REQUIRE_FALSE(toolbar.is_property_control(sawer::UiAction::properties_menu));
    REQUIRE(collapse.y + collapse.height < toolbar.properties_clip().y);
    toolbar.scroll_properties(10000.0);
    REQUIRE(toolbar.find(sawer::UiAction::properties_menu)->bounds == collapse);
    REQUIRE(toolbar.action_at(collapse_center) == sawer::UiAction::properties_menu);

    toolbar.toggle_properties();
    toolbar.tick(0.1);
    toolbar.tick(0.1);
    update();
    const auto* tab = toolbar.find(sawer::UiAction::properties_menu);
    REQUIRE(tab->label == "Style");
    REQUIRE(tab->icon == sawer::UiIcon::chevron_right);
    REQUIRE(tab->bounds.y == collapse.y - 4.0 * toolbar.scale());
    const sawer::Vec2d tab_center{tab->bounds.x + tab->bounds.width * 0.5,
        tab->bounds.y + tab->bounds.height * 0.5};
    REQUIRE(toolbar.contains(tab_center));
    toolbar.pointer_down(tab_center);
    REQUIRE(toolbar.pointer_up(tab_center) == sawer::UiAction::properties_menu);
    REQUIRE(toolbar.find(sawer::UiAction::color_white) == nullptr);
    REQUIRE(toolbar.panels()[1U].bounds == tools);
    toolbar.toggle_properties();
    toolbar.tick(0.1);
    toolbar.tick(0.1);
    update();
    REQUIRE(toolbar.find(sawer::UiAction::properties_menu)->bounds == collapse);
    REQUIRE(toolbar.find(sawer::UiAction::color_white) != nullptr);
}

TEST_CASE("HEX editing accepts exact colors and preserves pending color on invalid input")
{
    sawer::Toolbar toolbar;
    const sawer::Color original{24U, 110U, 190U, 255U};
    toolbar.begin_custom_color(sawer::CustomColorTarget::stroke, original);
    REQUIRE(toolbar.initial_custom_color() == original);
    for (const auto input : {"#aBc", "AABBCC", "#aabbcc"}) {
        toolbar.begin_hex_edit();
        REQUIRE(toolbar.hex_selected());
        toolbar.insert_hex_text(input);
        REQUIRE(toolbar.finish_hex_edit());
        REQUIRE(toolbar.custom_color() == sawer::Color{170U, 187U, 204U, 255U});
        REQUIRE_FALSE(toolbar.hex_editing());
    }
    for (const auto input : {"", "#12", "#abcd", "#G0ff00", "1234567890"}) {
        const auto before = toolbar.custom_color();
        toolbar.begin_hex_edit();
        toolbar.insert_hex_text(input);
        REQUIRE_FALSE(toolbar.finish_hex_edit());
        REQUIRE_FALSE(toolbar.hex_valid());
        REQUIRE(toolbar.hex_editing());
        REQUIRE(toolbar.custom_color() == before);
        toolbar.cancel_hex_edit();
        REQUIRE(toolbar.hex_valid());
    }
    REQUIRE(toolbar.initial_custom_color() == original);
}

TEST_CASE("recent custom colors are bounded and deduplicated in most recent order")
{
    sawer::Toolbar toolbar;
    toolbar.begin_custom_color(sawer::CustomColorTarget::stroke, {});
    for (std::uint8_t value = 1U; value <= 8U; ++value) {
        toolbar.set_custom_color({value, 30U, 200U, 255U});
        toolbar.remember_custom_color();
    }
    const auto update = [&]() {
        toolbar.update(1280.0, 720.0, 1.0, sawer::Tool::line, {}, false,
            false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
    };
    update();
    for (int index = 0; index < 6; ++index) {
        const auto action = static_cast<sawer::UiAction>(static_cast<int>(sawer::UiAction::recent_color_0) + index);
        REQUIRE(toolbar.find(action)->accent == sawer::Color{static_cast<std::uint8_t>(8 - index), 30U, 200U, 255U});
    }
    toolbar.set_custom_color({5U, 30U, 200U, 255U});
    toolbar.remember_custom_color();
    update();
    REQUIRE(toolbar.find(sawer::UiAction::recent_color_0)->accent == sawer::Color{5U, 30U, 200U, 255U});
    REQUIRE(toolbar.find(sawer::UiAction::recent_color_1)->accent == sawer::Color{8U, 30U, 200U, 255U});
    REQUIRE(toolbar.find(sawer::UiAction::recent_color_5)->accent == sawer::Color{3U, 30U, 200U, 255U});
}

TEST_CASE("tooltips wait dismiss on click and settle without idle animation")
{
    sawer::Toolbar toolbar;
    toolbar.update(1280.0, 720.0, 1.0, sawer::Tool::line, {}, false,
        false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
    const auto bounds = toolbar.find(sawer::UiAction::pencil)->bounds;
    const sawer::Vec2d point{bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5};
    toolbar.set_pointer(point);
    for (int tick = 0; tick < 4; ++tick) toolbar.tick(0.1);
    REQUIRE(toolbar.tooltip_control() == nullptr);
    toolbar.tick(0.1);
    REQUIRE(toolbar.tooltip_control()->action == sawer::UiAction::pencil);
    // Tooltip visibility can change on the final animation tick. The app must
    // present that settled state without waiting for another pointer event.
    REQUIRE_FALSE(toolbar.animating());
    for (int tick = 0; tick < 100; ++tick) toolbar.tick(0.1);
    REQUIRE_FALSE(toolbar.animating());
    toolbar.pointer_down(point);
    REQUIRE(toolbar.pointer_up(point) == sawer::UiAction::pencil);
    for (int tick = 0; tick < 100; ++tick) toolbar.tick(0.1);
    REQUIRE(toolbar.tooltip_control() == nullptr);
    REQUIRE_FALSE(toolbar.animating());
    toolbar.clear_pointer();
    toolbar.set_pointer(point);
    REQUIRE(toolbar.tooltip_control() == nullptr);
    toolbar.focus_next();
    REQUIRE(toolbar.tooltip_control() == toolbar.focused_control());
}

TEST_CASE("style retract animation shares visible bounds with input and settles")
{
    for (const auto viewport : {sawer::Vec2d{480.0, 420.0}, sawer::Vec2d{1920.0, 1440.0}}) {
        for (const double scale : {0.82, 1.0, 1.5}) {
            sawer::Toolbar toolbar;
            const auto layout = [&] {
                toolbar.update(viewport.x, viewport.y, scale, sawer::Tool::pencil,
                    {}, false, false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
            };
            layout();
            const auto expanded = toolbar.properties_bounds();
            const auto tools = toolbar.panels()[1U].bounds;
            toolbar.scroll_properties(10000.0);
            const double saved_scroll = toolbar.properties_scroll();
            const auto width = toolbar.find(sawer::UiAction::width_cycle)->bounds;
            const sawer::Vec2d width_center{width.x + width.width * 0.5, width.y + width.height * 0.5};
            toolbar.pointer_down(width_center);
            toolbar.close_properties();
            layout();
            REQUIRE_FALSE(toolbar.pointer_up(width_center));
            REQUIRE(toolbar.properties_bounds() == expanded);
            REQUIRE(toolbar.animating());
            auto previous = expanded;
            for (int frame = 0; frame < 6; ++frame) {
                toolbar.tick(0.03);
                const auto surface = toolbar.properties_surface_bounds();
                REQUIRE(surface.width <= previous.width);
                REQUIRE(surface.height <= previous.height);
                const auto toggle = toolbar.find(sawer::UiAction::properties_menu)->bounds;
                const sawer::Vec2d center{toggle.x + toggle.width * 0.5, toggle.y + toggle.height * 0.5};
                REQUIRE(surface.contains(center));
                REQUIRE(toolbar.contains(center));
                REQUIRE(toolbar.action_at(center) == sawer::UiAction::properties_menu);
                if (const auto* field = toolbar.find(sawer::UiAction::width_cycle)) {
                    const sawer::Vec2d point{field->bounds.x + field->bounds.width * 0.5,
                        field->bounds.y + field->bounds.height * 0.5};
                    REQUIRE(toolbar.action_at(point) != sawer::UiAction::width_cycle);
                }
                REQUIRE(toolbar.panels()[1U].bounds == tools);
                previous = surface;
            }
            toolbar.tick(0.03);
            REQUIRE(toolbar.properties_reveal() == 0.0);
            REQUIRE(toolbar.find(sawer::UiAction::width_cycle) == nullptr);
            REQUIRE(toolbar.properties_bounds().width == 0.0);
            const auto collapsed = toolbar.properties_surface_bounds();
            toolbar.clear_pointer();
            toolbar.clear_focus();
            for (int frame = 0; frame < 10; ++frame) toolbar.tick(0.1);
            REQUIRE_FALSE(toolbar.animating());
            toolbar.toggle_properties();
            layout();
            REQUIRE(toolbar.properties_surface_bounds() == collapsed);
            REQUIRE(toolbar.properties_reveal() == 0.0);
            for (int frame = 0; frame < 7; ++frame) toolbar.tick(0.03);
            REQUIRE(toolbar.properties_reveal() == 1.0);
            REQUIRE(toolbar.properties_bounds() == expanded);
            REQUIRE(toolbar.properties_scroll() == saved_scroll);
            REQUIRE(toolbar.find(sawer::UiAction::width_cycle) != nullptr);
            REQUIRE(toolbar.action_at(width_center) == sawer::UiAction::width_cycle);
        }
    }
}

TEST_CASE("style animation reverses continuously and survives relayout")
{
    sawer::Toolbar toolbar;
    const auto layout = [&](const double viewport_width, const double dpi) {
        toolbar.update(viewport_width, 720.0, dpi, sawer::Tool::rectangle, {}, false,
            false, false, 1.0, "Board", false, {}, sawer::BackgroundStyle::dot, {});
    };
    layout(1280.0, 1.0);
    toolbar.close_properties();
    layout(1280.0, 1.0);
    toolbar.tick(0.05);
    const auto reversing_bounds = toolbar.properties_surface_bounds();
    const auto reversing_reveal = toolbar.properties_reveal();
    toolbar.toggle_properties();
    layout(1280.0, 1.0);
    REQUIRE(toolbar.properties_surface_bounds() == reversing_bounds);
    REQUIRE(toolbar.properties_reveal() == reversing_reveal);
    toolbar.tick(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(toolbar.properties_surface_bounds() == reversing_bounds);
    toolbar.tick(0.04);
    REQUIRE(toolbar.properties_surface_bounds().width > reversing_bounds.width);
    layout(480.0, 1.5);
    REQUIRE(toolbar.properties_surface_bounds().x >= 0.0);
    REQUIRE(toolbar.properties_surface_bounds().x + toolbar.properties_surface_bounds().width <= 480.0);
    const auto toggle = toolbar.find(sawer::UiAction::properties_menu)->bounds;
    REQUIRE(toolbar.action_at({toggle.x + toggle.width * 0.5, toggle.y + toggle.height * 0.5})
        == sawer::UiAction::properties_menu);
    for (int frame = 0; frame < 10; ++frame) toolbar.tick(0.1);
    REQUIRE(toolbar.properties_reveal() == 1.0);
    REQUIRE_FALSE(toolbar.animating());
}

TEST_CASE("canvas settings align palettes and keep every choice reachable")
{
    using sawer::UiAction;
    for (const auto size : std::array<sawer::Vec2d, 5>{{
             {320.0, 480.0}, {480.0, 420.0}, {760.0, 480.0},
             {1280.0, 720.0}, {1920.0, 1080.0}}}) {
        for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
            sawer::Toolbar toolbar;
            toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
            toolbar.update(size.x, size.y, dpi, sawer::Tool::pencil,
                {}, false, false, false, 1.0, "Board", false, {},
                sawer::BackgroundStyle::dot, {240U, 242U, 247U, 255U});
            const auto panel = toolbar.panels()[toolbar.panels().size() - 2U].bounds;
            REQUIRE(panel.x >= 0.0);
            REQUIRE(panel.y >= 0.0);
            REQUIRE(panel.x + panel.width <= size.x);
            REQUIRE(panel.y + panel.height <= size.y);
            std::size_t patterns = 0U;
            std::size_t selected_patterns = 0U;
            for (const auto& control : toolbar.controls()) {
                if (!toolbar.is_settings_control(control.action)) continue;
                const auto bounds = control.bounds;
                REQUIRE(bounds.width >= 32.0);
                REQUIRE(bounds.height >= 32.0);
                REQUIRE(bounds.x >= panel.x);
                REQUIRE(bounds.y >= panel.y);
                REQUIRE(bounds.x + bounds.width <= panel.x + panel.width + 0.01);
                REQUIRE(bounds.y + bounds.height <= panel.y + panel.height + 0.01);
                const sawer::Vec2d center{bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5};
                REQUIRE(toolbar.contains(center));
                REQUIRE(toolbar.action_at(center) == control.action);
                toolbar.pointer_down(center);
                REQUIRE(toolbar.pointer_up(center) == control.action);
                for (const auto& other : toolbar.controls()) {
                    if (other.action == control.action || !toolbar.is_settings_control(other.action)) continue;
                    REQUIRE_FALSE((bounds.x < other.bounds.x + other.bounds.width
                        && bounds.x + bounds.width > other.bounds.x
                        && bounds.y < other.bounds.y + other.bounds.height
                        && bounds.y + bounds.height > other.bounds.y));
                }
                if (control.action >= UiAction::grid_solid && control.action <= UiAction::grid_narrow_rule) {
                    ++patterns;
                    selected_patterns += control.selected ? 1U : 0U;
                    REQUIRE_FALSE(control.label.empty());
                    REQUIRE_FALSE(control.tooltip.empty());
                }
            }
            REQUIRE(patterns == 9U);
            REQUIRE(selected_patterns == 1U);
            REQUIRE(toolbar.find(UiAction::grid_solid)->label == "None");
            REQUIRE(toolbar.find(UiAction::grid_dot)->label == "Dots");
            REQUIRE(toolbar.find(UiAction::grid_graph)->bounds.y
                > toolbar.find(UiAction::grid_solid)->bounds.y);
            for (int index = 0; index < 10; ++index) {
                const auto background = toolbar.find(static_cast<UiAction>(static_cast<int>(UiAction::bg_color_0) + index));
                const auto grid = toolbar.find(static_cast<UiAction>(static_cast<int>(UiAction::grid_color_0) + index));
                REQUIRE(background->bounds.x == grid->bounds.x);
                REQUIRE(background->tooltip == grid->tooltip);
            }
            REQUIRE(toolbar.find(UiAction::bg_color_6)->bounds.y
                < toolbar.find(UiAction::bg_color_0)->bounds.y);
        }
    }
}

TEST_CASE("canvas palette reordering preserves color actions and selected state")
{
    using sawer::UiAction;
    sawer::Toolbar toolbar;
    toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
    const sawer::Color cream{245U, 232U, 150U, 255U};
    const sawer::Color white{255U, 255U, 255U, 255U};
    REQUIRE(sawer::Toolbar::background_color_for(UiAction::bg_color_0) == cream);
    REQUIRE(sawer::Toolbar::grid_color_for(UiAction::grid_color_6) == white);
    toolbar.update(1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, "Board", false, {}, sawer::BackgroundStyle::triangle, cream, white);
    REQUIRE(toolbar.find(UiAction::bg_color_0)->selected);
    REQUIRE_FALSE(toolbar.find(UiAction::edit_background_custom)->selected);
    REQUIRE(toolbar.find(UiAction::grid_color_6)->selected);
    REQUIRE_FALSE(toolbar.find(UiAction::grid_color_auto)->selected);
    REQUIRE_FALSE(toolbar.find(UiAction::edit_grid_custom)->selected);
    REQUIRE(toolbar.find(UiAction::grid_triangle)->selected);
    toolbar.update(1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false,
        1.0, "Board", false, {}, sawer::BackgroundStyle::solid, {10U, 20U, 30U, 255U});
    REQUIRE(toolbar.find(UiAction::edit_background_custom)->selected);
    REQUIRE(toolbar.find(UiAction::edit_background_custom)->accent == sawer::Color{10U, 20U, 30U, 255U});
    REQUIRE(toolbar.find(UiAction::grid_color_auto)->selected);
    REQUIRE(toolbar.find(UiAction::grid_solid)->selected);
}

TEST_CASE("canvas settings keep a deliberate gap from navigation at every supported size")
{
    using sawer::UiAction;
    for (const auto size : std::array<sawer::Vec2d, 6>{{
             {320.0, 480.0}, {480.0, 420.0}, {760.0, 480.0},
             {1280.0, 720.0}, {1920.0, 1080.0}, {3840.0, 2160.0}}}) {
        for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
            CAPTURE(size.x, size.y, dpi);
            sawer::Toolbar toolbar;
            toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
            toolbar.update(size.x, size.y, dpi, sawer::Tool::pencil, {}, false,
                false, false, 0.72, "Board", false, {}, sawer::BackgroundStyle::dot, {});
            const auto popup = toolbar.panels()[toolbar.panels().size() - 2U].bounds;
            const auto zoom = toolbar.find(UiAction::zoom_menu)->bounds;
            const auto zoom_left = toolbar.find(UiAction::zoom_out)->bounds;
            const auto zoom_right = toolbar.find(UiAction::zoom_in)->bounds;
            const double scale = toolbar.scale();
            REQUIRE(popup.x >= 0.0);
            REQUIRE(popup.y >= 16.0 * scale);
            REQUIRE(popup.x + popup.width <= size.x);
            REQUIRE(popup.y + popup.height <= size.y);
            const bool above = popup.y + popup.height + 12.0 * scale
                <= zoom.y - 8.0 * scale + 0.01;
            const bool beside = popup.x + popup.width + 12.0 * scale
                <= zoom_left.x - 8.0 * scale + 0.01;
            REQUIRE((above || beside));
            if (above) {
                REQUIRE(popup.x + popup.width
                    == Catch::Approx(zoom_right.x + zoom_right.width + 8.0 * scale));
            }
            const sawer::Vec2d zoom_center{zoom.x + zoom.width * 0.5, zoom.y + zoom.height * 0.5};
            REQUIRE(toolbar.action_at(zoom_center) == UiAction::zoom_menu);
            toolbar.pointer_down(zoom_center);
            REQUIRE(toolbar.pointer_up(zoom_center) == UiAction::zoom_menu);
            const auto close = toolbar.find(UiAction::settings_close)->bounds;
            REQUIRE(popup.contains({close.x, close.y}));
            REQUIRE(popup.contains({close.x + close.width, close.y + close.height}));
        }
    }
}
