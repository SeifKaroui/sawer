#include "canvas/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace sawer {
namespace {

constexpr double board_size = Camera::board_half_extent * 2.0;

double clamp_axis(
    const double value,
    const double viewport_extent,
    const double zoom)
{
    const double half_visible = viewport_extent / (zoom * 2.0);
    if (half_visible >= Camera::board_half_extent) {
        return 0.0;
    }

    return std::clamp(
        value,
        -Camera::board_half_extent + half_visible,
        Camera::board_half_extent - half_visible);
}

} // namespace

Camera::Camera(
    const double viewport_width,
    const double viewport_height)
{
    set_viewport(viewport_width, viewport_height);
}

void Camera::set_viewport(const double width, const double height)
{
    viewport_.x = std::max(width, 1.0);
    viewport_.y = std::max(height, 1.0);
    zoom_ = std::clamp(zoom_, minimum_zoom(), maximum_zoom);
    clamp();
}

void Camera::pan_by_screen_delta(const Vec2d delta)
{
    position_.x -= delta.x / zoom_;
    position_.y -= delta.y / zoom_;
    clamp();
}

void Camera::zoom_at(const Vec2d screen_anchor, const double factor)
{
    if (!std::isfinite(factor) || factor <= 0.0) {
        return;
    }

    const Vec2d anchored_world = screen_to_world(screen_anchor);
    zoom_ = std::clamp(zoom_ * factor, minimum_zoom(), maximum_zoom);

    const Vec2d viewport_center{viewport_.x * 0.5, viewport_.y * 0.5};
    position_.x =
        anchored_world.x - (screen_anchor.x - viewport_center.x) / zoom_;
    position_.y =
        anchored_world.y - (screen_anchor.y - viewport_center.y) / zoom_;
    clamp();
}

void Camera::frame_bounds(
    const WorldBounds bounds,
    const Vec2d screen_min,
    const Vec2d screen_max,
    const double padding_pixels)
{
    if (!std::isfinite(bounds.min_x) || !std::isfinite(bounds.min_y)
        || !std::isfinite(bounds.max_x) || !std::isfinite(bounds.max_y)
        || bounds.max_x < bounds.min_x || bounds.max_y < bounds.min_y) {
        return;
    }
    const double padding =
        std::max(std::isfinite(padding_pixels) ? padding_pixels : 0.0, 0.0);
    const double usable_width = std::max(
        screen_max.x - screen_min.x - padding * 2.0, 1.0);
    const double usable_height = std::max(
        screen_max.y - screen_min.y - padding * 2.0, 1.0);
    const double world_width = std::max(bounds.max_x - bounds.min_x, 1.0);
    const double world_height = std::max(bounds.max_y - bounds.min_y, 1.0);
    zoom_ = std::clamp(
        std::min(usable_width / world_width, usable_height / world_height),
        minimum_zoom(),
        maximum_zoom);

    const Vec2d world_center{
        (bounds.min_x + bounds.max_x) * 0.5,
        (bounds.min_y + bounds.max_y) * 0.5,
    };
    const Vec2d screen_center{
        (screen_min.x + screen_max.x) * 0.5,
        (screen_min.y + screen_max.y) * 0.5,
    };
    const Vec2d viewport_center{viewport_.x * 0.5, viewport_.y * 0.5};
    position_.x =
        world_center.x - (screen_center.x - viewport_center.x) / zoom_;
    position_.y =
        world_center.y - (screen_center.y - viewport_center.y) / zoom_;
    clamp();
}

Vec2d Camera::world_to_screen(const Vec2d world) const noexcept
{
    return {
        (world.x - position_.x) * zoom_ + viewport_.x * 0.5,
        (world.y - position_.y) * zoom_ + viewport_.y * 0.5,
    };
}

Vec2d Camera::screen_to_world(const Vec2d screen) const noexcept
{
    return {
        position_.x + (screen.x - viewport_.x * 0.5) / zoom_,
        position_.y + (screen.y - viewport_.y * 0.5) / zoom_,
    };
}

WorldBounds Camera::visible_world_bounds() const noexcept
{
    const Vec2d top_left = screen_to_world({0.0, 0.0});
    const Vec2d bottom_right = screen_to_world(viewport_);
    return {
        top_left.x,
        top_left.y,
        bottom_right.x,
        bottom_right.y,
    };
}

Vec2d Camera::position() const noexcept
{
    return position_;
}

Vec2d Camera::viewport() const noexcept
{
    return viewport_;
}

double Camera::zoom() const noexcept
{
    return zoom_;
}

double Camera::minimum_zoom() const noexcept
{
    return std::max({
        absolute_min_zoom,
        viewport_.x / board_size,
        viewport_.y / board_size,
    });
}

void Camera::clamp()
{
    position_.x = clamp_axis(position_.x, viewport_.x, zoom_);
    position_.y = clamp_axis(position_.y, viewport_.y, zoom_);
}

} // namespace sawer
