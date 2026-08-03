#include "canvas/Camera.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using Catch::Approx;

TEST_CASE("camera coordinate conversion round trips")
{
    sawer::Camera camera{1920.0, 1080.0};
    camera.pan_by_screen_delta({-720.0, 240.0});
    camera.zoom_at({300.0, 200.0}, 4.0);

    const sawer::Vec2d world{812.25, -407.75};
    const auto screen = camera.world_to_screen(world);
    const auto round_trip = camera.screen_to_world(screen);

    REQUIRE(round_trip.x == Approx(world.x));
    REQUIRE(round_trip.y == Approx(world.y));
}

TEST_CASE("zoom remains anchored beneath the cursor")
{
    sawer::Camera camera{1600.0, 900.0};
    const sawer::Vec2d cursor{275.0, 190.0};
    const auto before = camera.screen_to_world(cursor);

    camera.zoom_at(cursor, 3.5);

    const auto after = camera.screen_to_world(cursor);
    REQUIRE(after.x == Approx(before.x).margin(1.0e-9));
    REQUIRE(after.y == Approx(before.y).margin(1.0e-9));
}

TEST_CASE("camera clamps navigation to finite board bounds")
{
    sawer::Camera camera{1920.0, 1080.0};
    camera.pan_by_screen_delta({-1.0e12, -1.0e12});

    const auto visible = camera.visible_world_bounds();
    REQUIRE(visible.max_x <= sawer::Camera::board_half_extent);
    REQUIRE(visible.max_y <= sawer::Camera::board_half_extent);

    camera.pan_by_screen_delta({1.0e12, 1.0e12});
    const auto opposite = camera.visible_world_bounds();
    REQUIRE(opposite.min_x >= -sawer::Camera::board_half_extent);
    REQUIRE(opposite.min_y >= -sawer::Camera::board_half_extent);
}

TEST_CASE("zoom is clamped to the configured range")
{
    sawer::Camera camera{3840.0, 2160.0};

    camera.zoom_at({1920.0, 1080.0}, 1.0e-20);
    REQUIRE(camera.zoom() == Approx(camera.minimum_zoom()));
    REQUIRE(camera.minimum_zoom() == Approx(sawer::Camera::absolute_min_zoom));
    REQUIRE(camera.zoom() == Approx(0.10));

    camera.zoom_at({1920.0, 1080.0}, 1.0e20);
    REQUIRE(camera.zoom() == Approx(sawer::Camera::maximum_zoom));
    REQUIRE(camera.zoom() == Approx(5.0));
}

TEST_CASE("dragging pans the board with the pointer")
{
    sawer::Camera camera{1000.0, 800.0};
    const auto world_before = camera.screen_to_world({500.0, 400.0});

    camera.pan_by_screen_delta({100.0, -50.0});

    const auto world_after = camera.screen_to_world({500.0, 400.0});
    REQUIRE(world_after.x == Approx(world_before.x - 100.0));
    REQUIRE(world_after.y == Approx(world_before.y + 50.0));
}

TEST_CASE("framing places world bounds inside the requested screen area")
{
    sawer::Camera camera{1280.0, 720.0};
    const sawer::Aabb content{-400.0, -100.0, 600.0, 300.0};
    const sawer::Vec2d screen_min{72.0, 68.0};
    const sawer::Vec2d screen_max{1268.0, 660.0};

    camera.frame_bounds(content, screen_min, screen_max, 24.0);

    const auto top_left =
        camera.world_to_screen({content.min_x, content.min_y});
    const auto bottom_right =
        camera.world_to_screen({content.max_x, content.max_y});
    REQUIRE(top_left.x >= screen_min.x + 24.0);
    REQUIRE(top_left.y >= screen_min.y + 24.0);
    REQUIRE(bottom_right.x <= screen_max.x - 24.0);
    REQUIRE(bottom_right.y <= screen_max.y - 24.0);
    REQUIRE(
        (top_left.x + bottom_right.x) * 0.5
        == Approx((screen_min.x + screen_max.x) * 0.5));
    REQUIRE(
        (top_left.y + bottom_right.y) * 0.5
        == Approx((screen_min.y + screen_max.y) * 0.5));
}

} // namespace
