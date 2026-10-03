#pragma once

#include "document/Object.hpp"
#include "geometry/Geometry.hpp"
#include "input/Tool.hpp"
#include "input/DrawingSettings.hpp"

#include <optional>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sawer {

enum class Theme {
    dark,
    light,
};

enum class BackgroundStyle {
    solid,
    dot,
    square,
    graph,
    hybrid,
    diamond,
    wide_rule,
    triangle,
    narrow_rule,
    count,
};

enum class SettingsPage {
    file,
    preferences,
    canvas,
    view,
    about,
    color_editor,
};

enum class CustomColorTarget {
    stroke,
    fill,
    background,
    grid,
};

enum class StyleColorTarget {
    stroke,
    fill,
};

enum class PropertyValueState {
    unavailable,
    uniform,
    mixed,
};

struct SelectionStyleSummary final {
    PropertyValueState stroke_color{PropertyValueState::unavailable};
    Color stroke_color_value;
    PropertyValueState stroke_width{PropertyValueState::unavailable};
    double stroke_width_value{4.0};
    PropertyValueState fill{PropertyValueState::unavailable};
    bool fill_enabled{};
    PropertyValueState fill_color{PropertyValueState::unavailable};
    Color fill_color_value;
    bool fill_partial{};
    PropertyValueState roundness{PropertyValueState::unavailable};
    double roundness_value{};
    bool roundness_partial{};
};

enum class UiAction {
    new_board,
    open_board,
    save,
    save_as,
    go_home,
    rename_board,
    rename_button,
    rename_file,
    home_new_board,
    home_open_board,
    select,
    hand,
    pencil,
    line,
    rectangle,
    ellipse,
    color_white,
    color_red,
    color_amber,
    color_green,
    color_blue,
    color_violet,
    color_black,
    edit_stroke_custom,
    color_target_stroke,
    color_target_fill,
    fill_none,
    toggle_fill,
    width_thin,
    width_regular,
    width_bold,
    width_heavy,
    width_decrease,
    width_cycle,
    width_increase,
    roundness_square,
    roundness_soft,
    roundness_round,
    roundness_full,
    undo,
    redo,
    copy_selection,
    cut_selection,
    paste,
    duplicate,
    delete_selection,
    zoom_out,
    zoom_reset,
    zoom_menu,
    zoom_fit_content,
    zoom_fit_selection,
    zoom_in,
    toggle_theme,
    about,
    format_background,
    settings_close,
    edit_background_custom,
    edit_grid_custom,
    grid_color_auto,
    grid_color_0,
    grid_color_1,
    grid_color_2,
    grid_color_3,
    grid_color_4,
    grid_color_5,
    grid_color_6,
    grid_color_7,
    grid_color_8,
    grid_color_9,
    custom_hue_field,
    custom_sv_field,
    custom_color_done,
    bg_color_0,
    bg_color_1,
    bg_color_2,
    bg_color_3,
    bg_color_4,
    bg_color_5,
    bg_color_6,
    bg_color_7,
    bg_color_8,
    bg_color_9,
    grid_solid,
    grid_dot,
    grid_square,
    grid_graph,
    grid_hybrid,
    grid_diamond,
    grid_wide_rule,
    grid_triangle,
    grid_narrow_rule,
    file_menu,
    preferences_menu,
    properties_menu,
    save_copy,
    custom_hex_field,
    custom_color_cancel,
    recent_color_0,
    recent_color_1,
    recent_color_2,
    recent_color_3,
    recent_color_4,
    recent_color_5,
    count,
};

enum class UiIcon {
    none,
    file_new,
    folder_open,
    home,
    save,
    cursor,
    hand,
    pencil,
    line,
    minus,
    plus,
    rectangle,
    ellipse,
    ban,
    undo,
    redo,
    copy,
    cut,
    paste,
    duplicate,
    trash,
    zoom_out,
    zoom_reset,
    zoom_in,
    theme,
    moon,
    background,
    custom_color,
    back,
    info,
    grid_solid,
    grid_dot,
    grid_square,
    grid_graph,
    grid_hybrid,
    grid_diamond,
    grid_wide_rule,
    grid_triangle,
    grid_narrow_rule,
    settings,
    close,
    check,
    chevron_left,
    chevron_right,
    chevron_down,
    rename,
};

struct UiRect final {
    double x{};
    double y{};
    double width{};
    double height{};

    [[nodiscard]] constexpr bool operator==(const UiRect&) const noexcept =
        default;

    [[nodiscard]] constexpr bool contains(const Vec2d point) const noexcept
    {
        return point.x >= x && point.x <= x + width
            && point.y >= y && point.y <= y + height;
    }
};

struct UiControl final {
    UiAction action{};
    UiRect bounds;
    std::string_view label;
    std::string_view tooltip;
    UiIcon icon{UiIcon::none};
    bool enabled{true};
    bool selected{};
    std::optional<Color> accent;
    bool partial{};
};

struct UiPanel final {
    UiRect bounds;
};

struct UiDivider final {
    Vec2d first;
    Vec2d second;
};

struct UiAnimation final {
    double hover{};
    double press{};
    double selected{};
};

class Toolbar final {
public:
    void update(
        double viewport_width,
        double viewport_height,
        double display_scale,
        Tool current_tool,
        const Style& style,
        bool can_undo,
        bool can_redo,
        bool has_selection,
        double zoom,
        std::string filename,
        bool dirty,
        std::string error_message,
        BackgroundStyle background_style,
        Color background_color,
        std::optional<Color> grid_color = std::nullopt,
        const DrawingSettings& drawing_settings = DrawingSettings{},
        bool filename_editing = false,
        std::size_t filename_cursor = 0U,
        std::size_t filename_anchor = 0U,
        bool has_file = false,
        bool selection_supports_fill = false,
        double rectangle_roundness = 0.0,
        bool selection_supports_roundness = false,
        bool rectangle_roundness_mixed = false,
        const SelectionStyleSummary& selection_style = {},
        StyleColorTarget color_target = StyleColorTarget::stroke,
        bool stroke_width_editing = false,
        std::string_view stroke_width_edit_text = {},
        std::string status_message = {},
        bool loading = false);

    void set_pointer(Vec2d point) noexcept;
    void clear_pointer() noexcept;
    void pointer_down(Vec2d point) noexcept;
    [[nodiscard]] std::optional<UiAction> pointer_up(Vec2d point) noexcept;
    [[nodiscard]] std::optional<UiAction> take_repeated_action() noexcept;
    void focus_next(bool reverse = false) noexcept;
    void clear_focus() noexcept;
    void tick(double elapsed_seconds) noexcept;
    [[nodiscard]] bool animating() const noexcept;
    // Restarts the entrance animation that slides the chrome into view.
    void play_entrance() noexcept;
    void toggle_theme() noexcept;
    void toggle_settings_panel(SettingsPage page = SettingsPage::canvas) noexcept;
    void close_settings_panel() noexcept;
    void set_settings_page(SettingsPage page) noexcept;
    void scroll_about(double delta) noexcept;
    void set_about_scroll(double position) noexcept;
    void toggle_properties() noexcept;
    void close_properties() noexcept;
    void scroll_properties(double delta) noexcept;
    [[nodiscard]] bool properties_open() const noexcept;
    [[nodiscard]] bool compact_properties() const noexcept;
    [[nodiscard]] UiRect properties_bounds() const noexcept;
    [[nodiscard]] UiRect properties_surface_bounds() const noexcept;
    [[nodiscard]] double properties_reveal() const noexcept;
    [[nodiscard]] UiRect properties_clip() const noexcept;
    // Focus outlines need a small margin outside the input/scrolling clip.
    [[nodiscard]] UiRect properties_render_clip() const noexcept;
    [[nodiscard]] double properties_scroll() const noexcept;
    [[nodiscard]] double properties_scroll_limit() const noexcept;
    [[nodiscard]] bool is_property_control(UiAction action) const noexcept;
    void begin_custom_color(CustomColorTarget target, Color color) noexcept;
    [[nodiscard]] std::optional<Color> update_custom_color(
        UiAction field, Vec2d point) noexcept;

    [[nodiscard]] std::optional<UiAction> action_at(Vec2d point) const noexcept;
    [[nodiscard]] bool contains(Vec2d point) const noexcept;
    // Bounds of the filename status control in the application bar.
    [[nodiscard]] UiRect filename_bounds() const noexcept;
    // Bounds of the inline save-error notice, or an empty rectangle.
    [[nodiscard]] UiRect error_bounds() const noexcept;
    [[nodiscard]] UiRect status_bounds() const noexcept;
    [[nodiscard]] Color current_color() const noexcept;
    [[nodiscard]] double context_stroke_width() const noexcept;
    [[nodiscard]] bool context_stroke_width_mixed() const noexcept;
    [[nodiscard]] bool stroke_width_editing() const noexcept;
    [[nodiscard]] const UiControl* find(UiAction action) const noexcept;
    [[nodiscard]] const UiControl* hovered_control() const noexcept;
    [[nodiscard]] const UiControl* focused_control() const noexcept;
    // Keyboard focus may describe a control without pointer hover. Mouse
    // focus retains its focus ring but must not pin a tooltip after exit.
    [[nodiscard]] const UiControl* focused_tooltip_control() const noexcept;
    [[nodiscard]] const UiControl* tooltip_control() const noexcept;
    [[nodiscard]] const std::vector<UiControl>& controls() const noexcept;
    [[nodiscard]] const std::vector<UiPanel>& panels() const noexcept;
    [[nodiscard]] const std::vector<UiDivider>& dividers() const noexcept;
    [[nodiscard]] const UiAnimation& animation(UiAction action) const noexcept;
    [[nodiscard]] std::string_view filename() const noexcept;
    [[nodiscard]] std::string_view error_message() const noexcept;
    [[nodiscard]] std::string_view status_message() const noexcept;
    [[nodiscard]] std::string_view document_status() const noexcept;
    [[nodiscard]] double height() const noexcept;
    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] double viewport_width() const noexcept;
    [[nodiscard]] double viewport_height() const noexcept;
    [[nodiscard]] double zoom() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool filename_editing() const noexcept;
    [[nodiscard]] std::size_t filename_cursor() const noexcept;
    [[nodiscard]] std::size_t filename_anchor() const noexcept;
    [[nodiscard]] Theme theme() const noexcept;
    [[nodiscard]] Theme previous_theme() const noexcept;
    // Progress of the current palette crossfade, 0 (previous) to 1 (current).
    [[nodiscard]] double theme_transition() const noexcept;
    // Entrance progress, 0 (hidden) to 1 (settled); eased by tick().
    [[nodiscard]] double reveal() const noexcept;
    // Settings flyout fade-in progress, 0 to 1.
    [[nodiscard]] double settings_reveal() const noexcept;
    [[nodiscard]] bool settings_open() const noexcept;
    [[nodiscard]] bool style_color_editor_open() const noexcept;
    [[nodiscard]] bool is_settings_control(UiAction action) const noexcept;
    // Canvas appearance controls stay undimmed so their live color and pattern
    // previews can be judged against the actual board.
    [[nodiscard]] bool settings_scrim_visible() const noexcept;
    [[nodiscard]] SettingsPage settings_page() const noexcept;
    [[nodiscard]] double about_scroll() const noexcept;
    [[nodiscard]] CustomColorTarget custom_color_target() const noexcept;
    [[nodiscard]] Color custom_color() const noexcept;
    [[nodiscard]] double custom_hue() const noexcept;
    [[nodiscard]] double custom_saturation() const noexcept;
    [[nodiscard]] double custom_value() const noexcept;
    [[nodiscard]] UiRect custom_color_preview_bounds() const noexcept;
    // Shared visible track bounds keep picker handles and pointer mapping aligned.
    [[nodiscard]] UiRect color_field_bounds(UiAction field) const noexcept;
    void set_custom_color(Color color) noexcept;
    void remember_custom_color() noexcept;
    [[nodiscard]] Color initial_custom_color() const noexcept;
    void begin_hex_edit();
    void insert_hex_text(std::string_view text);
    void erase_hex_text(bool all = false);
    void select_hex_text() noexcept;
    [[nodiscard]] bool finish_hex_edit();
    void cancel_hex_edit() noexcept;
    [[nodiscard]] bool hex_editing() const noexcept;
    [[nodiscard]] bool hex_valid() const noexcept;
    [[nodiscard]] bool hex_selected() const noexcept;
    [[nodiscard]] std::string_view hex_text() const noexcept;
    [[nodiscard]] BackgroundStyle background_style() const noexcept;
    [[nodiscard]] Color background_color() const noexcept;
    [[nodiscard]] std::optional<Color> grid_color() const noexcept;

    [[nodiscard]] static Color color_for(UiAction action);
    [[nodiscard]] static double width_for(UiAction action);
    [[nodiscard]] static double roundness_for(UiAction action);
    [[nodiscard]] static Color background_color_for(UiAction action);
    [[nodiscard]] static Color grid_color_for(UiAction action);
    [[nodiscard]] static BackgroundStyle style_for(UiAction action);

private:
    void build_settings_panel();
    void set_properties_scroll(double position) noexcept;
    void set_properties_open(bool open) noexcept;
    void apply_properties_animation() noexcept;
    [[nodiscard]] bool property_controls_active() const noexcept;
    [[nodiscard]] bool is_active_settings_action(
        UiAction action) const noexcept;

    std::vector<UiControl> controls_;
    std::vector<UiPanel> panels_;
    std::vector<UiDivider> dividers_;
    UiRect error_bounds_;
    UiRect status_bounds_;
    UiRect settings_bounds_;
    UiRect custom_color_preview_bounds_;
    UiRect properties_bounds_;
    UiRect properties_clip_;
    std::size_t property_begin_{};
    std::size_t property_end_{};
    double properties_scroll_{};
    double properties_scroll_limit_{};
    bool compact_properties_{};
    bool properties_open_{true};
    bool properties_laid_out_{};
    double properties_reveal_{1.0};
    double properties_animation_start_{1.0};
    double properties_animation_elapsed_{0.18};
    double properties_content_offset_{};
    std::optional<std::size_t> properties_panel_index_;
    UiRect properties_expanded_bounds_;
    UiRect properties_expanded_clip_;
    UiRect properties_expanded_toggle_;
    UiRect properties_tab_bounds_;
    UiRect properties_tab_toggle_;
    Tool properties_tool_{Tool::pencil};
    Color current_color_{31U, 41U, 55U, 255U};
    bool current_color_mixed_{};
    double context_stroke_width_{4.0};
    bool context_stroke_width_mixed_{};
    bool stroke_width_editing_{};
    std::string stroke_width_label_{"4 px"};
    std::optional<Vec2d> pointer_;
    std::optional<UiAction> pressed_action_;
    std::optional<UiAction> repeated_action_;
    std::optional<UiAction> focused_action_;
    bool focus_from_keyboard_{};
    double press_hold_time_{};
    double press_repeat_accumulator_{};
    std::optional<UiAction> tooltip_action_;
    double tooltip_elapsed_{};
    bool tooltip_dismissed_{};
    std::array<UiAnimation, static_cast<std::size_t>(UiAction::count)>
        animations_{};
    std::string filename_{"Untitled"};
    std::string error_message_;
    std::string status_message_;
    std::string document_status_;
    double height_{56.0};
    double scale_{1.0};
    double viewport_width_{1280.0};
    double viewport_height_{720.0};
    double zoom_{1.0};
    bool dirty_{};
    bool loading_{};
    bool has_file_{};
    bool filename_editing_{};
    bool has_selection_{};
    std::size_t filename_cursor_{};
    std::size_t filename_anchor_{};
    Theme theme_{Theme::light};
    Theme previous_theme_{Theme::light};
    double theme_transition_{1.0};
    double reveal_{1.0};
    double settings_reveal_{};
    bool settings_open_{};
    bool inline_color_palette_{};
    SettingsPage settings_page_{SettingsPage::canvas};
    double about_scroll_{};
    CustomColorTarget custom_color_target_{CustomColorTarget::background};
    double custom_hue_{};
    double custom_saturation_{1.0};
    double custom_value_{1.0};
    Color initial_custom_color_;
    std::array<Color, 6> recent_colors_{};
    std::size_t recent_color_count_{};
    bool hex_editing_{};
    bool hex_replace_all_{};
    bool hex_valid_{true};
    std::string hex_text_;
    BackgroundStyle background_style_{BackgroundStyle::dot};
    Color background_color_{240U, 242U, 247U, 255U};
    std::optional<Color> grid_color_;
};

} // namespace sawer
