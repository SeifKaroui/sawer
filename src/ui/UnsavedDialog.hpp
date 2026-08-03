#pragma once

#include "ui/Toolbar.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace sawer {

enum class UnsavedDialogChoice {
    cancel,
    discard,
    save,
    count,
};

class UnsavedDialog final {
public:
    void open(
        double viewport_width,
        double viewport_height,
        double display_scale,
        std::string board_name);
    void close() noexcept;
    void relayout(
        double viewport_width,
        double viewport_height,
        double display_scale) noexcept;

    void set_pointer(Vec2d point) noexcept;
    void clear_pointer() noexcept;
    void pointer_down(Vec2d point) noexcept;
    [[nodiscard]] std::optional<UnsavedDialogChoice> pointer_up(
        Vec2d point) noexcept;
    void focus_next(bool reverse = false) noexcept;
    [[nodiscard]] UnsavedDialogChoice focused_choice() const noexcept;
    void tick(double elapsed_seconds) noexcept;

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool animating() const noexcept;
    [[nodiscard]] double reveal() const noexcept;
    [[nodiscard]] double hover(UnsavedDialogChoice choice) const noexcept;
    [[nodiscard]] double press(UnsavedDialogChoice choice) const noexcept;
    [[nodiscard]] std::optional<UnsavedDialogChoice> hovered_choice()
        const noexcept;
    [[nodiscard]] UiRect bounds() const noexcept;
    [[nodiscard]] UiRect button_bounds(
        UnsavedDialogChoice choice) const noexcept;
    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] std::string_view board_name() const noexcept;

private:
    static constexpr std::size_t choice_count =
        static_cast<std::size_t>(UnsavedDialogChoice::count);
    [[nodiscard]] static constexpr std::size_t index(
        const UnsavedDialogChoice choice) noexcept
    {
        return static_cast<std::size_t>(choice);
    }
    [[nodiscard]] std::optional<UnsavedDialogChoice> choice_at(
        Vec2d point) const noexcept;

    bool visible_{};
    double viewport_width_{1280.0};
    double viewport_height_{720.0};
    double scale_{1.0};
    double open_seconds_{};
    UiRect bounds_;
    std::array<UiRect, choice_count> buttons_{};
    std::array<UiAnimation, choice_count> animations_{};
    std::optional<Vec2d> pointer_;
    std::optional<UnsavedDialogChoice> pressed_choice_;
    UnsavedDialogChoice focused_choice_{UnsavedDialogChoice::save};
    std::string board_name_{"Untitled"};
};

} // namespace sawer
