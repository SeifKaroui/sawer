#pragma once

#include "geometry/Geometry.hpp"

namespace sawer {

using WorldBounds = Aabb;

class Camera final {
public:
    static constexpr double board_half_extent = sawer::board_half_extent;
    static constexpr double absolute_min_zoom = 0.10;
    static constexpr double maximum_zoom = 5.0;

    Camera(double viewport_width, double viewport_height);

    void set_viewport(double width, double height);
    void pan_by_screen_delta(Vec2d delta);
    void zoom_at(Vec2d screen_anchor, double factor);
    void frame_bounds(
        WorldBounds bounds,
        Vec2d screen_min,
        Vec2d screen_max,
        double padding_pixels = 24.0);

    [[nodiscard]] Vec2d world_to_screen(Vec2d world) const noexcept;
    [[nodiscard]] Vec2d screen_to_world(Vec2d screen) const noexcept;
    [[nodiscard]] WorldBounds visible_world_bounds() const noexcept;
    [[nodiscard]] Vec2d position() const noexcept;
    [[nodiscard]] Vec2d viewport() const noexcept;
    [[nodiscard]] double zoom() const noexcept;
    [[nodiscard]] double minimum_zoom() const noexcept;

private:
    void clamp();

    Vec2d position_{};
    Vec2d viewport_{};
    double zoom_{1.0};
};

} // namespace sawer
