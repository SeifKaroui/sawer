#include "geometry/StrokeProcessing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using Catch::Approx;

TEST_CASE("point filtering rejects samples below the distance threshold")
{
    std::vector<sawer::Vec2d> points;
    REQUIRE(sawer::append_filtered_point(points, {0.0, 0.0}, 1.0));
    REQUIRE_FALSE(sawer::append_filtered_point(points, {0.5, 0.0}, 1.0));
    REQUIRE(sawer::append_filtered_point(points, {1.0, 0.0}, 1.0));
    REQUIRE(points.size() == 2U);
}

TEST_CASE("stroke simplification removes redundant collinear points")
{
    const std::vector<sawer::Vec2d> points{
        {0.0, 0.0},
        {1.0, 0.01},
        {2.0, -0.01},
        {3.0, 0.0},
    };
    const auto simplified = sawer::simplify_stroke(points, 0.05);

    REQUIRE(simplified.size() == 2U);
    REQUIRE(simplified.front() == points.front());
    REQUIRE(simplified.back() == points.back());
}

TEST_CASE("stroke simplification has bounded work on pathological zigzags")
{
    std::vector<sawer::Vec2d> points;
    points.reserve(50'000U);
    for (std::size_t index = 0U; index < 50'000U; ++index) {
        points.push_back({
            static_cast<double>(index),
            index % 2U == 0U ? -10.0 : 10.0,
        });
    }
    const auto simplified = sawer::simplify_stroke(points, 0.01);

    REQUIRE(simplified.front() == points.front());
    REQUIRE(simplified.back() == points.back());
    REQUIRE(simplified.size() > 40'000U);
}

TEST_CASE("completion smoothing removes cadence-scale pointer jitter")
{
    std::vector<sawer::Vec2d> points;
    for (std::size_t index = 0U; index < 21U; ++index) {
        points.push_back({
            static_cast<double>(index),
            index % 2U == 0U ? -1.0 : 1.0,
        });
    }
    const auto first = points.front();
    const auto last = points.back();

    sawer::smooth_stroke_points(points, 3U);

    REQUIRE(points.front() == first);
    REQUIRE(points.back() == last);
    for (std::size_t index = 3U; index + 3U < points.size(); ++index) {
        REQUIRE(std::abs(points[index].y) < 0.15);
    }
}

TEST_CASE("completion smoothing preserves straight pointer motion")
{
    std::vector<sawer::Vec2d> points;
    for (std::size_t index = 0U; index < 21U; ++index) {
        points.push_back({
            static_cast<double>(index),
            static_cast<double>(index) * 2.0,
        });
    }
    const auto original = points;

    sawer::smooth_stroke_points(points, 3U);

    REQUIRE(points == original);
}

TEST_CASE("live smoothing removes jitter before pointer release")
{
    std::vector<sawer::Vec2d> points;
    for (std::size_t index = 0U; index < 7U; ++index) {
        points.push_back({
            static_cast<double>(index),
            index % 2U == 0U ? -1.0 : 1.0,
        });
    }

    const auto smoothed =
        sawer::trailing_smoothed_stroke_point(points, 3U);
    REQUIRE(smoothed.x == Approx(3.0));
    REQUIRE(std::abs(smoothed.y) < 0.15);
}

TEST_CASE("disabled live smoothing follows the latest pointer exactly")
{
    const std::vector<sawer::Vec2d> points{
        {0.0, 0.0},
        {4.0, 3.0},
    };
    REQUIRE(sawer::trailing_smoothed_stroke_point(points, 0U)
        == points.back());
}

TEST_CASE("smooth stroke tail preserves direction and reaches release point")
{
    std::vector<sawer::Vec2d> points{{0.0, 0.0}, {1.0, 0.0}};
    sawer::append_smooth_tail(points, {3.0, 2.0}, 0.01);

    REQUIRE(points.size() > 3U);
    REQUIRE(points.back() == (sawer::Vec2d{3.0, 2.0}));
    REQUIRE(points[2].x > 1.0);
    REQUIRE(points[2].y >= 0.0);
    REQUIRE(points[2].y < points[2].x - 1.0);
}

TEST_CASE("smooth stroke tail keeps a reversal as a sharp cusp")
{
    std::vector<sawer::Vec2d> points{{0.0, 0.0}, {1.0, 0.0}};
    sawer::append_smooth_tail(points, {-1.0, 0.0}, 0.01);

    REQUIRE(points.size() == 3U);
    REQUIRE(points.back() == (sawer::Vec2d{-1.0, 0.0}));
}

TEST_CASE("stroke curve interpolation removes visible polygon corners")
{
    const std::vector<sawer::Vec2d> points{
        {0.0, 0.0}, {10.0, 0.0}, {20.0, 10.0}, {30.0, 10.0}};
    const auto curve = sawer::interpolate_stroke_curve(points, 0.05);

    REQUIRE(curve.size() > points.size());
    REQUIRE(curve.front() == points.front());
    REQUIRE(curve.back() == points.back());
    REQUIRE(std::ranges::find(curve, points[1U]) != curve.end());
    REQUIRE(std::ranges::find(curve, points[2U]) != curve.end());
}

TEST_CASE("stroke curve interpolation leaves straight paths compact")
{
    const std::vector<sawer::Vec2d> points{
        {0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}, {30.0, 0.0}};
    const auto curve = sawer::interpolate_stroke_curve(points, 0.05);
    REQUIRE(curve == points);
}

TEST_CASE("line constraints snap to 45 degree increments")
{
    const auto endpoint = sawer::constrain_line_endpoint(
        {0.0, 0.0}, {10.0, 2.0}, true);
    REQUIRE(endpoint.y == Approx(0.0).margin(1.0e-9));
    REQUIRE(std::hypot(endpoint.x, endpoint.y)
        == Approx(std::hypot(10.0, 2.0)));
}

TEST_CASE("shape constraints produce equal dimensions")
{
    const auto endpoint = sawer::constrain_shape_endpoint(
        {10.0, 10.0}, {-20.0, 25.0}, true);
    REQUIRE(std::abs(endpoint.x - 10.0)
        == Approx(std::abs(endpoint.y - 10.0)));
    REQUIRE(endpoint.x < 10.0);
    REQUIRE(endpoint.y > 10.0);
}

TEST_CASE("drawing constraints remain inside finite board bounds")
{
    const auto line = sawer::constrain_line_endpoint(
        {sawer::board_half_extent - 2.0, 0.0},
        {sawer::board_half_extent, 100.0},
        true);
    REQUIRE(line.x <= sawer::board_half_extent);
    REQUIRE(line.y <= sawer::board_half_extent);

    const auto square = sawer::constrain_shape_endpoint(
        {sawer::board_half_extent - 5.0, 0.0},
        {sawer::board_half_extent, 100.0},
        true);
    REQUIRE(square.x == sawer::board_half_extent);
    REQUIRE(square.y == 5.0);

    const auto unclamped = sawer::constrain_line_endpoint(
        {}, {sawer::board_half_extent * 2.0, 0.0}, false);
    REQUIRE(unclamped.x == sawer::board_half_extent);
}

TEST_CASE("incremental stroke curve retains its preview on completion")
{
    sawer::IncrementalStrokeCurve curve;
    std::vector<sawer::Vec2d> points{{0.0, 0.0}};
    curve.reset(points.front(), 0.01);
    REQUIRE(curve.push({8.0, 0.0}, points));
    const auto preview = points;
    REQUIRE(curve.finish({8.0, 0.0}, points) == false);
    REQUIRE(points == preview);
    REQUIRE(curve.finished());
}

TEST_CASE("incremental stroke curve preserves the release endpoint")
{
    sawer::IncrementalStrokeCurve curve;
    std::vector<sawer::Vec2d> points{{0.0, 0.0}};
    curve.reset(points.front(), 0.01);
    static_cast<void>(curve.push({4.0, 1.0}, points));
    REQUIRE(curve.finish({6.0, -2.0}, points));
    REQUIRE(points.back() == (sawer::Vec2d{6.0, -2.0}));
}

} // namespace
