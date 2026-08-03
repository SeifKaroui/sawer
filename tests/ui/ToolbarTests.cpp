#include "ui/Toolbar.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    toolbar.update(
        3840.0, 2160.0, 2.0, sawer::Tool::line, {}, false, false, false, 2.0,
        "board.sawer", false, {}, sawer::BackgroundStyle::square, {});

    REQUIRE(toolbar.scale() == 1.5);
    REQUIRE(toolbar.zoom() == 2.0);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_thin) == 2.5);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_regular) == 4.0);
    REQUIRE(sawer::Toolbar::width_for(sawer::UiAction::width_bold) == 7.0);
    const auto* const regular_width =
        toolbar.find(sawer::UiAction::width_regular);
    REQUIRE(regular_width != nullptr);
    REQUIRE(regular_width->selected);
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
    REQUIRE(toolbar.find(sawer::UiAction::new_board)->icon
        == sawer::UiIcon::file_new);
    REQUIRE(toolbar.find(sawer::UiAction::open_board)->icon
        == sawer::UiIcon::folder_open);
    REQUIRE(toolbar.find(sawer::UiAction::save_as)->label == "Save...");
    REQUIRE(toolbar.find(sawer::UiAction::undo)->icon
        == sawer::UiIcon::undo);
    REQUIRE(toolbar.find(sawer::UiAction::format_background)->icon
        == sawer::UiIcon::background);
    REQUIRE(toolbar.find(sawer::UiAction::toggle_theme)->icon
        == sawer::UiIcon::theme);
    REQUIRE(toolbar.find(sawer::UiAction::format_background)->icon
        != toolbar.find(sawer::UiAction::toggle_theme)->icon);
    const auto app_bar = toolbar.panels().front().bounds;
    REQUIRE(app_bar.x == 0.0);
    REQUIRE(app_bar.y == 0.0);
    REQUIRE(app_bar.width == 1280.0);
    REQUIRE(app_bar.height == toolbar.height());
    REQUIRE(toolbar.height() == 56.0);

    const auto* const home = toolbar.find(sawer::UiAction::go_home);
    const auto* const select = toolbar.find(sawer::UiAction::select);
    const auto* const hand = toolbar.find(sawer::UiAction::hand);
    REQUIRE(home->bounds.width == 36.0);
    REQUIRE(select->bounds.width == 36.0);
    REQUIRE(toolbar.panels()[1U].bounds.width == 48.0);
    REQUIRE(home->bounds.y < toolbar.height());
    REQUIRE(select->bounds.y > toolbar.height());
    REQUIRE(select->bounds.x == hand->bounds.x);
    REQUIRE(select->bounds.y < hand->bounds.y);
    const auto tool_panel = toolbar.panels()[1U].bounds;
    REQUIRE(
        tool_panel.y + tool_panel.height * 0.5
        == Catch::Approx(
            toolbar.height()
            + (toolbar.viewport_height() - toolbar.height()) * 0.5));
    REQUIRE(toolbar.find(sawer::UiAction::format_background)->bounds.y
        > toolbar.viewport_height() * 0.5);

    const auto* const rename = toolbar.find(sawer::UiAction::rename_board);
    REQUIRE(rename != nullptr);
    REQUIRE(rename->icon == sawer::UiIcon::pencil);
    REQUIRE(toolbar.filename_bounds().contains({
        rename->bounds.x + rename->bounds.width * 0.5,
        rename->bounds.y + rename->bounds.height * 0.5,
    }));
    REQUIRE(toolbar.filename_bounds().y < toolbar.height());
    REQUIRE(toolbar.panels().back().bounds.width < 180.0);

    toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "a-longer-board-name.sawer", false, {},
        sawer::BackgroundStyle::dot, {});
    REQUIRE(toolbar.settings_open());
    REQUIRE(toolbar.settings_page() == sawer::SettingsPage::canvas);
    REQUIRE(toolbar.find(sawer::UiAction::format_background)->selected);
    REQUIRE(toolbar.find(sawer::UiAction::grid_dot)->selected);
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
    REQUIRE(toolbar.filename_bounds().width > completed_width);
    REQUIRE(toolbar.find(sawer::UiAction::rename_board)->bounds.x
        > completed_icon_x);
    REQUIRE(toolbar.filename_bounds().width <= 620.0);
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

TEST_CASE("drawing settings expose compact stabilization presets")
{
    sawer::Toolbar toolbar;
    sawer::DrawingSettings settings;
    settings.stabilization = sawer::StrokeStabilization::strong;

    toolbar.update(
        1280.0, 720.0, 1.0, sawer::Tool::pencil, {}, false, false, false, 1.0,
        "Untitled", false, {}, sawer::BackgroundStyle::dot, {},
        std::nullopt, settings);

    const auto* const off =
        toolbar.find(sawer::UiAction::stabilization_off);
    const auto* const light =
        toolbar.find(sawer::UiAction::stabilization_light);
    const auto* const default_level =
        toolbar.find(sawer::UiAction::stabilization_default);
    const auto* const strong =
        toolbar.find(sawer::UiAction::stabilization_strong);
    REQUIRE(off != nullptr);
    REQUIRE(light != nullptr);
    REQUIRE(default_level != nullptr);
    REQUIRE(strong != nullptr);
    REQUIRE(strong->selected);
    REQUIRE(off->label == "Off");
    REQUIRE(light->label == "Light");
    REQUIRE(default_level->label == "Medium");
    REQUIRE(strong->label == "Strong");
    REQUIRE(light->bounds.y == Catch::Approx(off->bounds.y));
    REQUIRE(default_level->bounds.y > off->bounds.y);
    REQUIRE(strong->bounds.y == Catch::Approx(default_level->bounds.y));
    REQUIRE(default_level->bounds.x == Catch::Approx(off->bounds.x));
    REQUIRE(strong->bounds.x == Catch::Approx(light->bounds.x));
    REQUIRE(light->bounds.x > off->bounds.x);
    REQUIRE(off->tooltip.empty());
    REQUIRE(light->tooltip.empty());
    REQUIRE(default_level->tooltip.empty());
    REQUIRE(strong->tooltip.empty());

    toolbar.toggle_settings_panel(sawer::SettingsPage::canvas);
    REQUIRE(toolbar.settings_open());
    REQUIRE(toolbar.settings_page() == sawer::SettingsPage::canvas);
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

    REQUIRE(toolbar.find(sawer::UiAction::format_background)->selected);
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
    REQUIRE(background_custom->bounds.width == background_custom->bounds.height);
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
    REQUIRE(toolbar.find(sawer::UiAction::stabilization_light) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::stabilization_default) != nullptr);

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
    REQUIRE(toolbar.find(sawer::UiAction::save) != nullptr);
    REQUIRE(toolbar.find(sawer::UiAction::save_as)->label == "Save as...");
}

TEST_CASE("inline palette aligns with the contextual rail")
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
    REQUIRE(
        context_panel.x - (tool_panel.x + tool_panel.width)
        == Catch::Approx(8.0));
    REQUIRE(last_color->bounds.x >= first_color->bounds.x);
    REQUIRE(last_color->bounds.y > first_color->bounds.y);
    REQUIRE(context_panel.width > tool_panel.width);
    REQUIRE(
        context_panel.y + context_panel.height * 0.5
        == Catch::Approx(
            pencil->bounds.y + pencil->bounds.height * 0.5));
    const std::size_t panel_count_before_color_editor =
        toolbar.panels().size();

    for (const sawer::UiAction action : {
             sawer::UiAction::width_thin,
             sawer::UiAction::width_heavy,
             sawer::UiAction::stabilization_default,
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
    REQUIRE(toolbar.settings_scrim_visible());
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
        editor.x + editor.width * 0.5
        == Catch::Approx(toolbar.viewport_width() * 0.5));
    REQUIRE(
        editor.y + editor.height * 0.5
        == Catch::Approx(toolbar.viewport_height() * 0.5));
    const auto old_pencil_action = toolbar.action_at({
        old_pencil_bounds.x + old_pencil_bounds.width * 0.5,
        old_pencil_bounds.y + old_pencil_bounds.height * 0.5,
    });
    REQUIRE((
        !old_pencil_action.has_value()
        || *old_pencil_action != sawer::UiAction::pencil));
}

TEST_CASE(
    "custom stroke color editor keeps background chrome visible and inert")
{
    sawer::Toolbar toolbar;
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

    REQUIRE(toolbar.settings_scrim_visible());
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
    REQUIRE((
        !old_palette_action.has_value()
        || *old_palette_action != sawer::UiAction::color_black));
    const auto old_width_action = toolbar.action_at({
        old_width.x + old_width.width * 0.5,
        old_width.y + old_width.height * 0.5,
    });
    REQUIRE((
        !old_width_action.has_value()
        || *old_width_action != sawer::UiAction::width_thin));
    toolbar.set_pointer({
        old_width.x + old_width.width * 0.5,
        old_width.y + old_width.height * 0.5,
    });
    const auto* const hovered = toolbar.hovered_control();
    REQUIRE((
        hovered == nullptr
        || hovered->action != sawer::UiAction::width_thin));
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
    REQUIRE(context_panel.x > tool_panel.x + tool_panel.width);
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
