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

constexpr std::array<double, 4> widths{{2.5, 4.0, 7.0, 14.0}};
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
        "Solid",
        "Dot",
        "Square",
        "Graph",
        "Hybrid",
        "Diamond",
        "Wide rule",
        "Triangle",
        "Narrow rule",
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
    bool stabilization{};
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
            .stabilization = true,
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
    const DrawingSettings& drawing_settings,
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
    const std::string_view stroke_width_edit_text)
{
    viewport_width_ = std::max(viewport_width, 1.0);
    viewport_height_ = std::max(viewport_height, 1.0);
    const double available_scale = std::max(
        0.82,
        std::min(viewport_width_ / 760.0, viewport_height_ / 470.0));
    scale_ = std::min(std::clamp(display_scale, 0.82, 1.5), available_scale);
    height_ = 56.0 * scale_;
    zoom_ = zoom;
    filename_ = std::move(filename);
    dirty_ = dirty;
    filename_editing_ = filename_editing;
    has_selection_ = has_selection;
    filename_cursor_ = std::min(filename_cursor, filename_.size());
    filename_anchor_ = std::min(filename_anchor, filename_.size());
    error_message_ = std::move(error_message);
    background_style_ = background_style;
    background_color_ = background_color;
    grid_color_ = grid_color;
    drawing_settings_ = drawing_settings;

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

    const double button = std::max(36.0 * scale_, 32.0);
    const double gap = 3.0 * scale_;
    const double panel_pad = 6.0 * scale_;
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
            .enabled = enabled,
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

    // A full-width application bar anchors file actions and document state.
    panels_.push_back({{0.0, 0.0, viewport_width_, height_}});
    x = 12.0 * scale_;
    y = 10.0 * scale_;
    add(
        UiAction::go_home, button, UiIcon::home, "",
        "Home - all boards", true, false);
    add(UiAction::new_board, button, UiIcon::file_new, "", "New board  Ctrl+N", true, false);
    add(UiAction::open_board, button, UiIcon::folder_open, "", "Open board  Ctrl+O", true, false);
    if (has_file) {
        add(
            UiAction::save, button, UiIcon::save, "",
            "Save  Ctrl+S", true, false);
        add(
            UiAction::save_as, 76.0 * scale_, UiIcon::none, "Save as...",
            "Save a copy in another location  Ctrl+Shift+S", true, false);
    } else {
        add(
            UiAction::save_as, 76.0 * scale_, UiIcon::none, "Save...",
            "Choose where to save this board  Ctrl+S", true, false);
    }
    const double filename_x = x + 6.0 * scale_;

    const double history_width = button * 2.0 + gap;
    x = viewport_width_ - 12.0 * scale_ - history_width;
    const double history_x = x;
    add(UiAction::undo, button, UiIcon::undo, "", "Undo  Ctrl+Z", can_undo, false);
    add(UiAction::redo, button, UiIcon::redo, "", "Redo  Ctrl+Shift+Z", can_redo, false);

    // Primary tools live in a stable left rail, separate from file commands.
    const double tool_panel_x = 10.0 * scale_;
    const double tool_panel_height =
        button * 6.0 + gap * 5.0 + 6.0 * scale_ + panel_pad * 2.0;
    const double tool_panel_y = std::max(
        height_ + 8.0 * scale_,
        height_ + (viewport_height_ - height_ - tool_panel_height) * 0.5);
    const double tool_x = tool_panel_x + panel_pad;
    double tool_y = tool_panel_y + panel_pad;
    double active_tool_center_y = tool_y + button * 0.5;
    const auto add_tool = [&](const UiAction action,
                              const UiIcon icon,
                              const std::string_view tooltip,
                              const bool selected) {
        add_at(
            action, {tool_x, tool_y, button, button}, icon, "",
            tooltip, true, selected);
        if (selected) {
            active_tool_center_y = tool_y + button * 0.5;
        }
        tool_y += button + gap;
    };
    add_tool(
        UiAction::select, UiIcon::cursor, "Select and edit  V",
        current_tool == Tool::select);
    add_tool(
        UiAction::hand, UiIcon::hand,
        "Hand / Pan  H - hold Space or drag with middle mouse",
        current_tool == Tool::hand);
    tool_y += 6.0 * scale_;
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
    panels_.push_back({{
        tool_panel_x,
        tool_panel_y,
        button + panel_pad * 2.0,
        tool_y - tool_panel_y - gap + panel_pad,
    }});

    // Capabilities determine the rail instead of exposing generic controls
    // that the active tool or selection cannot use.
    const double sidecar_gap = 8.0 * scale_;
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
    const double context_label_height = 18.0 * scale_;
    const double minimum_color_target_width =
        std::max(68.0 * scale_, 60.0);
    const double color_target_content_width = capabilities.color_target
        ? minimum_color_target_width * 2.0
            + compact_button + gap * 2.0
        : 0.0;
    const double context_content_width = std::max({
        156.0 * scale_,
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
            ? context_label_height + compact_button * 2.0 + gap
            : 0.0,
        capabilities.color_target ? button : 0.0,
        capabilities.width
            ? context_label_height + compact_button + gap + button
            : 0.0,
        capabilities.roundness
            ? context_label_height + compact_button
            : 0.0,
    });
    const double behavior_height =
        capabilities.stabilization
        ? context_label_height + button * 2.0 + gap
        : 0.0;
    const double action_height =
        capabilities.selection_actions ? button * 2.0 + gap : 0.0;
    const std::size_t group_count =
        static_cast<std::size_t>(appearance_height > 0.0)
        + static_cast<std::size_t>(behavior_height > 0.0)
        + static_cast<std::size_t>(action_height > 0.0);
    if (group_count > 0U) {
        const double context_panel_x =
            tool_panel_x + button + panel_pad * 2.0 + sidecar_gap;
        const double section_gap = 13.0 * scale_;
        const double context_panel_height =
            appearance_height
            + behavior_height
            + action_height
            + static_cast<double>(group_count - 1U) * section_gap
            + panel_pad * 2.0;
        const double minimum_context_y = height_ + 8.0 * scale_;
        const double maximum_context_y = std::max(
            minimum_context_y,
            viewport_height_ - context_panel_height - 8.0 * scale_);
        const double context_panel_y = std::clamp(
            active_tool_center_y - context_panel_height * 0.5,
            minimum_context_y,
            maximum_context_y);
        const double context_horizontal_pad = panel_pad;
        const double pair_width = button * 2.0 + gap;
        const double preset_width =
            (context_content_width - gap * 3.0) * 0.25;
        const double width_value_width = std::max(42.0 * scale_, 40.0);
        const double minimum_stepper_width =
            button * 2.0 + width_value_width + gap * 2.0;
        const double content_width = std::max(
            context_content_width, minimum_stepper_width);
        const double context_x = context_panel_x + context_horizontal_pad;
        const double pair_x =
            context_x + (content_width - pair_width) * 0.5;
        const double width_value_actual =
            content_width - button * 2.0 - gap * 2.0;
        double context_y = context_panel_y + panel_pad;
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

        if (capabilities.color_target) {
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
            const double target_width =
                (content_width - compact_button - gap * 2.0) * 0.5;
            add_at(
                UiAction::color_target_stroke,
                {context_x, context_y, target_width, button},
                UiIcon::none,
                "Stroke",
                "",
                true,
                effective_color_target == StyleColorTarget::stroke,
                stroke_accent);
            add_at(
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
                    : "",
                capabilities.fill,
                effective_color_target == StyleColorTarget::fill,
                fill_accent,
                fill_partial);
            add_at(
                UiAction::fill_none,
                {
                    context_x + (target_width + gap) * 2.0,
                    context_y + (button - compact_button) * 0.5,
                    compact_button,
                    compact_button,
                },
                UiIcon::ban,
                "",
                fill_mixed
                    ? "Mixed fill - remove fill from shapes"
                    : (fill_partial
                        ? "No fill - applies to shapes only"
                        : "No fill"),
                capabilities.fill,
                no_fill_selected,
                std::nullopt,
                fill_partial);
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
                                * (compact_button + gap),
                        preset_width,
                        compact_button,
                    },
                    UiIcon::none,
                    "",
                    "",
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
                    context_y + compact_button + gap,
                    preset_width,
                    compact_button,
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
            context_y += compact_button * 2.0 + gap;
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
                    "",
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
                "",
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
                "",
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
                "",
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

        if (appearance_height > 0.0
            && (behavior_height > 0.0 || action_height > 0.0)) {
            add_section_divider();
        }

        if (capabilities.stabilization) {
            context_y += context_label_height;
            constexpr std::array stabilization_actions{
                UiAction::stabilization_off,
                UiAction::stabilization_light,
                UiAction::stabilization_default,
                UiAction::stabilization_strong,
            };
            constexpr std::array<std::string_view, 4> stabilization_labels{{
                "Off",
                "Light",
                "Medium",
                "Strong",
            }};
            constexpr std::array<std::string_view, 4> stabilization_tooltips{{
                "",
                "",
                "",
                "",
            }};
            const double stabilization_width =
                (content_width - gap) / 2.0;
            for (std::size_t index = 0U;
                 index < stabilization_actions.size(); ++index) {
                const std::size_t column = index % 2U;
                const std::size_t row = index / 2U;
                add_at(
                    stabilization_actions[index],
                    {
                        context_x
                            + static_cast<double>(column)
                                * (stabilization_width + gap),
                        context_y
                            + static_cast<double>(row) * (button + gap),
                        stabilization_width,
                        button,
                    },
                    UiIcon::none,
                    stabilization_labels[index],
                    stabilization_tooltips[index],
                    true,
                    drawing_settings_.stabilization
                        == static_cast<StrokeStabilization>(index));
            }
            context_y += button * 2.0 + gap;
        }
        if (behavior_height > 0.0 && action_height > 0.0) {
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
            add_at(
                UiAction::duplicate,
                {pair_x, context_y, button, button},
                UiIcon::duplicate,
                "",
                "Duplicate selection  Ctrl+D",
                true,
                false);
            add_at(
                UiAction::delete_selection,
                {pair_x + button + gap, context_y, button, button},
                UiIcon::trash,
                "",
                "Delete selection  Delete",
                true,
                false);
        }
        panels_.push_back({{
            context_panel_x,
            context_panel_y,
            content_width + context_horizontal_pad * 2.0,
            context_panel_height,
        }});
    }

    // View and appearance controls stay at the lower-right canvas edge.
    const double bottom_y = viewport_height_ - 48.0 * scale_;
    constexpr double estimated_view_width = 258.0;
    x = std::max(
        72.0 * scale_,
        viewport_width_ - (estimated_view_width + 12.0) * scale_);
    y = bottom_y;
    const double zoom_start = x;
    add(UiAction::zoom_out, button, UiIcon::zoom_out, "", "Zoom out  Ctrl+-", true, false);
    add(
        UiAction::zoom_menu,
        button * 1.25,
        UiIcon::zoom_reset,
        "100%",
        "Zoom and framing",
        true,
        settings_open_ && settings_page_ == SettingsPage::view);
    add(UiAction::zoom_in, button, UiIcon::zoom_in, "", "Zoom in  Ctrl++", true, false);
    add(
        UiAction::format_background,
        button,
        UiIcon::background,
        "",
        "Format background",
        true,
        settings_open_
            && (settings_page_ == SettingsPage::canvas
                || (settings_page_ == SettingsPage::color_editor
                    && custom_color_target_ != CustomColorTarget::stroke
                    && custom_color_target_ != CustomColorTarget::fill)));
    add(
        UiAction::toggle_theme,
        button,
        UiIcon::theme,
        "",
        "Light or dark theme  T",
        true,
        false);
    add(
        UiAction::about,
        button,
        UiIcon::info,
        "",
        "About Sawer",
        true,
        settings_open_ && settings_page_ == SettingsPage::about);
    finish_panel(zoom_start);

    if (!error_message_.empty()) {
        error_bounds_ = {
            12.0 * scale_,
            std::max(
                height_ + 6.0 * scale_,
                bottom_y - 44.0 * scale_),
            std::min(
                420.0 * scale_,
                std::max(viewport_width_ - 24.0 * scale_, 1.0)),
            button,
        };
        panels_.push_back({error_bounds_});
    }

    if (settings_open_) {
        build_settings_panel();
    }

    // The document title sits inside the application bar. It is pushed last so
    // filename_bounds() and the renderer retain their stable lookup.
    const double estimated_filename_width =
        (52.0 + static_cast<double>(filename_.size()) * 7.6) * scale_;
    const double available_filename_width = std::max(
        96.0 * scale_,
        history_x - filename_x - 10.0 * scale_);
    const double completed_maximum =
        std::min(280.0 * scale_, available_filename_width);
    const double minimum_filename_width =
        std::min(116.0 * scale_, completed_maximum);
    const double maximum_filename_width = filename_editing_
        ? std::min(620.0 * scale_, available_filename_width)
        : completed_maximum;
    const UiRect status_bounds{
        filename_x,
        10.0 * scale_,
        std::clamp(
            estimated_filename_width,
            minimum_filename_width,
            maximum_filename_width),
        button,
    };
    const double rename_size = std::max(28.0 * scale_, 32.0);
    controls_.push_back(UiControl{
        .action = UiAction::rename_board,
        .bounds = {
            status_bounds.x + status_bounds.width
                - rename_size - 4.0 * scale_,
            status_bounds.y + (status_bounds.height - rename_size) * 0.5,
            rename_size,
            rename_size,
        },
        .label = {},
        .tooltip = "Rename board",
        .icon = UiIcon::pencil,
        .enabled = true,
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
    const double gap = 6.0 * scale_;
    const bool about_page = settings_page_ == SettingsPage::about;
    const double preferred_panel_width =
        (about_page ? 720.0 : 304.0) * scale_;
    const double panel_width = std::min(
        preferred_panel_width,
        std::max(viewport_width_ - 24.0 * scale_, 1.0));
    const double desired_panel_height = settings_page_ == SettingsPage::canvas
        ? 446.0 * scale_
        : (settings_page_ == SettingsPage::color_editor
            ? 360.0 * scale_
            : (about_page ? 620.0 * scale_ : 216.0 * scale_));
    const double panel_height = std::min(
        desired_panel_height,
        std::max(viewport_height_ - 24.0 * scale_, 1.0));
    const bool style_color_editor =
        settings_page_ == SettingsPage::color_editor
        && (custom_color_target_ == CustomColorTarget::stroke
            || custom_color_target_ == CustomColorTarget::fill);
    const double rightmost_panel_x = std::max(
        16.0 * scale_,
        viewport_width_ - panel_width - 12.0 * scale_);
    const double panel_x = style_color_editor || about_page
        ? std::clamp(
            (viewport_width_ - panel_width) * 0.5,
            12.0 * scale_,
            rightmost_panel_x)
        : rightmost_panel_x;
    const double lowest_panel_y = std::max(
        16.0 * scale_,
        viewport_height_ - 60.0 * scale_ - panel_height);
    const double panel_y = style_color_editor || about_page
        ? std::clamp(
            (viewport_height_ - panel_height) * 0.5,
            12.0 * scale_,
            std::max(12.0 * scale_,
                viewport_height_ - panel_height - 12.0 * scale_))
        : lowest_panel_y;

    controls_.push_back(UiControl{
        .action = UiAction::settings_close,
        .bounds = {
            panel_x + pad,
            panel_y + pad * 0.5,
            std::max(header * 0.9, 32.0),
            std::max(header * 0.9, 32.0),
        },
        .label = {},
        .tooltip = settings_page_ == SettingsPage::color_editor
            ? "Back"
            : "Close",
        .icon = UiIcon::back,
        .enabled = true,
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
            .enabled = enabled,
            .selected = selected,
            .accent = accent,
        });
    };

    if (settings_page_ == SettingsPage::canvas) {
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
        const double swatch = std::max(36.0 * scale_, 32.0);
        const double swatch_gap = 6.0 * scale_;
        const double row_x = panel_x + pad;
        double row_y = panel_y + pad + header + 28.0 * scale_;
        for (std::size_t index = 0U; index < background_actions.size(); ++index) {
            const std::size_t row = index / 6U;
            const std::size_t column = index % 6U;
            add_control(
                background_actions[index],
                {row_x + static_cast<double>(column) * (swatch + swatch_gap),
                 row_y + static_cast<double>(row) * (swatch + swatch_gap),
                 swatch, swatch},
                {}, "Background color",
                same_rgb(background_color_, background_palette[index]),
                UiIcon::none, background_palette[index]);
        }
        add_control(
            UiAction::edit_background_custom,
            {row_x + 4.0 * (swatch + swatch_gap), row_y + swatch + swatch_gap,
             swatch, swatch},
            {}, "Custom background color",
            std::ranges::none_of(background_palette, [&](const Color color) {
                return same_rgb(background_color_, color);
            }),
            UiIcon::custom_color);

        row_y += 2.0 * (swatch + swatch_gap) + 30.0 * scale_;
        add_control(
            UiAction::grid_color_auto,
            {row_x, row_y, swatch, swatch},
            "Auto", "Automatic grid color", !grid_color_.has_value());
        for (std::size_t index = 0U; index < grid_color_actions.size(); ++index) {
            const std::size_t slot = index + 1U;
            const std::size_t row = slot / 6U;
            const std::size_t column = slot % 6U;
            add_control(
                grid_color_actions[index],
                {row_x + static_cast<double>(column) * (swatch + swatch_gap),
                 row_y + static_cast<double>(row) * (swatch + swatch_gap),
                 swatch, swatch},
                {}, "Grid color",
                grid_color_.has_value()
                    && same_rgb(*grid_color_, background_palette[index]),
                UiIcon::none, background_palette[index]);
        }
        add_control(
            UiAction::edit_grid_custom,
            {row_x + 5.0 * (swatch + swatch_gap), row_y + swatch + swatch_gap,
             swatch, swatch},
            {}, "Custom grid color",
            grid_color_.has_value()
                && std::ranges::none_of(
                    background_palette, [&](const Color color) {
                        return same_rgb(*grid_color_, color);
                    }),
            UiIcon::custom_color);

        row_y += 2.0 * (swatch + swatch_gap) + 30.0 * scale_;
        const double tile = 44.0 * scale_;
        for (std::size_t index = 0U; index < grid_actions.size(); ++index) {
            const std::size_t row = index / 5U;
            const std::size_t column = index % 5U;
            add_control(
                grid_actions[index],
                {row_x + static_cast<double>(column) * (tile + gap),
                 row_y + static_cast<double>(row) * (tile + gap), tile, tile},
                {}, grid_tooltips[index],
                static_cast<std::size_t>(background_style_) == index,
                grid_icons[index]);
        }
    } else if (settings_page_ == SettingsPage::view) {
        const double field_x = panel_x + pad;
        const double field_width = panel_width - pad * 2.0;
        const double row_height = std::max(36.0 * scale_, 32.0);
        double row_y = panel_y + pad + header + 18.0 * scale_;
        add_control(
            UiAction::zoom_reset,
            {field_x, row_y, field_width, row_height},
            "100%", "Reset zoom to 100%", false);
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
        const double hue_y = panel_y + pad + header + 24.0 * scale_;
        add_control(
            UiAction::custom_hue_field,
            {field_x, hue_y, field_width, std::max(24.0 * scale_, 32.0)},
            {}, "Hue", false);
        add_control(
            UiAction::custom_sv_field,
            {field_x, hue_y + 48.0 * scale_, field_width, 150.0 * scale_},
            {}, "Saturation and brightness", false);
        add_control(
            UiAction::custom_color_done,
            {
                field_x,
                hue_y + 222.0 * scale_,
                field_width,
                std::max(36.0 * scale_, 32.0),
            },
            "Done", "Use this color", true);
    }

    settings_bounds_ = {panel_x, panel_y, panel_width, panel_height};
    panels_.push_back({settings_bounds_});
}

void Toolbar::set_pointer(const Vec2d point) noexcept
{
    pointer_ = point;
}

void Toolbar::clear_pointer() noexcept
{
    pointer_.reset();
    pressed_action_.reset();
    repeated_action_.reset();
    press_hold_time_ = 0.0;
    press_repeat_accumulator_ = 0.0;
}

void Toolbar::pointer_down(const Vec2d point) noexcept
{
    pointer_ = point;
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
    std::vector<UiAction> focusable;
    focusable.reserve(controls_.size());
    for (const auto& control : controls_) {
        if (control.enabled
            && (!settings_open_
                || is_active_settings_action(control.action))
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
}

void Toolbar::clear_focus() noexcept
{
    focused_action_.reset();
    focus_from_keyboard_ = false;
}

void Toolbar::tick(const double elapsed_seconds) noexcept
{
    const double elapsed = std::clamp(elapsed_seconds, 0.0, 0.1);
    const UiControl* const hovered = hovered_control();
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
        || pressed_action_.has_value()) {
        return true;
    }
    const UiControl* const hovered = hovered_control();
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

void Toolbar::begin_custom_color(
    const CustomColorTarget target, const Color color) noexcept
{
    custom_color_target_ = target;
    const Hsv hsv = to_hsv(color);
    custom_hue_ = hsv.hue;
    custom_saturation_ = hsv.saturation;
    custom_value_ = hsv.value;
    settings_page_ = SettingsPage::color_editor;
    settings_open_ = true;
}

std::optional<Color> Toolbar::update_custom_color(
    const UiAction field, const Vec2d point) noexcept
{
    const UiControl* const control = find(field);
    if (control == nullptr || control->bounds.width <= 0.0
        || control->bounds.height <= 0.0) {
        return std::nullopt;
    }
    if (field == UiAction::custom_hue_field) {
        custom_hue_ = std::clamp(
            (point.x - control->bounds.x) / control->bounds.width,
            0.0, 1.0);
    } else if (field == UiAction::custom_sv_field) {
        custom_saturation_ = std::clamp(
            (point.x - control->bounds.x) / control->bounds.width,
            0.0, 1.0);
        custom_value_ = 1.0 - std::clamp(
            (point.y - control->bounds.y) / control->bounds.height,
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

bool Toolbar::is_active_settings_action(const UiAction action) const noexcept
{
    if (!settings_open_) {
        return false;
    }
    if (action == UiAction::settings_close) {
        return true;
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
            || action == UiAction::custom_color_done;
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
    if (!settings_open_ || settings_page_ == SettingsPage::canvas) {
        return false;
    }
    return settings_page_ != SettingsPage::color_editor
        || custom_color_target_ == CustomColorTarget::stroke
        || custom_color_target_ == CustomColorTarget::fill;
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
