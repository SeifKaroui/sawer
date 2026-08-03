#include "ui/UnsavedDialog.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sawer {
namespace {

double approach(
    const double current,
    const double target,
    const double elapsed_seconds) noexcept
{
    const double amount = 1.0 - std::exp(
        -std::max(0.0, elapsed_seconds) * 18.0);
    return current + (target - current) * amount;
}

} // namespace

void UnsavedDialog::open(
    const double viewport_width,
    const double viewport_height,
    const double display_scale,
    std::string board_name)
{
    board_name_ = std::move(board_name);
    visible_ = true;
    open_seconds_ = 0.0;
    pointer_.reset();
    pressed_choice_.reset();
    focused_choice_ = UnsavedDialogChoice::save;
    animations_ = {};
    relayout(viewport_width, viewport_height, display_scale);
}

void UnsavedDialog::close() noexcept
{
    visible_ = false;
    pointer_.reset();
    pressed_choice_.reset();
    animations_ = {};
}

void UnsavedDialog::relayout(
    const double viewport_width,
    const double viewport_height,
    const double display_scale) noexcept
{
    viewport_width_ = std::max(1.0, viewport_width);
    viewport_height_ = std::max(1.0, viewport_height);
    const double available_scale = std::max(
        0.72,
        std::min(viewport_width_ / 560.0, viewport_height_ / 340.0));
    scale_ = std::clamp(display_scale, 0.72, available_scale);

    const double outer_margin = 16.0 * scale_;
    const double width = std::min(
        520.0 * scale_,
        std::max(1.0, viewport_width_ - outer_margin * 2.0));
    const double height = std::min(
        252.0 * scale_,
        std::max(1.0, viewport_height_ - outer_margin * 2.0));
    bounds_ = {
        (viewport_width_ - width) * 0.5,
        (viewport_height_ - height) * 0.5,
        width,
        height,
    };

    const double content_margin = 24.0 * scale_;
    const double gap = 10.0 * scale_;
    const double button_height = 40.0 * scale_;
    const double button_width =
        (width - content_margin * 2.0 - gap * 2.0) / 3.0;
    const double button_y =
        bounds_.y + bounds_.height - content_margin - button_height;
    buttons_[index(UnsavedDialogChoice::cancel)] = {
        bounds_.x + content_margin,
        button_y,
        button_width,
        button_height,
    };
    buttons_[index(UnsavedDialogChoice::discard)] = {
        bounds_.x + content_margin + button_width + gap,
        button_y,
        button_width,
        button_height,
    };
    buttons_[index(UnsavedDialogChoice::save)] = {
        bounds_.x + content_margin + (button_width + gap) * 2.0,
        button_y,
        button_width,
        button_height,
    };
}

void UnsavedDialog::set_pointer(const Vec2d point) noexcept
{
    pointer_ = point;
}

void UnsavedDialog::clear_pointer() noexcept
{
    pointer_.reset();
    pressed_choice_.reset();
}

void UnsavedDialog::pointer_down(const Vec2d point) noexcept
{
    pointer_ = point;
    pressed_choice_ = choice_at(point);
    if (pressed_choice_.has_value()) {
        focused_choice_ = *pressed_choice_;
    }
}

std::optional<UnsavedDialogChoice> UnsavedDialog::pointer_up(
    const Vec2d point) noexcept
{
    pointer_ = point;
    const auto released = choice_at(point);
    const bool activates = pressed_choice_.has_value()
        && released == pressed_choice_;
    const auto result = activates ? released : std::nullopt;
    pressed_choice_.reset();
    return result;
}

void UnsavedDialog::focus_next(const bool reverse) noexcept
{
    constexpr std::size_t count = index(UnsavedDialogChoice::count);
    const std::size_t current = index(focused_choice_);
    focused_choice_ = static_cast<UnsavedDialogChoice>(
        reverse
        ? (current + count - 1U) % count
        : (current + 1U) % count);
}

UnsavedDialogChoice UnsavedDialog::focused_choice() const noexcept
{
    return focused_choice_;
}

void UnsavedDialog::tick(const double elapsed_seconds) noexcept
{
    if (!visible_) {
        return;
    }
    open_seconds_ += std::max(0.0, elapsed_seconds);
    const auto hovered = hovered_choice();
    for (std::size_t choice = 0U; choice < animations_.size(); ++choice) {
        const auto value = static_cast<UnsavedDialogChoice>(choice);
        animations_[choice].hover = approach(
            animations_[choice].hover,
            hovered == value ? 1.0 : 0.0,
            elapsed_seconds);
        animations_[choice].press = approach(
            animations_[choice].press,
            pressed_choice_ == value ? 1.0 : 0.0,
            elapsed_seconds);
        animations_[choice].selected = approach(
            animations_[choice].selected,
            focused_choice_ == value ? 1.0 : 0.0,
            elapsed_seconds);
    }
}

bool UnsavedDialog::visible() const noexcept
{
    return visible_;
}

bool UnsavedDialog::animating() const noexcept
{
    if (!visible_ || open_seconds_ < 0.18) {
        return visible_;
    }
    const auto near_target = [](const double value, const double target) {
        return std::abs(value - target) <= 0.01;
    };
    const auto hovered = hovered_choice();
    for (std::size_t choice = 0U; choice < animations_.size(); ++choice) {
        const auto value = static_cast<UnsavedDialogChoice>(choice);
        if (!near_target(
                animations_[choice].hover,
                hovered == value ? 1.0 : 0.0)
            || !near_target(
                animations_[choice].press,
                pressed_choice_ == value ? 1.0 : 0.0)
            || !near_target(
                animations_[choice].selected,
                focused_choice_ == value ? 1.0 : 0.0)) {
            return true;
        }
    }
    return false;
}

double UnsavedDialog::reveal() const noexcept
{
    const double amount = std::clamp(open_seconds_ / 0.16, 0.0, 1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

double UnsavedDialog::hover(const UnsavedDialogChoice choice) const noexcept
{
    return animations_[index(choice)].hover;
}

double UnsavedDialog::press(const UnsavedDialogChoice choice) const noexcept
{
    return animations_[index(choice)].press;
}

std::optional<UnsavedDialogChoice> UnsavedDialog::hovered_choice()
    const noexcept
{
    return pointer_.has_value() ? choice_at(*pointer_) : std::nullopt;
}

UiRect UnsavedDialog::bounds() const noexcept
{
    return bounds_;
}

UiRect UnsavedDialog::button_bounds(
    const UnsavedDialogChoice choice) const noexcept
{
    return buttons_[index(choice)];
}

double UnsavedDialog::scale() const noexcept
{
    return scale_;
}

std::string_view UnsavedDialog::board_name() const noexcept
{
    return board_name_;
}

std::optional<UnsavedDialogChoice> UnsavedDialog::choice_at(
    const Vec2d point) const noexcept
{
    for (std::size_t choice = 0U; choice < buttons_.size(); ++choice) {
        if (buttons_[choice].contains(point)) {
            return static_cast<UnsavedDialogChoice>(choice);
        }
    }
    return std::nullopt;
}

} // namespace sawer
