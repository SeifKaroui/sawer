#pragma once

#include "ui/BoardPreview.hpp"
#include "geometry/Geometry.hpp"
#include "ui/Toolbar.hpp"

#include <filesystem>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sawer {

// A single board shown on the home screen. Owns its display strings so the
// UiControl labels can reference them safely.
struct HomeBoard final {
    std::string name;
    std::string date;
    std::shared_ptr<const BoardPreview> preview;
    std::filesystem::path path;
    bool editing{};
    std::size_t edit_cursor{};
    std::size_t edit_anchor{};
    std::string display_date{};
};

// Full-screen gallery of recently opened board files. Cards are stored first
// in controls() (one per board(), in order), followed by Open Board and New
// Board. No folder or workspace ownership is implied by the gallery.
class HomeView final {
public:
    // Logical (pre-scale) card metrics shared with the renderer so the preview
    // thumbnail and the name/date text agree on where the card is divided.
    static constexpr double card_width = 288.0;
    static constexpr double card_height = 262.0;
    static constexpr double card_text_area = 72.0;
    static constexpr double card_preview_margin = 12.0;

    [[nodiscard]] static std::string format_date(
        std::string_view timestamp, std::chrono::year_month_day today);

    void update(
        double viewport_width,
        double viewport_height,
        double display_scale,
        Theme theme,
        std::vector<HomeBoard> boards,
        std::string error_message = {},
        std::string status_message = {});

    // Recompute responsive geometry without replacing board/preview data.
    // Used by live window resize so no file I/O or preview copying occurs.
    void relayout(
        double viewport_width,
        double viewport_height,
        double display_scale,
        Theme theme);

    void set_pointer(Vec2d point) noexcept;
    void clear_pointer() noexcept;
    void pointer_down(Vec2d point) noexcept;
    // Returns true only when press and release target the same control.
    [[nodiscard]] bool pointer_up(Vec2d point) noexcept;
    // Move the gallery by complete rows. Row-snapped scrolling keeps cards
    // from sliding beneath the fixed home header without requiring a GPU
    // clipping surface.
    void scroll_rows(int rows) noexcept;
    void focus_next(bool reverse = false) noexcept;
    void clear_focus() noexcept;
    void tick(double elapsed_seconds) noexcept;
    [[nodiscard]] bool animating() const noexcept;
    // Kept as a lifecycle hook; Home intentionally appears without staged
    // motion so recent files are ready to use immediately.
    void play_entrance() noexcept;

    [[nodiscard]] std::optional<UiAction> action_at(Vec2d point) const noexcept;
    // Index into boards() when a board card is under the point.
    [[nodiscard]] std::optional<std::size_t> board_at(
        Vec2d point) const noexcept;
    // Index into boards() when the small per-card rename button is under it.
    [[nodiscard]] std::optional<std::size_t> rename_at(
        Vec2d point) const noexcept;
    [[nodiscard]] const std::vector<UiRect>& rename_buttons() const noexcept;

    [[nodiscard]] const std::vector<UiControl>& controls() const noexcept;
    [[nodiscard]] const std::vector<UiPanel>& panels() const noexcept;
    [[nodiscard]] const std::vector<HomeBoard>& boards() const noexcept;
    [[nodiscard]] const UiControl* hovered_control() const noexcept;
    [[nodiscard]] const UiControl* focused_control() const noexcept;
    [[nodiscard]] std::optional<std::size_t> focused_index() const noexcept;
    [[nodiscard]] const UiAnimation& animation(std::size_t index) const noexcept;
    [[nodiscard]] double rename_hover(std::size_t index) const noexcept;
    // Per-control entrance progress, 0 (off-stage) to 1 (settled). Board
    // cards start slightly later than the header and stagger by index.
    [[nodiscard]] double entrance(std::size_t index) const noexcept;
    // Whole-view entrance progress used by the backdrop and header text.
    [[nodiscard]] double reveal() const noexcept;
    // Vertical entrance offset shared by geometry, text, hit testing, and
    // thumbnail placement so a card moves as one piece.
    [[nodiscard]] double control_offset(std::size_t index) const noexcept;

    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] Theme theme() const noexcept;
    [[nodiscard]] Theme previous_theme() const noexcept;
    // Progress of the current palette crossfade, 0 (previous) to 1 (current).
    [[nodiscard]] double theme_transition() const noexcept;
    [[nodiscard]] double viewport_width() const noexcept;
    [[nodiscard]] double viewport_height() const noexcept;
    [[nodiscard]] UiRect content_bounds() const noexcept;
    [[nodiscard]] UiRect header_bounds() const noexcept;
    [[nodiscard]] UiRect heading_bounds() const noexcept;
    [[nodiscard]] UiRect status_bounds() const noexcept;
    [[nodiscard]] UiRect board_name_bounds(std::size_t index) const noexcept;
    [[nodiscard]] UiRect board_date_bounds(std::size_t index) const noexcept;
    [[nodiscard]] UiRect board_preview_bounds(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> date_tooltip() const noexcept;
    [[nodiscard]] UiRect scrollbar_track() const noexcept;
    [[nodiscard]] UiRect scrollbar_thumb() const noexcept;
    [[nodiscard]] std::string_view error_message() const noexcept;
    [[nodiscard]] std::string_view status_message() const noexcept;
    [[nodiscard]] double grid_top() const noexcept;
    [[nodiscard]] std::size_t top_row() const noexcept;
    [[nodiscard]] std::size_t maximum_top_row() const noexcept;

private:
    [[nodiscard]] UiRect visual_control_bounds(
        std::size_t index) const noexcept;
    [[nodiscard]] UiRect visual_rename_bounds(
        std::size_t index) const noexcept;
    void ensure_focused_visible() noexcept;

    std::vector<UiControl> controls_;
    std::vector<UiPanel> panels_;
    std::vector<UiRect> rename_buttons_;
    std::vector<UiAnimation> animations_;
    std::vector<double> rename_hovers_;
    std::vector<HomeBoard> boards_;
    std::string error_message_;
    std::string status_message_;
    std::optional<Vec2d> pointer_;
    std::optional<std::size_t> pressed_control_;
    std::optional<std::size_t> pressed_rename_;
    std::optional<std::size_t> focused_control_;
    std::optional<std::size_t> hovered_date_;
    double date_hover_seconds_{};
    UiRect content_bounds_{};
    UiRect header_bounds_{};
    UiRect heading_bounds_{};
    UiRect status_bounds_{};
    double grid_top_{};
    double row_step_{};
    double viewport_width_{1280.0};
    double viewport_height_{720.0};
    double scale_{1.0};
    double open_seconds_{100.0};
    std::size_t top_row_{};
    std::size_t maximum_top_row_{};
    std::size_t row_count_{};
    std::size_t visible_rows_{1U};
    std::size_t columns_{1U};
    Theme theme_{Theme::light};
    Theme previous_theme_{Theme::light};
    double theme_transition_{1.0};
};

} // namespace sawer
