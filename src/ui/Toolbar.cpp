#include "ui/Toolbar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace sawer {
namespace {

constexpr std::array<Color, 7> palette{{
    {235U, 240U, 248U, 255U},
    {239U, 92U, 92U, 255U},
    {244U, 180U, 64U, 255U},
    {73U, 190U, 126U, 255U},
    {82U, 145U, 244U, 255U},
    {167U, 112U, 239U, 255U},
    {30U, 34U, 42U, 255U},
}};

constexpr std::array<double, 4> widths{{3.0, 5.0, 9.0, 18.0}};
constexpr std::array<std::string_view, 7> color_tooltips{{
    "White  1", "Red  2", "Amber  3", "Green  4",
    "Blue  5", "Violet  6", "Black  7",
}};
constexpr std::array<std::string_view, 4> width_tooltips{{
    "Fine - 3 px", "Regular - 5 px", "Bold - 9 px", "Heavy - 18 px",
}};
constexpr std::array<double, 4> roundness_presets{{0.0, 0.12, 0.25, 0.5}};

constexpr std::array<std::string_view, 4> roundness_tooltips{{
    "Square corners - 0%",
    "Soft corners - 12%",
    "Rounded corners - 25%",
    "Fully rounded corners - 50%",
}};

constexpr std::array<std::string_view, 4> mixed_roundness_tooltips{{
    "Mixed corners - set square (0%)",
    "Mixed corners - set soft (12%)",
    "Mixed corners - set rounded (25%)",
    "Mixed corners - set fully rounded (50%)",
}};

// Canvas background and grid swatches: pastel tints, then a neutral row ending
// in near-black.
constexpr std::array<Color, 10> background_palette{{
    {245U, 232U, 150U, 255U},
    {245U, 205U, 178U, 255U},
    {242U, 205U, 212U, 255U},
    {215U, 205U, 240U, 255U},
    {198U, 228U, 242U, 255U},
    {210U, 235U, 200U, 255U},
    {255U, 255U, 255U, 255U},
    {235U, 238U, 242U, 255U},
    {205U, 210U, 216U, 255U},
    {32U, 36U, 44U, 255U},
}};

constexpr std::array<UiAction, static_cast<std::size_t>(BackgroundStyle::count)>
    grid_actions{{
        UiAction::grid_solid,
        UiAction::grid_dot,
        UiAction::grid_square,
        UiAction::grid_graph,
        UiAction::grid_hybrid,
        UiAction::grid_diamond,
        UiAction::grid_wide_rule,
        UiAction::grid_triangle,
        UiAction::grid_narrow_rule,
    }};

constexpr std::array<UiIcon, static_cast<std::size_t>(BackgroundStyle::count)>
    grid_icons{{
        UiIcon::grid_solid,
        UiIcon::grid_dot,
        UiIcon::grid_square,
        UiIcon::grid_graph,
        UiIcon::grid_hybrid,
        UiIcon::grid_diamond,
        UiIcon::grid_wide_rule,
        UiIcon::grid_triangle,
        UiIcon::grid_narrow_rule,
    }};

constexpr std::array<std::string_view,
    static_cast<std::size_t>(BackgroundStyle::count)>
    grid_tooltips{{
        "No pattern",
        "Dots",
        "Square grid",
        "Graph paper",
        "Ruled dots",
        "Diamond grid",
        "Wide ruled lines",
        "Triangle grid",
        "Narrow ruled lines",
    }};

// Keep both canvas palettes in the same visual order without changing the
// action-to-color mapping used by persisted settings.
constexpr std::array<std::size_t, 10> canvas_color_order{{6U, 7U, 8U, 9U, 0U, 1U, 2U, 3U, 4U, 5U}};
constexpr std::array<std::string_view, 10> canvas_color_names{{
    "Cream", "Peach", "Rose", "Lavender", "Sky", "Mint",
    "White", "Light gray", "Gray", "Graphite",
}};
constexpr std::array<std::string_view, 9> grid_labels{{
    "None", "Dots", "Squares", "Graph", "Mixed", "Diamond", "Ruled", "Triangles", "Lines",
}};

struct Hsv final {
    double hue{};
    double saturation{};
    double value{};
};

Hsv to_hsv(const Color color) noexcept
{
    const double red = static_cast<double>(color.red) / 255.0;
    const double green = static_cast<double>(color.green) / 255.0;
    const double blue = static_cast<double>(color.blue) / 255.0;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;
    double hue = 0.0;
    if (delta > 1.0e-9) {
        if (maximum == red) {
            hue = std::fmod((green - blue) / delta, 6.0) / 6.0;
        } else if (maximum == green) {
            hue = ((blue - red) / delta + 2.0) / 6.0;
        } else {
            hue = ((red - green) / delta + 4.0) / 6.0;
        }
        if (hue < 0.0) hue += 1.0;
    }
    return {
        hue,
        maximum <= 1.0e-9 ? 0.0 : delta / maximum,
        maximum,
    };
}

Color from_hsv(const double hue, const double saturation, const double value) noexcept
{
    const double wrapped = hue - std::floor(hue);
    const double scaled = wrapped * 6.0;
    const int sector = static_cast<int>(std::floor(scaled)) % 6;
    const double fraction = scaled - std::floor(scaled);
    const double p = value * (1.0 - saturation);
    const double q = value * (1.0 - fraction * saturation);
    const double t = value * (1.0 - (1.0 - fraction) * saturation);
    double red = value;
    double green = t;
    double blue = p;
    switch (sector) {
    case 1: red = q; green = value; blue = p; break;
    case 2: red = p; green = value; blue = t; break;
    case 3: red = p; green = q; blue = value; break;
    case 4: red = t; green = p; blue = value; break;
    case 5: red = value; green = p; blue = q; break;
    default: break;
    }
    const auto channel = [](const double component) {
        return static_cast<std::uint8_t>(std::lround(
            std::clamp(component, 0.0, 1.0) * 255.0));
    };
    return {channel(red), channel(green), channel(blue), 255U};
}

bool same_rgb(const Color first, const Color second) noexcept
{
    return first.red == second.red && first.green == second.green
        && first.blue == second.blue;
}

struct ContextCapabilities final {
    bool palette{};
    bool width{};
    bool fill{};
    bool color_target{};
    bool roundness{};
    bool selection_actions{};
};

ContextCapabilities context_capabilities(
    const Tool tool,
    const bool has_selection,
    const SelectionStyleSummary& selection) noexcept
{
    switch (tool) {
    case Tool::pencil:
        return {
            .palette = true,
            .width = true,
        };
    case Tool::line:
        return {
            .palette = true,
            .width = true,
        };
    case Tool::rectangle:
        return {
            .palette = true,
            .width = true,
            .fill = true,
            .color_target = true,
            .roundness = true,
        };
    case Tool::ellipse:
        return {
            .palette = true,
            .width = true,
            .fill = true,
            .color_target = true,
        };
    case Tool::select:
        if (!has_selection) {
            return {};
        }
        return {
            .palette =
                selection.stroke_color != PropertyValueState::unavailable
                || selection.fill_color != PropertyValueState::unavailable,
            .width =
                selection.stroke_width != PropertyValueState::unavailable,
            .fill = selection.fill != PropertyValueState::unavailable,
            .color_target =
                selection.fill != PropertyValueState::unavailable,
            .roundness =
                selection.roundness != PropertyValueState::unavailable,
            .selection_actions = true,
        };
    case Tool::hand:
        return {};
    }
    return {};
}

} // namespace

void Toolbar::update(
    const double viewport_width,
    const double viewport_height,
    const double display_scale,
    const Tool current_tool,
    const Style& style,
    const bool can_undo,
    const bool can_redo,
    const bool has_selection,
    const double zoom,
    std::string filename,
    const bool dirty,
    std::string error_message,
    const BackgroundStyle background_style,
    const Color background_color,
    const std::optional<Color> grid_color,
    const DrawingSettings&,
    const bool filename_editing,
    const std::size_t filename_cursor,
    const std::size_t filename_anchor,
    const bool has_file,
    const bool selection_supports_fill,
    const double rectangle_roundness,
    const bool selection_supports_roundness,
    const bool rectangle_roundness_mixed,
    const SelectionStyleSummary& selection_style,
    const StyleColorTarget color_target,
    const bool stroke_width_editing,
    const std::string_view stroke_width_edit_text,
    std::string status_message,
    const bool loading)
{
    viewport_width_ = std::max(viewport_width, 1.0);
    viewport_height_ = std::max(viewport_height, 1.0);
    const double available_scale = std::max(
        0.82,
        std::min(viewport_width_ / 760.0, viewport_height_ / 470.0));
    scale_ = std::min(std::clamp(display_scale, 0.82, 1.5), available_scale);
    const bool single_header_row = viewport_width_ >= 1040.0 * scale_;
    height_ = (single_header_row ? 72.0 : 56.0) * scale_;
    compact_properties_ = viewport_width_ < 900.0 * scale_
        || viewport_height_ < 560.0 * scale_;
    if (properties_tool_ != current_tool) {
        properties_scroll_ = 0.0;
        properties_tool_ = current_tool;
    }
    zoom_ = zoom;
    filename_ = std::move(filename);
    dirty_ = dirty;
    loading_ = loading;
    has_file_ = has_file;
    filename_editing_ = filename_editing;
    has_selection_ = has_selection;
    filename_cursor_ = std::min(filename_cursor, filename_.size());
    filename_anchor_ = std::min(filename_anchor, filename_.size());
    error_message_ = std::move(error_message);
    status_message_ = std::move(status_message);
    document_status_ = loading ? "Opening..."
        : !error_message_.empty() ? "Needs attention"
        : (!has_file ? "Not saved" : (dirty ? "Autosave pending" : "Saved"));
    background_style_ = background_style;
    background_color_ = background_color;
    grid_color_ = grid_color;

    const bool selection_context =
        current_tool == Tool::select && has_selection;
    SelectionStyleSummary effective_selection = selection_style;
    // Keep direct/legacy Toolbar callers useful while the application supplies
    // the full summary below. Runtime selection UI never falls back to a
    // misleading first-object value.
    if (selection_context
        && effective_selection.stroke_color
            == PropertyValueState::unavailable) {
        effective_selection.stroke_color = PropertyValueState::uniform;
        effective_selection.stroke_color_value = style.stroke;
        effective_selection.stroke_width = PropertyValueState::uniform;
        effective_selection.stroke_width_value = style.stroke_width;
        if (selection_supports_fill) {
            effective_selection.fill = PropertyValueState::uniform;
            effective_selection.fill_enabled = style.fill.has_value();
            if (style.fill.has_value()) {
                effective_selection.fill_color = PropertyValueState::uniform;
                effective_selection.fill_color_value = *style.fill;
            }
        }
        if (selection_supports_roundness) {
            effective_selection.roundness = rectangle_roundness_mixed
                ? PropertyValueState::mixed
                : PropertyValueState::uniform;
            effective_selection.roundness_value = rectangle_roundness;
        }
    }
    const ContextCapabilities capabilities = context_capabilities(
        current_tool, has_selection, effective_selection);
    inline_color_palette_ = capabilities.palette;
    const bool closed_drawing_tool = current_tool == Tool::rectangle
        || current_tool == Tool::ellipse;
    StyleColorTarget effective_color_target = color_target;
    if (!capabilities.color_target
        || (effective_color_target == StyleColorTarget::fill
            && (selection_context
                    ? effective_selection.fill
                        == PropertyValueState::unavailable
                    : !closed_drawing_tool))) {
        effective_color_target = StyleColorTarget::stroke;
    }

    controls_.clear();
    controls_.reserve(64U);
    panels_.clear();
    panels_.reserve(8U);
    dividers_.clear();
    dividers_.reserve(4U);
    error_bounds_ = {};
    status_bounds_ = {};
    properties_bounds_ = {};
    properties_panel_index_.reset();
    properties_expanded_bounds_ = {};
    properties_content_offset_ = 0.0;
    custom_color_preview_bounds_ = {};
    properties_clip_ = {};
    property_begin_ = property_end_ = 0U;
    properties_scroll_limit_ = 0.0;
    settings_bounds_ = {};
    current_color_mixed_ = selection_context
        && (effective_color_target == StyleColorTarget::fill
                ? effective_selection.fill == PropertyValueState::mixed
                    || effective_selection.fill_color
                        == PropertyValueState::mixed
                : effective_selection.stroke_color
                    == PropertyValueState::mixed);
    if (selection_context) {
        current_color_ = effective_color_target == StyleColorTarget::fill
            && effective_selection.fill_color
                != PropertyValueState::unavailable
            ? effective_selection.fill_color_value
            : effective_selection.stroke_color_value;
    } else {
        current_color_ = effective_color_target == StyleColorTarget::fill
            ? style.fill.value_or(style.stroke)
            : style.stroke;
    }
    context_stroke_width_mixed_ = selection_context
        && effective_selection.stroke_width == PropertyValueState::mixed;
    context_stroke_width_ = selection_context
        ? effective_selection.stroke_width_value
        : style.stroke_width;
    stroke_width_editing_ = stroke_width_editing;
    if (stroke_width_editing_) {
        stroke_width_label_ = stroke_width_edit_text.empty()
            ? "\xE2\x80\x94 px"
            : std::string{stroke_width_edit_text} + " px";
    } else if (context_stroke_width_mixed_) {
        stroke_width_label_ = "Mixed";
    } else {
        std::array<char, 24> width_label{};
        const double rounded = std::round(context_stroke_width_);
        if (std::abs(context_stroke_width_ - rounded) < 0.001) {
            static_cast<void>(std::snprintf(
                width_label.data(),
                width_label.size(),
                "%.0f px",
                rounded));
        } else {
            static_cast<void>(std::snprintf(
                width_label.data(),
                width_label.size(),
                "%.1f px",
                context_stroke_width_));
        }
        stroke_width_label_ = width_label.data();
    }

    const double button = std::max(40.0 * scale_, 32.0);
    const double gap = 8.0 * scale_;
    const double panel_pad = 8.0 * scale_;
    double x = 0.0;
    double y = 0.0;

    const auto add_at = [&](const UiAction action,
                            const UiRect bounds,
                            const UiIcon icon,
                            const std::string_view label,
                            const std::string_view tooltip,
                            const bool enabled,
                            const bool selected,
                            const std::optional<Color> accent = std::nullopt,
                            const bool partial = false) {
        controls_.push_back(UiControl{
            .action = action,
            .bounds = bounds,
            .label = label,
            .tooltip = tooltip,
            .icon = icon,
            .enabled = enabled && !loading,
            .selected = selected,
            .accent = accent,
            .partial = partial,
        });
    };
    const auto add = [&](const UiAction action,
                         const double width,
                         const UiIcon icon,
                         const std::string_view label,
                         const std::string_view tooltip,
                         const bool enabled,
                         const bool selected,
                         const std::optional<Color> accent = std::nullopt) {
        add_at(
            action, {x, y, width, button}, icon, label, tooltip,
            enabled, selected, accent);
        x += width + gap;
    };
    const auto finish_panel = [&](const double start_x) {
        panels_.push_back({{
            start_x - panel_pad,
            y - panel_pad,
            x - start_x + panel_pad * 2.0 - gap,
            button + panel_pad * 2.0,
        }});
    };

    // Header bounds reserve file and utility controls without painting a full-width strip.
    panels_.push_back({{0.0, 0.0, viewport_width_, height_}});
    x = 16.0 * scale_;
    y = (height_ - button) * 0.5;
    add(
        UiAction::go_home, button, UiIcon::home, "",
        "Home - recent boards", true, false);
    add(UiAction::file_menu, std::max(64.0 * scale_, 56.0),
        UiIcon::chevron_down, "File", "New, Open, Rename, and Save as",
        true, settings_open_ && settings_page_ == SettingsPage::file);
    const double filename_x = x + 8.0 * scale_;

    const double header_actions_width = button * 2.0 + gap;
    x = viewport_width_ - 16.0 * scale_ - header_actions_width;
    const double utility_x = x;
    add(UiAction::toggle_theme, button,
        theme_ == Theme::light ? UiIcon::moon : UiIcon::theme, "",
        theme_ == Theme::light ? "Switch to dark theme  T" : "Switch to light theme  T",
        true, false);
    add(UiAction::preferences_menu, button, UiIcon::settings, "",
        "Board settings and appearance", true,
        settings_open_ && settings_page_ == SettingsPage::preferences);

    // Keep the drawing tools centered and stable while their options change.
    const std::size_t tool_count = 8U;
    const double tool_panel_width = static_cast<double>(tool_count) * button
        + static_cast<double>(tool_count - 1U) * gap
        + 24.0 * scale_ + panel_pad * 2.0;
    const double tool_panel_x = (viewport_width_ - tool_panel_width) * 0.5;
    const double tool_panel_y = single_header_row ? 8.0 * scale_ : height_ + 12.0 * scale_;
    const double tool_panel_height = button + panel_pad * 2.0;
    double tool_x = tool_panel_x + panel_pad;
    const double tool_y = tool_panel_y + panel_pad;
    const auto add_tool = [&](const UiAction action,
                              const UiIcon icon,
                              const std::string_view tooltip,
                              const bool selected,
                              const bool enabled = true) {
        add_at(
            action, {tool_x, tool_y, button, button}, icon, "",
            tooltip, enabled, selected);
        tool_x += button + gap;
    };
    add_tool(
        UiAction::select, UiIcon::cursor, "Select and edit  V",
        current_tool == Tool::select);
    add_tool(
        UiAction::hand, UiIcon::hand,
        "Hand / Pan  H - hold Space or drag with middle mouse",
        current_tool == Tool::hand);
    tool_x += 12.0 * scale_;
    add_tool(
        UiAction::pencil, UiIcon::pencil, "Pencil  P",
        current_tool == Tool::pencil);
    add_tool(
        UiAction::line, UiIcon::line, "Line  L",
        current_tool == Tool::line);
    add_tool(
        UiAction::rectangle, UiIcon::rectangle, "Rectangle  R",
        current_tool == Tool::rectangle);
    add_tool(
        UiAction::ellipse, UiIcon::ellipse, "Ellipse  E",
        current_tool == Tool::ellipse);
    tool_x += 12.0 * scale_;
    add_tool(UiAction::undo, UiIcon::undo, "Undo  Ctrl+Z", false, can_undo);
    add_tool(UiAction::redo, UiIcon::redo, "Redo  Ctrl+Shift+Z", false, can_redo);
    panels_.push_back({{
        tool_panel_x,
        tool_panel_y,
        tool_panel_width,
        tool_panel_height,
    }});

    // Capabilities determine the options instead of exposing generic controls
    // that the active tool or selection cannot use.
    const bool width_mixed = selection_context
        && effective_selection.stroke_width == PropertyValueState::mixed;
    const double displayed_width = selection_context
        ? effective_selection.stroke_width_value
        : style.stroke_width;
    const bool fill_mixed = selection_context
        && effective_selection.fill == PropertyValueState::mixed;
    const bool no_fill_selected = selection_context
        ? effective_selection.fill == PropertyValueState::uniform
            && !effective_selection.fill_enabled
        : !style.fill.has_value();
    const bool fill_partial = selection_context
        && effective_selection.fill_partial;
    const bool roundness_mixed = selection_context
        && effective_selection.roundness == PropertyValueState::mixed;
    const double displayed_roundness = selection_context
        ? effective_selection.roundness_value
        : rectangle_roundness;
    const bool roundness_partial = selection_context
        && effective_selection.roundness_partial;
    const double compact_button = std::max(32.0 * scale_, 32.0);
    const double color_button = std::max(40.0 * scale_, 32.0);
    const double context_label_height = 24.0 * scale_;
    const double minimum_color_target_width =
        std::max(68.0 * scale_, 60.0);
    // Reserve the same readable Stroke / Fill / No fill row for every tool,
    // including when responsive scaling reaches its minimum.
    const double color_target_content_width = minimum_color_target_width * 2.0
        + std::max(60.0 * scale_, 60.0) + gap * 2.0;
    const double context_content_width = std::max({
        224.0 * scale_,
        compact_button * 4.0 + gap * 3.0,
        color_target_content_width,
    });
    const auto stacked_height = [gap](
                                    const std::initializer_list<double> rows) {
        double result = 0.0;
        std::size_t count = 0U;
        for (const double row : rows) {
            if (row <= 0.0) continue;
            result += row;
            ++count;
        }
        return result + (count > 1U
            ? static_cast<double>(count - 1U) * gap
            : 0.0);
    };
    const double appearance_height = stacked_height({
        inline_color_palette_
            ? context_label_height + color_button * 2.0 + gap
            : 0.0,
        capabilities.palette ? button : 0.0,
        capabilities.width
            ? context_label_height + compact_button + gap + button
            : 0.0,
        capabilities.roundness
            ? context_label_height + compact_button
            : 0.0,
    });
    const double action_height =
        capabilities.selection_actions
            ? compact_button + button + gap + 12.0 * scale_ : 0.0;
    const std::size_t group_count =
        static_cast<std::size_t>(appearance_height > 0.0)
        + static_cast<std::size_t>(action_height > 0.0);
    const double context_top = tool_panel_y + tool_panel_height + 16.0 * scale_;
    const double footer_reserve = 64.0 * scale_;
    const double available_height = std::max(96.0,
        viewport_height_ - context_top - footer_reserve);
    const double context_panel_y = context_top
        + (available_height - std::min(440.0 * scale_, available_height)) * 0.5;
    const double panel_header_height = button + gap;
    properties_tab_bounds_ = {16.0 * scale_, context_panel_y,
        std::max(104.0 * scale_, 96.0), button + 16.0 * scale_};
    properties_tab_toggle_ = {properties_tab_bounds_.x + 8.0 * scale_,
        properties_tab_bounds_.y + 8.0 * scale_,
        properties_tab_bounds_.width - 16.0 * scale_, button};
    if (group_count == 0U) {
        properties_reveal_ = properties_open_ ? 1.0 : 0.0;
        properties_animation_elapsed_ = 0.18;
    }
    if (group_count > 0U && !properties_open_ && properties_reveal_ == 0.0) {
        const UiRect tab{16.0 * scale_, context_panel_y,
            std::max(104.0 * scale_, 96.0), button + 16.0 * scale_};
        properties_panel_index_ = panels_.size();
        panels_.push_back({tab});
        add_at(UiAction::properties_menu,
            {tab.x + 8.0 * scale_, tab.y + 8.0 * scale_,
                tab.width - 16.0 * scale_, button},
            UiIcon::chevron_right, "Style", "Expand style panel", true, false);
    }
    if (group_count > 0U && (properties_open_ || properties_reveal_ > 0.0)) {
        const double context_panel_x = 16.0 * scale_;
        const double section_gap = 24.0 * scale_;
        const double context_horizontal_pad = 12.0 * scale_;
        const double context_content_height =
            appearance_height
            + action_height
            + static_cast<double>(group_count - 1U) * section_gap
            + context_horizontal_pad * 2.0 + panel_header_height;
        const double context_panel_height = std::min(context_content_height,
            available_height);
        // A fixed anchor keeps color and width targets still when sections change.
        const double visible_panel_height = std::min(context_panel_height,
            viewport_height_ - footer_reserve - context_panel_y);
        const double preset_width =
            (context_content_width - gap * 3.0) * 0.25;
        const double width_value_width = std::max(42.0 * scale_, 40.0);
        const double minimum_stepper_width =
            button * 2.0 + width_value_width + gap * 2.0;
        const double content_width = std::max(
            context_content_width, minimum_stepper_width);
        const double context_x = context_panel_x + context_horizontal_pad;
        const double width_value_actual =
            content_width - button * 2.0 - gap * 2.0;
        properties_bounds_ = {context_panel_x, context_panel_y,
            content_width + context_horizontal_pad * 2.0, visible_panel_height};
        properties_clip_ = {context_panel_x + context_horizontal_pad,
            context_panel_y + context_horizontal_pad + panel_header_height, content_width,
            visible_panel_height - context_horizontal_pad * 2.0 - panel_header_height};
        properties_expanded_bounds_ = properties_bounds_;
        properties_expanded_clip_ = properties_clip_;
        properties_scroll_limit_ = std::max(0.0,
            context_content_height - visible_panel_height);
        properties_scroll_ = std::clamp(properties_scroll_, 0.0,
            properties_scroll_limit_);
        // The collapse control stays outside the scrolling content.
        add_at(UiAction::properties_menu,
            {context_x + content_width - button,
                context_panel_y + context_horizontal_pad, button, button},
            properties_open_ ? UiIcon::chevron_left : UiIcon::chevron_right,
            properties_open_ ? "" : "Style",
            properties_open_ ? "Collapse style panel" : "Expand style panel", true, false);
        properties_expanded_toggle_ = controls_.back().bounds;
        property_begin_ = controls_.size();
        double context_y = properties_clip_.y - properties_scroll_;
        bool appearance_started = false;

        const auto add_section_divider = [&] {
            const double divider_y = context_y + section_gap * 0.5;
            dividers_.push_back({
                {context_x + 5.0 * scale_, divider_y},
                {
                    context_x + content_width - 5.0 * scale_,
                    divider_y,
                },
            });
            context_y += section_gap;
        };
        const auto begin_appearance_block = [&] {
            if (appearance_started) {
                context_y += gap;
            }
            appearance_started = true;
        };

        // The color target belongs immediately above its palette.
        if (capabilities.palette) {
            begin_appearance_block();
            const std::optional<Color> stroke_accent = selection_context
                ? (effective_selection.stroke_color
                            == PropertyValueState::uniform
                    ? std::optional<Color>{
                        effective_selection.stroke_color_value}
                    : std::nullopt)
                : std::optional<Color>{style.stroke};
            const std::optional<Color> fill_accent = selection_context
                ? (effective_selection.fill_color
                            == PropertyValueState::uniform
                    ? std::optional<Color>{
                        effective_selection.fill_color_value}
                        : std::nullopt)
                : style.fill;
            const double no_fill_width = std::max(60.0 * scale_, 60.0);
            const double target_width = capabilities.color_target
                ? (content_width - no_fill_width - gap * 2.0) * 0.5 : content_width;
            add_at(
                UiAction::color_target_stroke,
                {context_x, context_y, target_width, button},
                UiIcon::none,
                "Stroke",
                "Choose the stroke color",
                true,
                effective_color_target == StyleColorTarget::stroke,
                stroke_accent);
            if (capabilities.color_target) add_at(
                UiAction::color_target_fill,
                {
                    context_x + target_width + gap,
                    context_y,
                    target_width,
                    button,
                },
                UiIcon::none,
                "Fill",
                fill_partial
                    ? "Fill color - applies to shapes only"
                    : "Choose the fill color",
                capabilities.fill,
                effective_color_target == StyleColorTarget::fill,
                fill_accent,
                fill_partial);
            if (capabilities.color_target) add_at(UiAction::fill_none,
                {context_x + (target_width + gap) * 2.0, context_y, no_fill_width, button},
                UiIcon::none, "No fill", fill_mixed ? "Remove mixed fills" : "Remove fill from shapes",
                capabilities.fill, no_fill_selected,
                std::nullopt, fill_partial);
            context_y += button;
        }

        if (inline_color_palette_) {
            begin_appearance_block();
            context_y += context_label_height;
            const bool color_value_present =
                effective_color_target == StyleColorTarget::stroke
                || (selection_context
                        ? effective_selection.fill_color
                            != PropertyValueState::unavailable
                        : style.fill.has_value());
            constexpr std::array color_actions{
                UiAction::color_white,
                UiAction::color_red,
                UiAction::color_amber,
                UiAction::color_green,
                UiAction::color_blue,
                UiAction::color_violet,
                UiAction::color_black,
            };
            for (std::size_t index = 0U;
                 index < color_actions.size(); ++index) {
                const std::size_t row = index / 4U;
                const std::size_t column = index % 4U;
                add_at(
                    color_actions[index],
                    {
                        context_x
                            + static_cast<double>(column)
                                * (preset_width + gap),
                        context_y
                            + static_cast<double>(row)
                                * (color_button + gap),
                        preset_width,
                        color_button,
                    },
                    UiIcon::none,
                    "",
                    color_tooltips[index],
                    true,
                    color_value_present
                        && !current_color_mixed_
                        && same_rgb(current_color_, palette[index]),
                    palette[index]);
            }
            const CustomColorTarget custom_target =
                effective_color_target == StyleColorTarget::fill
                ? CustomColorTarget::fill
                : CustomColorTarget::stroke;
            const bool custom_color_selected = color_value_present
                && !current_color_mixed_
                && std::ranges::none_of(
                    palette, [&](const Color preset) {
                        return same_rgb(current_color_, preset);
                    });
            add_at(
                UiAction::edit_stroke_custom,
                {
                    context_x + 3.0 * (preset_width + gap),
                    context_y + color_button + gap,
                    preset_width,
                    color_button,
                },
                UiIcon::custom_color,
                "",
                effective_color_target == StyleColorTarget::fill
                    ? "Custom fill color"
                    : "Custom stroke color",
                true,
                custom_color_selected
                    || (settings_open_
                        && settings_page_ == SettingsPage::color_editor
                        && custom_color_target_ == custom_target),
                current_color_);
            context_y += color_button * 2.0 + gap;
        }

        if (capabilities.width) {
            begin_appearance_block();
            context_y += context_label_height;
            constexpr std::array width_actions{
                UiAction::width_thin,
                UiAction::width_regular,
                UiAction::width_bold,
                UiAction::width_heavy,
            };
            for (std::size_t index = 0U;
                 index < width_actions.size(); ++index) {
                add_at(
                    width_actions[index],
                    {
                        context_x
                            + static_cast<double>(index)
                                * (preset_width + gap),
                        context_y,
                        preset_width,
                        compact_button,
                    },
                    UiIcon::line,
                    "",
                    width_tooltips[index],
                    true,
                    !stroke_width_editing_
                        && !width_mixed
                        && std::abs(
                            displayed_width - widths[index]) < 0.001);
            }
            context_y += compact_button + gap;
            add_at(
                UiAction::width_decrease,
                {context_x, context_y, button, button},
                UiIcon::minus,
                " ",
                "Decrease width  [ - hold Shift for smaller steps",
                width_mixed || displayed_width > 0.5 + 0.001,
                false);
            add_at(
                UiAction::width_cycle,
                {
                    context_x + button + gap,
                    context_y,
                    width_value_actual,
                    button,
                },
                UiIcon::none,
                stroke_width_label_,
                "Click to enter a stroke width",
                true,
                stroke_width_editing_);
            add_at(
                UiAction::width_increase,
                {
                    context_x + button + gap
                        + width_value_actual + gap,
                    context_y,
                    button,
                    button,
                },
                UiIcon::plus,
                " ",
                "Increase width  ] - hold Shift for smaller steps",
                width_mixed || displayed_width < 64.0 - 0.001,
                false);
            context_y += button;
        }

        if (capabilities.roundness) {
            begin_appearance_block();
            context_y += context_label_height;
            constexpr std::array roundness_actions{
                UiAction::roundness_square,
                UiAction::roundness_soft,
                UiAction::roundness_round,
                UiAction::roundness_full,
            };
            for (std::size_t index = 0;
                 index < roundness_actions.size(); ++index) {
                add_at(
                    roundness_actions[index],
                    {
                        context_x
                            + static_cast<double>(index)
                                * (preset_width + gap),
                        context_y,
                        preset_width,
                        compact_button,
                    },
                    UiIcon::rectangle,
                    "",
                    roundness_mixed
                        ? mixed_roundness_tooltips[index]
                        : roundness_tooltips[index],
                    true,
                    !roundness_mixed
                        && std::abs(
                            displayed_roundness
                            - roundness_presets[index]) < 0.001,
                    std::nullopt,
                    roundness_partial);
            }
            context_y += compact_button;
        }

        if (appearance_height > 0.0 && action_height > 0.0) {
            add_section_divider();
        }

        if (capabilities.selection_actions) {
            const double action_width =
                (context_content_width - gap * 2.0) / 3.0;
            add_at(
                UiAction::copy_selection,
                {context_x, context_y, action_width, compact_button},
                UiIcon::copy,
                "",
                "Copy selection  Ctrl+C",
                true,
                false);
            add_at(
                UiAction::cut_selection,
                {context_x + action_width + gap, context_y,
                 action_width, compact_button},
                UiIcon::cut,
                "",
                "Cut selection  Ctrl+X",
                true,
                false);
            add_at(
                UiAction::paste,
                {context_x + (action_width + gap) * 2.0, context_y,
                 action_width, compact_button},
                UiIcon::paste,
                "",
                "Paste  Ctrl+V",
                true,
                false);
            context_y += compact_button + gap;
            context_y += 12.0 * scale_;
            const double selection_button_width =
                (content_width - 16.0 * scale_) * 0.5;
            add_at(
                UiAction::duplicate,
                {context_x, context_y, selection_button_width, button},
                UiIcon::duplicate,
                "",
                "Duplicate selection  Ctrl+D",
                true,
                false);
            add_at(
                UiAction::delete_selection,
                {context_x + selection_button_width + 16.0 * scale_,
                 context_y, selection_button_width, button},
                UiIcon::trash,
                "",
                "Delete selection  Delete",
                true,
                false);
        }
        property_end_ = controls_.size();
        properties_panel_index_ = panels_.size();
        panels_.push_back({properties_bounds_});
        apply_properties_animation();
    }
    properties_laid_out_ |= group_count > 0U;

    // Zoom has its own quiet group. Appearance belongs in the header settings.
    const double bottom_y = viewport_height_ - button - 16.0 * scale_;
    const double zoom_width = std::max(60.0 * scale_, 52.0);
    x = viewport_width_ - 16.0 * scale_
        - (button * 2.0 + zoom_width + gap * 2.0);
    y = bottom_y;
    const double zoom_start = x;
    add(UiAction::zoom_out, button, UiIcon::zoom_out, "", "Zoom out  Ctrl+-", true, false);
    add(
        UiAction::zoom_menu,
        zoom_width,
        UiIcon::zoom_reset,
        "100%",
        "Zoom and framing",
        true,
        settings_open_ && settings_page_ == SettingsPage::view);
    add(UiAction::zoom_in, button, UiIcon::zoom_in, "", "Zoom in  Ctrl++", true, false);
    finish_panel(zoom_start);

    if (!error_message_.empty() || !status_message_.empty()) {
        UiRect message_bounds{
            12.0 * scale_,
            std::max(
                height_ + 6.0 * scale_,
                bottom_y),
            std::min(
                420.0 * scale_,
                std::max(zoom_start - panel_pad - 24.0 * scale_, 1.0)),
            button,
        };
        if (!error_message_.empty()) {
            error_bounds_ = message_bounds;
        } else {
            status_bounds_ = message_bounds;
        }
        panels_.push_back({message_bounds});
    }

    if (settings_open_) {
        build_settings_panel();
    }

    // The document title sits inside the application bar. It is pushed last so
    // filename_bounds() and the renderer retain their stable lookup.
    const double available_filename_width = std::max(
        64.0,
        (single_header_row ? tool_panel_x : utility_x) - filename_x - 16.0 * scale_);
    // Reserve stable document space so names, editing, and autosave feedback
    // cannot move the surrounding controls. File retains Rename on narrow bars.
    const double document_width = std::min(220.0 * scale_, available_filename_width);
    const bool show_rename_button = document_width >= 160.0 * scale_;
    const double rename_gap = 4.0 * scale_;
    const UiRect status_bounds{
        filename_x,
        (height_ - button) * 0.5,
        document_width - (show_rename_button ? button + rename_gap : 0.0),
        button,
    };
    if (show_rename_button) {
        add_at(UiAction::rename_button,
            {status_bounds.x + status_bounds.width + rename_gap,
                status_bounds.y, button, button},
            UiIcon::rename, {}, "Rename board  F2", !filename_editing_, false);
    }
    controls_.push_back(UiControl{
        .action = UiAction::rename_board,
        .bounds = status_bounds,
        .label = {},
        .tooltip = filename_editing_ ? std::string{} : filename_,
        .icon = UiIcon::none,
        .enabled = !loading_,
        .selected = false,
        .accent = std::nullopt,
    });
    panels_.push_back({status_bounds});

    if (focused_action_.has_value()) {
        const UiControl* const focused = find(*focused_action_);
        if (focused == nullptr || !focused->enabled
            || (settings_open_
                && !is_active_settings_action(focused->action))) {
            focused_action_.reset();
            focus_from_keyboard_ = false;
        }
    }
}

void Toolbar::build_settings_panel()
{
    const double pad = 16.0 * scale_;
    const double header = 30.0 * scale_;
    const double gap = 8.0 * scale_;
    const bool about_page = settings_page_ == SettingsPage::about;
    const bool view_page = settings_page_ == SettingsPage::view;
    const bool canvas_page = settings_page_ == SettingsPage::canvas;
    const double view_row_height = std::max(36.0 * scale_, 32.0);
    const bool header_menu = settings_page_ == SettingsPage::file
        || settings_page_ == SettingsPage::preferences;
    const double preferred_panel_width =
        (view_page ? 280.0 : about_page ? 720.0
            : (settings_page_ == SettingsPage::color_editor ? 320.0 : (header_menu ? 288.0 : 320.0))) * scale_;
    const double panel_width = std::min(
        preferred_panel_width,
        std::max(viewport_width_ - 24.0 * scale_, 1.0));
    const UiControl* const zoom_anchor = find(UiAction::zoom_menu);
    const double navigation_clearance = 20.0 * scale_;
    const double canvas_room = zoom_anchor == nullptr
        ? viewport_height_ - 24.0 * scale_
        : zoom_anchor->bounds.y - navigation_clearance - 16.0 * scale_;
    const bool compact_canvas = canvas_room < 510.0 * scale_;
    const double canvas_swatch = std::max((compact_canvas ? 32.0 : 40.0) * scale_, 32.0);
    const double canvas_gap = (compact_canvas ? 6.0 : 8.0) * scale_;
    const double canvas_palette_height = canvas_swatch * 2.0 + canvas_gap;
    const double canvas_section_gap = (compact_canvas ? 28.0 : 36.0) * scale_;
    const double canvas_first_row = (compact_canvas ? 66.0 : 74.0) * scale_;
    const double canvas_pattern_height = std::max((compact_canvas ? 48.0 : 52.0) * scale_, 32.0);
    const double canvas_height = canvas_first_row
        + (canvas_palette_height + canvas_section_gap) * 2.0
        + canvas_pattern_height * 3.0 + canvas_gap * 2.0 + pad;
    const double desired_panel_height = view_page
        ? pad * 2.0 + header + 18.0 * scale_ + view_row_height * 3.0 + gap * 2.0
        : settings_page_ == SettingsPage::canvas
        ? canvas_height
        : (settings_page_ == SettingsPage::color_editor
            ? 440.0 * scale_
            : (about_page ? 620.0 * scale_
                : (header_menu ? (settings_page_ == SettingsPage::preferences ? 132.0 : 220.0) : 240.0) * scale_));
    const double panel_height = std::min(
        desired_panel_height,
        std::max(viewport_height_ - 24.0 * scale_, 1.0));
    const bool style_color_editor =
        settings_page_ == SettingsPage::color_editor
        && (custom_color_target_ == CustomColorTarget::stroke
            || custom_color_target_ == CustomColorTarget::fill);
    const double rightmost_panel_x = std::max(
        16.0 * scale_,
        viewport_width_ - panel_width - ((view_page || canvas_page) ? 8.0 : 12.0) * scale_);
    const UiControl* const color_trigger = find(UiAction::edit_stroke_custom);
    const UiRect color_anchor = color_trigger == nullptr
        ? properties_bounds_ : color_trigger->bounds;
    const UiControl* const file_trigger = find(UiAction::file_menu);
    double panel_x = settings_page_ == SettingsPage::file
        ? std::clamp(file_trigger != nullptr ? file_trigger->bounds.x : 16.0 * scale_,
            12.0 * scale_, rightmost_panel_x)
        : (style_color_editor
            ? std::clamp(properties_bounds_.x + properties_bounds_.width
                    + 12.0 * scale_, 12.0 * scale_, rightmost_panel_x)
            : (about_page
        ? std::clamp(
            (viewport_width_ - panel_width) * 0.5,
            12.0 * scale_,
            rightmost_panel_x)
        : rightmost_panel_x));
    const double above_zoom_y = zoom_anchor == nullptr ? 16.0 * scale_
        : zoom_anchor->bounds.y - navigation_clearance - panel_height;
    // A tall canvas picker can fit beside navigation in a short window.
    // Keep its complete controls visible rather than covering the zoom bar.
    if (canvas_page && above_zoom_y < 16.0 * scale_) {
        if (const UiControl* zoom_left = find(UiAction::zoom_out)) {
            const double beside_zoom_x = zoom_left->bounds.x
                - navigation_clearance - panel_width;
            if (beside_zoom_x >= 16.0 * scale_) panel_x = beside_zoom_x;
        }
    }
    const double lowest_panel_y = std::max(
        16.0 * scale_,
        ((view_page || canvas_page) && zoom_anchor != nullptr
            ? zoom_anchor->bounds.y - navigation_clearance
            : viewport_height_ - 60.0 * scale_) - panel_height);
    const double panel_y = header_menu
        ? height_ + 8.0 * scale_
        : (style_color_editor
            ? std::clamp(color_anchor.y, height_ + 8.0 * scale_,
                std::max(height_ + 8.0 * scale_,
                    viewport_height_ - panel_height - 12.0 * scale_))
            : (about_page
        ? std::clamp(
            (viewport_height_ - panel_height) * 0.5,
            12.0 * scale_,
            std::max(12.0 * scale_,
                viewport_height_ - panel_height - 12.0 * scale_))
        : lowest_panel_y));

    const double close_size = std::max(header * 0.9, 32.0);
    controls_.push_back(UiControl{
        .action = UiAction::settings_close,
        .bounds = {
            panel_x + panel_width - pad - close_size,
            panel_y + pad * 0.5,
            close_size,
            close_size,
        },
        .label = {},
        .tooltip = settings_page_ == SettingsPage::color_editor
            ? "Cancel color change"
            : "Close",
        .icon = UiIcon::close,
        .enabled = !loading_,
        .selected = false,
        .accent = std::nullopt,
    });

    const auto add_control = [&](const UiAction action,
                                 const UiRect bounds,
                                 const std::string_view label,
                                 const std::string_view tooltip,
                                 const bool selected,
                                 const UiIcon icon = UiIcon::none,
                                 const std::optional<Color> accent = std::nullopt,
                                 const bool enabled = true) {
        controls_.push_back(UiControl{
            .action = action,
            .bounds = bounds,
            .label = label,
            .tooltip = tooltip,
            .icon = icon,
            .enabled = enabled && !loading_,
            .selected = selected,
            .accent = accent,
        });
    };

    if (header_menu) {
        const double field_x = panel_x + pad;
        const double field_width = panel_width - pad * 2.0;
        const double row_height = std::max(36.0 * scale_, 32.0);
        double row_y = panel_y + 44.0 * scale_;
        const auto row = [&](const UiAction action, const std::string_view label,
                             const std::string_view tooltip, const UiIcon icon) {
            add_control(action, {field_x, row_y, field_width, row_height},
                label, tooltip, false, icon);
            row_y += row_height + gap;
        };
        if (settings_page_ == SettingsPage::file) {
            row(UiAction::new_board, "New board", "New board  Ctrl+N", UiIcon::file_new);
            row(UiAction::open_board, "Open board...", "Open board  Ctrl+O", UiIcon::folder_open);
            row(UiAction::rename_file, "Rename...", "Rename board  F2", UiIcon::rename);
            row(has_file_ ? UiAction::save_copy : UiAction::save_as,
                has_file_ ? "Save as..." : "Save...",
                has_file_ ? "Save in another location  Ctrl+Shift+S"
                    : "Choose where to save this board  Ctrl+S", UiIcon::save);
        } else {
            row(UiAction::format_background, "Canvas background",
                "Background color, grid color, and pattern", UiIcon::background);
            row(UiAction::about, "About Sawer", "Version and licenses", UiIcon::info);
        }
    } else if (settings_page_ == SettingsPage::canvas) {
        constexpr std::array background_actions{
            UiAction::bg_color_0, UiAction::bg_color_1, UiAction::bg_color_2,
            UiAction::bg_color_3, UiAction::bg_color_4, UiAction::bg_color_5,
            UiAction::bg_color_6, UiAction::bg_color_7, UiAction::bg_color_8,
            UiAction::bg_color_9,
        };
        constexpr std::array grid_color_actions{
            UiAction::grid_color_0, UiAction::grid_color_1,
            UiAction::grid_color_2, UiAction::grid_color_3,
            UiAction::grid_color_4, UiAction::grid_color_5,
            UiAction::grid_color_6, UiAction::grid_color_7,
            UiAction::grid_color_8, UiAction::grid_color_9,
        };
        const double swatch = canvas_swatch;
        const double field_width = panel_width - pad * 2.0;
        const double stride = (field_width - swatch) / 5.0;
        const double row_x = panel_x + pad;
        double row_y = panel_y + canvas_first_row;
        const auto swatch_bounds = [&](const std::size_t slot) {
            return UiRect{row_x + static_cast<double>(slot % 6U) * stride,
                row_y + static_cast<double>(slot / 6U) * (swatch + canvas_gap),
                swatch, swatch};
        };
        for (std::size_t position = 0U; position < canvas_color_order.size(); ++position) {
            const std::size_t index = canvas_color_order[position];
            // Four neutrals on the first row, six pastels on the second.
            const std::size_t slot = position < 4U ? position : position + 2U;
            add_control(background_actions[index], swatch_bounds(slot),
                {}, canvas_color_names[index],
                same_rgb(background_color_, background_palette[index]),
                UiIcon::none, background_palette[index]);
        }
        UiRect custom_background = swatch_bounds(4U);
        custom_background.width += stride;
        add_control(UiAction::edit_background_custom, custom_background,
            "Custom", "Custom background color",
            std::ranges::none_of(background_palette, [&](const Color color) {
                return same_rgb(background_color_, color);
            }), UiIcon::custom_color, background_color_);

        row_y += canvas_palette_height + canvas_section_gap;
        for (std::size_t position = 0U; position < canvas_color_order.size(); ++position) {
            const std::size_t index = canvas_color_order[position];
            const std::size_t slot = position < 4U ? position : position + 2U;
            add_control(grid_color_actions[index], swatch_bounds(slot),
                {}, canvas_color_names[index],
                grid_color_.has_value() && same_rgb(*grid_color_, background_palette[index]),
                UiIcon::none, background_palette[index]);
        }
        add_control(UiAction::edit_grid_custom, swatch_bounds(4U),
            {}, "Custom grid color", grid_color_.has_value()
                && std::ranges::none_of(background_palette, [&](const Color color) {
                    return same_rgb(*grid_color_, color);
                }), UiIcon::custom_color, grid_color_.value_or(background_color_));
        add_control(UiAction::grid_color_auto, swatch_bounds(5U),
            "Auto", "Automatically contrast the grid with the background", !grid_color_.has_value());

        row_y += canvas_palette_height + canvas_section_gap;
        const double tile_width = (field_width - canvas_gap * 2.0) / 3.0;
        for (std::size_t index = 0U; index < grid_actions.size(); ++index) {
            add_control(grid_actions[index],
                {row_x + static_cast<double>(index % 3U) * (tile_width + canvas_gap),
                 row_y + static_cast<double>(index / 3U) * (canvas_pattern_height + canvas_gap),
                 tile_width, canvas_pattern_height},
                grid_labels[index], grid_tooltips[index],
                static_cast<std::size_t>(background_style_) == index, grid_icons[index]);
        }
    } else if (settings_page_ == SettingsPage::view) {
        const double field_x = panel_x + pad;
        const double field_width = panel_width - pad * 2.0;
        const double row_height = view_row_height;
        double row_y = panel_y + pad + header + 18.0 * scale_;
        add_control(
            UiAction::zoom_reset,
            {field_x, row_y, field_width, row_height},
            "Reset to 100%", "Reset zoom to 100%", false);
        row_y += row_height + gap;
        add_control(
            UiAction::zoom_fit_content,
            {field_x, row_y, field_width, row_height},
            "Fit content", "Frame every object", false);
        row_y += row_height + gap;
        add_control(
            UiAction::zoom_fit_selection,
            {field_x, row_y, field_width, row_height},
            "Fit selection", "Frame selected objects", false,
            UiIcon::none, std::nullopt, has_selection_);
    } else if (settings_page_ == SettingsPage::color_editor) {
        const double field_x = panel_x + pad;
        const double field_width = panel_width - pad * 2.0;
        const double button_height = std::max(40.0 * scale_, 32.0);
        const double preview_height = std::max(44.0 * scale_, 40.0);
        const double hue_height = std::max(32.0 * scale_, 32.0);
        const double done_y = panel_y + panel_height - pad - button_height;
        const double recent_y = done_y - 40.0 * scale_;
        const double preview_y = recent_y - 8.0 * scale_ - preview_height;
        const double hue_y = preview_y - 12.0 * scale_ - hue_height;
        const double sv_y = panel_y + 80.0 * scale_;
        const double sv_height = hue_y - 32.0 * scale_ - sv_y;
        add_control(
            UiAction::custom_hue_field,
            {field_x, hue_y, field_width, hue_height},
            {}, "Hue", false);
        add_control(
            UiAction::custom_sv_field,
            {field_x, sv_y, field_width, std::max(32.0, sv_height)},
            {}, "Saturation and brightness", false);
        custom_color_preview_bounds_ = {field_x, preview_y, field_width, preview_height};
        add_control(UiAction::custom_hex_field,
            {field_x + preview_height + 8.0 * scale_, preview_y,
                field_width - preview_height - 8.0 * scale_, preview_height},
            {}, "Enter a HEX color, such as #2563EB", hex_editing_);
        for (std::size_t index = 0U; index < recent_color_count_; ++index) {
            add_control(static_cast<UiAction>(static_cast<int>(UiAction::recent_color_0) + static_cast<int>(index)),
                {field_x + static_cast<double>(index) * (32.0 + 8.0) * scale_, recent_y,
                    std::max(32.0 * scale_, 32.0), std::max(32.0 * scale_, 32.0)},
                {}, "Use a recent custom color", same_rgb(custom_color(), recent_colors_[index]),
                UiIcon::none, recent_colors_[index]);
        }
        const double footer_width = (field_width - 8.0 * scale_) * 0.5;
        add_control(UiAction::custom_color_cancel,
            {field_x, done_y, footer_width, button_height}, "Cancel", "Cancel this color change", false);
        add_control(
            UiAction::custom_color_done,
            {field_x + footer_width + 8.0 * scale_, done_y, footer_width, button_height},
            "Apply", "Use this color", hex_valid_, UiIcon::none, std::nullopt, hex_valid_);
    }

    settings_bounds_ = {panel_x, panel_y, panel_width, panel_height};
    panels_.push_back({settings_bounds_});
}

void Toolbar::set_pointer(const Vec2d point) noexcept
{
    pointer_ = point;
    const UiControl* hovered = hovered_control();
    const auto action = hovered == nullptr ? std::optional<UiAction>{} : std::optional<UiAction>{hovered->action};
    if (action != tooltip_action_) {
        tooltip_action_ = action;
        tooltip_elapsed_ = 0.0;
        tooltip_dismissed_ = false;
    }
}

void Toolbar::clear_pointer() noexcept
{
    pointer_.reset();
    tooltip_action_.reset();
    tooltip_elapsed_ = 0.0;
    tooltip_dismissed_ = false;
    pressed_action_.reset();
    repeated_action_.reset();
    press_hold_time_ = 0.0;
    press_repeat_accumulator_ = 0.0;
}

void Toolbar::pointer_down(const Vec2d point) noexcept
{
    pointer_ = point;
    tooltip_dismissed_ = true;
    pressed_action_ = action_at(point);
    focus_from_keyboard_ = false;
    repeated_action_.reset();
    press_hold_time_ = 0.0;
    press_repeat_accumulator_ = 0.0;
    if (pressed_action_.has_value()) {
        focused_action_ = pressed_action_;
    }
}

std::optional<UiAction> Toolbar::pointer_up(const Vec2d point) noexcept
{
    pointer_ = point;
    const std::optional<UiAction> released_action = action_at(point);
    const std::optional<UiAction> activated =
        pressed_action_.has_value() && released_action == pressed_action_
        ? pressed_action_
        : std::nullopt;
    pressed_action_.reset();
    repeated_action_.reset();
    press_hold_time_ = 0.0;
    press_repeat_accumulator_ = 0.0;
    return activated;
}

std::optional<UiAction> Toolbar::take_repeated_action() noexcept
{
    const std::optional<UiAction> action = repeated_action_;
    repeated_action_.reset();
    return action;
}

void Toolbar::focus_next(const bool reverse) noexcept
{
    tooltip_dismissed_ = false;
    std::vector<UiAction> focusable;
    focusable.reserve(controls_.size());
    for (const auto& control : controls_) {
        if (control.enabled
            && (!settings_open_
                || is_active_settings_action(control.action))
            && (!is_property_control(control.action) || property_controls_active())
            && control.action != UiAction::custom_hue_field
            && control.action != UiAction::custom_sv_field) {
            focusable.push_back(control.action);
        }
    }
    if (focusable.empty()) {
        focused_action_.reset();
        focus_from_keyboard_ = false;
        return;
    }
    focus_from_keyboard_ = true;
    const auto current = focused_action_.has_value()
        ? std::ranges::find(focusable, *focused_action_)
        : focusable.end();
    if (current == focusable.end()) {
        focused_action_ = reverse ? focusable.back() : focusable.front();
        return;
    }
    const std::size_t index =
        static_cast<std::size_t>(current - focusable.begin());
    focused_action_ = reverse
        ? focusable[(index + focusable.size() - 1U) % focusable.size()]
        : focusable[(index + 1U) % focusable.size()];
    if (is_property_control(*focused_action_)) {
        const UiRect bounds = find(*focused_action_)->bounds;
        if (bounds.y < properties_clip_.y) {
            set_properties_scroll(properties_scroll_ + bounds.y - properties_clip_.y);
        } else if (bounds.y + bounds.height
                > properties_clip_.y + properties_clip_.height) {
            set_properties_scroll(properties_scroll_ + bounds.y + bounds.height
                - properties_clip_.y - properties_clip_.height);
        }
    }
}

void Toolbar::clear_focus() noexcept
{
    focused_action_.reset();
    focus_from_keyboard_ = false;
}

void Toolbar::tick(const double elapsed_seconds) noexcept
{
    const double elapsed = std::isfinite(elapsed_seconds)
        ? std::clamp(elapsed_seconds, 0.0, 0.1) : 0.0;
    if (properties_reveal_ != (properties_open_ ? 1.0 : 0.0)) {
        properties_animation_elapsed_ = std::min(0.18,
            properties_animation_elapsed_ + elapsed);
        const double remaining = 1.0 - properties_animation_elapsed_ / 0.18;
        const double eased = 1.0 - remaining * remaining * remaining;
        const double target = properties_open_ ? 1.0 : 0.0;
        properties_reveal_ = properties_animation_elapsed_ >= 0.18 ? target
            : properties_animation_start_ + (target - properties_animation_start_) * eased;
        apply_properties_animation();
    }
    const UiControl* const hovered = hovered_control();
    const auto action = hovered == nullptr ? std::optional<UiAction>{}
        : std::optional<UiAction>{hovered->action};
    if (action != tooltip_action_) {
        tooltip_action_ = action;
        tooltip_elapsed_ = 0.0;
        tooltip_dismissed_ = false;
    }
    if (hovered != nullptr && !tooltip_dismissed_) tooltip_elapsed_ = std::min(0.45, tooltip_elapsed_ + elapsed);
    const auto approach = [elapsed](double& value, const double target, const double speed) {
        const double blend = 1.0 - std::exp(-speed * elapsed);
        value += (target - value) * blend;
        if (std::abs(value - target) < 0.001) value = target;
    };
    const bool repeatable = pressed_action_ == UiAction::width_decrease
        || pressed_action_ == UiAction::width_increase;
    if (repeatable && hovered != nullptr && hovered->enabled
        && pressed_action_ == hovered->action) {
        press_hold_time_ += elapsed;
        if (press_hold_time_ >= 0.45) {
            press_repeat_accumulator_ += elapsed;
            const double acceleration = std::clamp(
                (press_hold_time_ - 0.45) / 1.8, 0.0, 1.0);
            const double repeat_interval =
                0.11 + (0.035 - 0.11) * acceleration;
            if (press_repeat_accumulator_ >= repeat_interval) {
                repeated_action_ = pressed_action_;
                press_repeat_accumulator_ = std::fmod(
                    press_repeat_accumulator_, repeat_interval);
            }
        }
    } else {
        press_hold_time_ = 0.0;
        press_repeat_accumulator_ = 0.0;
        repeated_action_.reset();
    }
    approach(reveal_, 1.0, 7.0);
    approach(settings_reveal_, settings_open_ ? 1.0 : 0.0, 16.0);
    theme_transition_ = std::min(
        1.0,
        theme_transition_ + elapsed / 0.24);
    for (const auto& control : controls_) {
        auto& state = animations_[static_cast<std::size_t>(control.action)];
        approach(state.hover, hovered == &control && control.enabled ? 1.0 : 0.0, 16.0);
        approach(
            state.press,
            pressed_action_ == control.action && hovered == &control ? 1.0 : 0.0,
            pressed_action_ == control.action ? 24.0 : 18.0);
        approach(state.selected, control.selected ? 1.0 : 0.0, 14.0);
    }
}

bool Toolbar::animating() const noexcept
{
    if (reveal_ < 1.0 || theme_transition_ < 1.0
        || settings_reveal_ != (settings_open_ ? 1.0 : 0.0)
        || properties_reveal_ != (properties_open_ ? 1.0 : 0.0)
        || pressed_action_.has_value()) {
        return true;
    }
    const UiControl* const hovered = hovered_control();
    if (hovered != nullptr && !tooltip_dismissed_ && tooltip_elapsed_ < 0.45) return true;
    for (const auto& control : controls_) {
        const UiAnimation& state =
            animations_[static_cast<std::size_t>(control.action)];
        const double hover_target =
            hovered == &control && control.enabled ? 1.0 : 0.0;
        const double press_target =
            pressed_action_ == control.action && hovered == &control
            ? 1.0
            : 0.0;
        const double selected_target = control.selected ? 1.0 : 0.0;
        if (state.hover != hover_target
            || state.press != press_target
            || state.selected != selected_target) {
            return true;
        }
    }
    return false;
}

void Toolbar::play_entrance() noexcept
{
    reveal_ = 0.0;
}

void Toolbar::toggle_theme() noexcept
{
    previous_theme_ = theme_;
    theme_ = theme_ == Theme::dark ? Theme::light : Theme::dark;
    theme_transition_ = 0.0;
}

void Toolbar::toggle_settings_panel(const SettingsPage page) noexcept
{
    if (settings_open_ && settings_page_ == page) {
        settings_open_ = false;
        return;
    }
    settings_page_ = page;
    if (page == SettingsPage::about) {
        about_scroll_ = 0.0;
    }
    settings_open_ = true;
}

void Toolbar::close_settings_panel() noexcept
{
    settings_open_ = false;
}

void Toolbar::set_settings_page(const SettingsPage page) noexcept
{
    settings_page_ = page;
}

void Toolbar::scroll_about(const double delta) noexcept
{
    if (!settings_open_ || settings_page_ != SettingsPage::about) {
        return;
    }
    about_scroll_ = std::clamp(about_scroll_ + delta, 0.0, 1.0);
}

void Toolbar::set_about_scroll(const double position) noexcept
{
    about_scroll_ = std::clamp(position, 0.0, 1.0);
}

void Toolbar::toggle_properties() noexcept
{
    set_properties_open(!properties_open_);
    close_settings_panel();
}

void Toolbar::close_properties() noexcept
{
    set_properties_open(false);
}

void Toolbar::set_properties_open(const bool open) noexcept
{
    if (properties_open_ == open) return;
    properties_open_ = open;
    properties_animation_start_ = properties_reveal_;
    properties_animation_elapsed_ = 0.0;
    if (!properties_laid_out_ || !properties_panel_index_.has_value()) {
        properties_reveal_ = open ? 1.0 : 0.0;
        properties_animation_elapsed_ = 0.18;
    }
    if (focused_action_.has_value() && is_property_control(*focused_action_)) clear_focus();
    pressed_action_.reset();
    repeated_action_.reset();
    tooltip_dismissed_ = true;
}

bool Toolbar::property_controls_active() const noexcept
{
    return properties_open_ && properties_reveal_ == 1.0;
}

void Toolbar::apply_properties_animation() noexcept
{
    if (!properties_panel_index_.has_value() || properties_expanded_bounds_.width <= 0.0) return;
    const auto mix = [&](const double closed, const double expanded) {
        return closed + (expanded - closed) * properties_reveal_;
    };
    const auto interpolate = [&](const UiRect closed, const UiRect expanded) {
        return UiRect{mix(closed.x, expanded.x), mix(closed.y, expanded.y),
            mix(closed.width, expanded.width), mix(closed.height, expanded.height)};
    };
    const UiRect surface = interpolate(properties_tab_bounds_, properties_expanded_bounds_);
    panels_[*properties_panel_index_].bounds = surface;
    properties_bounds_ = properties_reveal_ > 0.0 ? surface : UiRect{};
    for (auto& control : controls_) {
        if (control.action == UiAction::properties_menu) {
            control.bounds = interpolate(properties_tab_toggle_, properties_expanded_toggle_);
            break;
        }
    }
    const double offset = -16.0 * scale_ * (1.0 - properties_reveal_);
    const double delta = offset - properties_content_offset_;
    for (std::size_t i = property_begin_; i < property_end_; ++i) controls_[i].bounds.x += delta;
    for (auto& divider : dividers_) {
        divider.first.x += delta;
        divider.second.x += delta;
    }
    properties_content_offset_ = offset;
    const double pad = 12.0 * scale_;
    properties_clip_ = {surface.x + pad, properties_expanded_clip_.y,
        std::max(0.0, surface.width - 2.0 * pad),
        std::max(0.0, std::min(properties_expanded_clip_.height,
            surface.y + surface.height - pad - properties_expanded_clip_.y))};
}

void Toolbar::set_properties_scroll(const double position) noexcept
{
    const double next = std::clamp(position, 0.0, properties_scroll_limit_);
    const double shift = properties_scroll_ - next;
    properties_scroll_ = next;
    for (std::size_t index = property_begin_; index < property_end_; ++index) {
        controls_[index].bounds.y += shift;
    }
    for (auto& divider : dividers_) {
        divider.first.y += shift;
        divider.second.y += shift;
    }
    // Scrolling during a held click must not activate a newly exposed control.
    pressed_action_.reset();
    repeated_action_.reset();
}

void Toolbar::scroll_properties(const double delta) noexcept
{
    if (property_controls_active() && std::isfinite(delta)) {
        set_properties_scroll(properties_scroll_ + delta);
    }
}

bool Toolbar::properties_open() const noexcept { return properties_open_; }
bool Toolbar::compact_properties() const noexcept { return compact_properties_; }
UiRect Toolbar::properties_bounds() const noexcept { return properties_bounds_; }
UiRect Toolbar::properties_surface_bounds() const noexcept
{
    return properties_panel_index_.has_value() ? panels_[*properties_panel_index_].bounds : UiRect{};
}
double Toolbar::properties_reveal() const noexcept { return properties_reveal_; }
UiRect Toolbar::properties_clip() const noexcept { return properties_clip_; }
UiRect Toolbar::properties_render_clip() const noexcept
{
    if (properties_clip_.width <= 0.0 || properties_clip_.height <= 0.0) return {};
    const double margin = 2.0 * scale_;
    return {properties_clip_.x - margin, properties_clip_.y - margin,
        properties_clip_.width + margin * 2.0, properties_clip_.height + margin * 2.0};
}
double Toolbar::properties_scroll() const noexcept { return properties_scroll_; }
double Toolbar::properties_scroll_limit() const noexcept
{
    return properties_reveal_ > 0.0 ? properties_scroll_limit_ : 0.0;
}

bool Toolbar::is_property_control(const UiAction action) const noexcept
{
    for (std::size_t index = property_begin_; index < property_end_; ++index) {
        if (controls_[index].action == action) return true;
    }
    return false;
}

void Toolbar::begin_custom_color(
    const CustomColorTarget target, const Color color) noexcept
{
    custom_color_target_ = target;
    initial_custom_color_ = color;
    cancel_hex_edit();
    set_custom_color(color);
    settings_page_ = SettingsPage::color_editor;
    settings_open_ = true;
}

void Toolbar::set_custom_color(const Color color) noexcept
{
    const Hsv hsv = to_hsv(color);
    custom_hue_ = hsv.hue;
    custom_saturation_ = hsv.saturation;
    custom_value_ = hsv.value;
}

Color Toolbar::initial_custom_color() const noexcept { return initial_custom_color_; }

void Toolbar::remember_custom_color() noexcept
{
    const Color color = custom_color();
    std::size_t existing = recent_color_count_;
    for (std::size_t i = 0; i < recent_color_count_; ++i) {
        if (same_rgb(recent_colors_[i], color)) { existing = i; break; }
    }
    const std::size_t last = std::min(existing, recent_colors_.size() - 1U);
    for (std::size_t i = last; i > 0U; --i) recent_colors_[i] = recent_colors_[i - 1U];
    recent_colors_[0] = color;
    recent_color_count_ = std::min(recent_color_count_ + (existing == recent_color_count_ ? 1U : 0U),
        recent_colors_.size());
}

void Toolbar::begin_hex_edit()
{
    const Color color = custom_color();
    std::array<char, 8> text{};
    static_cast<void>(std::snprintf(text.data(), text.size(), "#%02X%02X%02X",
        static_cast<unsigned>(color.red), static_cast<unsigned>(color.green), static_cast<unsigned>(color.blue)));
    hex_text_ = text.data();
    hex_editing_ = true;
    hex_replace_all_ = true;
    hex_valid_ = true;
}

void Toolbar::insert_hex_text(const std::string_view text)
{
    if (!hex_editing_) return;
    if (hex_replace_all_) { hex_text_.clear(); hex_replace_all_ = false; }
    // Keep invalid input visible instead of silently changing the requested color.
    hex_text_.append(text.substr(0U, 9U - std::min<std::size_t>(hex_text_.size(), 9U)));
    hex_valid_ = true;
}

void Toolbar::erase_hex_text(const bool all)
{
    if (all || hex_replace_all_) hex_text_.clear();
    else if (!hex_text_.empty()) hex_text_.pop_back();
    hex_replace_all_ = false;
    hex_valid_ = true;
}

void Toolbar::select_hex_text() noexcept { hex_replace_all_ = true; }
bool Toolbar::finish_hex_edit()
{
    if (!hex_editing_) return true;
    std::string_view text = hex_text_;
    if (text.starts_with('#')) text.remove_prefix(1U);
    hex_valid_ = text.size() == 6U || text.size() == 3U;
    unsigned value = 0U;
    for (const char digit : text) {
        const int number = digit >= '0' && digit <= '9' ? digit - '0'
            : (digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                : (digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1));
        if (number < 0) { hex_valid_ = false; break; }
        value = value * 16U + static_cast<unsigned>(number);
    }
    if (!hex_valid_) return false;
    if (text.size() == 3U) value = ((value >> 8U) & 15U) * 0x110000U
        + ((value >> 4U) & 15U) * 0x1100U + (value & 15U) * 0x11U;
    set_custom_color({static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value), 255U});
    cancel_hex_edit();
    return true;
}
void Toolbar::cancel_hex_edit() noexcept { hex_editing_ = false; hex_valid_ = true; hex_replace_all_ = false; }
bool Toolbar::hex_editing() const noexcept { return hex_editing_; }
bool Toolbar::hex_valid() const noexcept { return hex_valid_; }
bool Toolbar::hex_selected() const noexcept { return hex_replace_all_; }
std::string_view Toolbar::hex_text() const noexcept { return hex_text_; }

std::optional<Color> Toolbar::update_custom_color(
    const UiAction field, const Vec2d point) noexcept
{
    const UiControl* const control = find(field);
    if (control == nullptr || control->bounds.width <= 0.0
        || control->bounds.height <= 0.0) {
        return std::nullopt;
    }
    if (field == UiAction::custom_hue_field) {
        const UiRect track = color_field_bounds(field);
        custom_hue_ = std::clamp(
            (point.x - track.x) / track.width,
            0.0, 1.0);
    } else if (field == UiAction::custom_sv_field) {
        const UiRect track = color_field_bounds(field);
        custom_saturation_ = std::clamp(
            (point.x - track.x) / track.width,
            0.0, 1.0);
        custom_value_ = 1.0 - std::clamp(
            (point.y - track.y) / track.height,
            0.0, 1.0);
    } else {
        return std::nullopt;
    }
    return custom_color();
}

std::optional<UiAction> Toolbar::action_at(const Vec2d point) const noexcept
{
    const bool settings_only = settings_open_
        && (settings_scrim_visible() || settings_bounds_.contains(point));
    for (const auto& control : controls_) {
        if (control.enabled
            && (!settings_only
                || is_active_settings_action(control.action))
            && (!is_property_control(control.action)
                || (property_controls_active() && properties_clip_.contains(point)))
            && control.bounds.contains(point)) {
            return control.action;
        }
    }
    return std::nullopt;
}

bool Toolbar::contains(const Vec2d point) const noexcept
{
    return std::ranges::any_of(panels_, [point](const UiPanel& panel) {
        return panel.bounds.contains(point);
    });
}

UiRect Toolbar::filename_bounds() const noexcept
{
    // The status pill is always pushed last (see update()).
    return panels_.empty() ? UiRect{} : panels_.back().bounds;
}

UiRect Toolbar::error_bounds() const noexcept
{
    return error_bounds_;
}

UiRect Toolbar::status_bounds() const noexcept
{
    return status_bounds_;
}

Color Toolbar::current_color() const noexcept
{
    return current_color_;
}

double Toolbar::context_stroke_width() const noexcept
{
    return context_stroke_width_;
}

bool Toolbar::context_stroke_width_mixed() const noexcept
{
    return context_stroke_width_mixed_;
}

bool Toolbar::stroke_width_editing() const noexcept
{
    return stroke_width_editing_;
}

const UiControl* Toolbar::find(const UiAction action) const noexcept
{
    if (properties_reveal_ == 0.0 && is_property_control(action)) return nullptr;
    const auto found = std::find_if(
        controls_.begin(), controls_.end(),
        [action](const UiControl& control) { return control.action == action; });
    return found == controls_.end() ? nullptr : &*found;
}

const UiControl* Toolbar::hovered_control() const noexcept
{
    if (!pointer_.has_value()) {
        return nullptr;
    }
    const bool settings_only = settings_open_
        && (settings_scrim_visible()
            || settings_bounds_.contains(*pointer_));
    for (const auto& control : controls_) {
        if ((!settings_only
                || is_active_settings_action(control.action))
            && (!is_property_control(control.action)
                || (property_controls_active() && properties_clip_.contains(*pointer_)))
            && control.bounds.contains(*pointer_)) {
            return &control;
        }
    }
    return nullptr;
}

const UiControl* Toolbar::focused_control() const noexcept
{
    if (!focused_action_.has_value()
        || (settings_open_
            && !is_active_settings_action(*focused_action_))) {
        return nullptr;
    }
    return find(*focused_action_);
}

const UiControl* Toolbar::focused_tooltip_control() const noexcept
{
    return focus_from_keyboard_ ? focused_control() : nullptr;
}

const UiControl* Toolbar::tooltip_control() const noexcept
{
    if (tooltip_dismissed_) return nullptr;
    if (focus_from_keyboard_) return focused_control();
    return tooltip_elapsed_ >= 0.45 ? hovered_control() : nullptr;
}

bool Toolbar::is_active_settings_action(const UiAction action) const noexcept
{
    if (!settings_open_) {
        return false;
    }
    if (action == UiAction::settings_close) {
        return true;
    }
    if (settings_page_ == SettingsPage::file) {
        return action == UiAction::new_board || action == UiAction::open_board
            || action == UiAction::rename_file
            || action == UiAction::save_copy || action == UiAction::save_as;
    }
    if (settings_page_ == SettingsPage::preferences) {
        return action == UiAction::format_background
            || action == UiAction::about;
    }
    if (settings_page_ == SettingsPage::view) {
        return action == UiAction::zoom_reset
            || action == UiAction::zoom_fit_content
            || action == UiAction::zoom_fit_selection;
    }
    if (settings_page_ == SettingsPage::about) {
        return false;
    }
    if (settings_page_ == SettingsPage::color_editor) {
        return action == UiAction::custom_hue_field
            || action == UiAction::custom_sv_field
            || action == UiAction::custom_color_done || action == UiAction::custom_hex_field
            || action == UiAction::custom_color_cancel
            || (action >= UiAction::recent_color_0 && action <= UiAction::recent_color_5);
    }
    return action >= UiAction::edit_background_custom
        && action <= UiAction::grid_narrow_rule;
}

bool Toolbar::is_settings_control(const UiAction action) const noexcept
{
    return is_active_settings_action(action);
}

const std::vector<UiControl>& Toolbar::controls() const noexcept
{
    return controls_;
}

const std::vector<UiPanel>& Toolbar::panels() const noexcept
{
    return panels_;
}

const std::vector<UiDivider>& Toolbar::dividers() const noexcept
{
    return dividers_;
}

const UiAnimation& Toolbar::animation(const UiAction action) const noexcept
{
    return animations_[static_cast<std::size_t>(action)];
}

std::string_view Toolbar::filename() const noexcept
{
    return filename_;
}

std::string_view Toolbar::error_message() const noexcept
{
    return error_message_;
}

std::string_view Toolbar::status_message() const noexcept
{
    return status_message_;
}

std::string_view Toolbar::document_status() const noexcept { return document_status_; }

double Toolbar::height() const noexcept
{
    return height_;
}

double Toolbar::scale() const noexcept
{
    return scale_;
}

double Toolbar::viewport_width() const noexcept
{
    return viewport_width_;
}

double Toolbar::viewport_height() const noexcept
{
    return viewport_height_;
}

double Toolbar::zoom() const noexcept
{
    return zoom_;
}

bool Toolbar::dirty() const noexcept
{
    return dirty_;
}

bool Toolbar::filename_editing() const noexcept
{
    return filename_editing_;
}

std::size_t Toolbar::filename_cursor() const noexcept
{
    return filename_cursor_;
}

std::size_t Toolbar::filename_anchor() const noexcept
{
    return filename_anchor_;
}

Theme Toolbar::theme() const noexcept
{
    return theme_;
}

Theme Toolbar::previous_theme() const noexcept
{
    return previous_theme_;
}

double Toolbar::theme_transition() const noexcept
{
    return theme_transition_;
}

double Toolbar::reveal() const noexcept
{
    return reveal_;
}

double Toolbar::settings_reveal() const noexcept
{
    return settings_reveal_;
}

bool Toolbar::settings_open() const noexcept
{
    return settings_open_;
}

bool Toolbar::style_color_editor_open() const noexcept
{
    return settings_open_ && settings_page_ == SettingsPage::color_editor
        && (custom_color_target_ == CustomColorTarget::stroke
            || custom_color_target_ == CustomColorTarget::fill);
}

bool Toolbar::settings_scrim_visible() const noexcept
{
    if (!settings_open_ || settings_page_ == SettingsPage::canvas
        || settings_page_ == SettingsPage::file
        || settings_page_ == SettingsPage::preferences) {
        return false;
    }
    return settings_page_ != SettingsPage::color_editor;
}

SettingsPage Toolbar::settings_page() const noexcept
{
    return settings_page_;
}

double Toolbar::about_scroll() const noexcept
{
    return about_scroll_;
}

CustomColorTarget Toolbar::custom_color_target() const noexcept
{
    return custom_color_target_;
}

Color Toolbar::custom_color() const noexcept
{
    return from_hsv(custom_hue_, custom_saturation_, custom_value_);
}

double Toolbar::custom_hue() const noexcept
{
    return custom_hue_;
}

double Toolbar::custom_saturation() const noexcept
{
    return custom_saturation_;
}

double Toolbar::custom_value() const noexcept
{
    return custom_value_;
}

UiRect Toolbar::custom_color_preview_bounds() const noexcept
{
    return custom_color_preview_bounds_;
}

UiRect Toolbar::color_field_bounds(const UiAction field) const noexcept
{
    if (field != UiAction::custom_hue_field && field != UiAction::custom_sv_field) return {};
    const UiControl* const control = find(field);
    if (control == nullptr) return {};
    const double inset = (field == UiAction::custom_hue_field ? 6.0 : 2.0) * scale_;
    UiRect track{control->bounds.x + inset, control->bounds.y + inset,
        std::max(1.0, control->bounds.width - inset * 2.0),
        std::max(1.0, control->bounds.height - inset * 2.0)};
    if (field == UiAction::custom_hue_field) {
        track.height = 18.0 * scale_;
        track.y = control->bounds.y + (control->bounds.height - track.height) * 0.5;
    }
    return track;
}

BackgroundStyle Toolbar::background_style() const noexcept
{
    return background_style_;
}

Color Toolbar::background_color() const noexcept
{
    return background_color_;
}

std::optional<Color> Toolbar::grid_color() const noexcept
{
    return grid_color_;
}



Color Toolbar::color_for(const UiAction action)
{
    const auto index = static_cast<int>(action)
        - static_cast<int>(UiAction::color_white);
    if (index < 0 || index >= static_cast<int>(palette.size())) {
        throw std::invalid_argument{"UI action is not a palette color"};
    }
    return palette[static_cast<std::size_t>(index)];
}

double Toolbar::width_for(const UiAction action)
{
    const auto index = static_cast<int>(action)
        - static_cast<int>(UiAction::width_thin);
    if (index < 0 || index >= static_cast<int>(widths.size())) {
        throw std::invalid_argument{"UI action is not a stroke width"};
    }
    return widths[static_cast<std::size_t>(index)];
}

double Toolbar::roundness_for(const UiAction action)
{
    const auto index = static_cast<int>(action)
        - static_cast<int>(UiAction::roundness_square);
    if (index < 0 || index >= static_cast<int>(roundness_presets.size())) {
        throw std::invalid_argument{"UI action is not rectangle roundness"};
    }
    return roundness_presets[static_cast<std::size_t>(index)];
}

Color Toolbar::background_color_for(const UiAction action)
{
    const auto index = static_cast<int>(action)
        - static_cast<int>(UiAction::bg_color_0);
    if (index < 0 || index >= static_cast<int>(background_palette.size())) {
        throw std::invalid_argument{"UI action is not a background color"};
    }
    return background_palette[static_cast<std::size_t>(index)];
}

Color Toolbar::grid_color_for(const UiAction action)
{
    const auto index = static_cast<int>(action)
        - static_cast<int>(UiAction::grid_color_0);
    if (index < 0 || index >= static_cast<int>(background_palette.size())) {
        throw std::invalid_argument{"UI action is not a grid color"};
    }
    return background_palette[static_cast<std::size_t>(index)];
}

BackgroundStyle Toolbar::style_for(const UiAction action)
{
    const auto index = static_cast<int>(action)
        - static_cast<int>(UiAction::grid_solid);
    if (index < 0
        || index >= static_cast<int>(BackgroundStyle::count)) {
        throw std::invalid_argument{"UI action is not a grid style"};
    }
    return static_cast<BackgroundStyle>(index);
}

} // namespace sawer
