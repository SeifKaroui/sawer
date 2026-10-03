#include "renderer/BackgroundGrid.hpp"
#include "renderer/BackgroundParameters.hpp"
#include "renderer/StrokeDetail.hpp"
#include "canvas/Camera.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>

TEST_CASE("background grids remain inside their geometry budget")
{
    constexpr std::size_t budget = 100'000U;
    constexpr std::array patterns{
        sawer::BackgroundGridPattern::solid,
        sawer::BackgroundGridPattern::dot,
        sawer::BackgroundGridPattern::square,
        sawer::BackgroundGridPattern::graph,
        sawer::BackgroundGridPattern::hybrid,
        sawer::BackgroundGridPattern::diamond,
        sawer::BackgroundGridPattern::wide_rule,
        sawer::BackgroundGridPattern::triangle,
        sawer::BackgroundGridPattern::narrow_rule,
    };
    constexpr std::array viewports{
        std::array{3840.0, 2160.0},
        std::array{7680.0, 4320.0},
        std::array{2160.0, 3840.0},
    };
    constexpr std::array zooms{0.01, 0.1, 1.0, 8.0, 64.0};

    for (const auto pattern : patterns) {
        for (const auto viewport : viewports) {
            for (const double zoom : zooms) {
                CAPTURE(pattern, viewport[0], viewport[1], zoom);
                const auto plan = sawer::plan_background_grid(
                    pattern, viewport[0], viewport[1], zoom, 50.0, budget);
                REQUIRE(plan.estimated_vertices <= budget);
                if (pattern != sawer::BackgroundGridPattern::solid) {
                    REQUIRE(plan.world_spacing >= 50.0);
                    REQUIRE(plan.world_spacing / 50.0
                        == Catch::Approx(std::round(plan.world_spacing / 50.0)));
                }
                REQUIRE(plan.density >= 0.55);
                REQUIRE(plan.density <= 1.0);
            }
        }
    }
}

TEST_CASE("background patterns use fixed world spacing across normal zoom levels")
{
    for (const auto pattern : {sawer::BackgroundGridPattern::dot,
             sawer::BackgroundGridPattern::square, sawer::BackgroundGridPattern::graph,
             sawer::BackgroundGridPattern::hybrid, sawer::BackgroundGridPattern::diamond,
             sawer::BackgroundGridPattern::wide_rule, sawer::BackgroundGridPattern::triangle,
             sawer::BackgroundGridPattern::narrow_rule}) {
        for (const auto viewport : {std::array{1920.0, 1080.0},
                 std::array{3840.0, 2160.0}}) {
            for (const double zoom : {0.35, 0.5, 0.99, 1.0, 1.01, 2.0, 5.0}) {
                CAPTURE(pattern, viewport[0], viewport[1], zoom);
                const auto plan = sawer::plan_background_grid(pattern, viewport[0], viewport[1], zoom);
                REQUIRE(plan.world_spacing == 57.5);
                REQUIRE(plan.world_spacing * zoom == Catch::Approx(50.0 * 1.15 * zoom));
                REQUIRE(plan.estimated_vertices <= 250'000U);
            }
        }
    }
}

TEST_CASE("grid intersections stay attached to drawings through cursor zoom and pan")
{
    sawer::Camera camera{1920.0, 1080.0};
    const sawer::Vec2d intersection{115.0, -172.5};
    const auto cursor = camera.world_to_screen(intersection);
    for (const double factor : {2.0, 0.5, 0.5, 4.0}) {
        camera.zoom_at(cursor, factor);
        const auto plan = sawer::plan_background_grid(
            sawer::BackgroundGridPattern::dot, 1920.0, 1080.0, camera.zoom());
        REQUIRE(plan.world_spacing == 57.5);
        REQUIRE(std::remainder(intersection.x, plan.world_spacing) == 0.0);
        REQUIRE(std::remainder(intersection.y, plan.world_spacing) == 0.0);
        const auto screen = camera.world_to_screen(intersection);
        REQUIRE(screen.x == Catch::Approx(cursor.x));
        REQUIRE(screen.y == Catch::Approx(cursor.y));
        const auto neighbor = camera.world_to_screen(
            {intersection.x + plan.world_spacing, intersection.y});
        REQUIRE(neighbor.x - screen.x == Catch::Approx(57.5 * camera.zoom()));
    }
    camera.pan_by_screen_delta({137.0, -91.0});
    const auto panned = camera.world_to_screen(intersection);
    REQUIRE(panned.x == Catch::Approx(cursor.x + 137.0));
    REQUIRE(panned.y == Catch::Approx(cursor.y - 91.0));
}

TEST_CASE("distant grids skip detail only when their geometry exceeds the budget")
{
    constexpr std::size_t budget = 100'000U;
    const auto distant = sawer::plan_background_grid(
        sawer::BackgroundGridPattern::dot, 3840.0, 2160.0, 0.1, 50.0, budget);
    REQUIRE(distant.world_spacing > 50.0);
    REQUIRE(distant.estimated_vertices <= budget);
    REQUIRE(sawer::estimate_background_grid_vertices(
        sawer::BackgroundGridPattern::dot, 3840.0, 2160.0,
        distant.world_spacing * 0.1 / 5.0) > budget);

    // Less dense line patterns keep their original lattice even at minimum zoom.
    const auto lines = sawer::plan_background_grid(
        sawer::BackgroundGridPattern::square, 3840.0, 2160.0, 0.1, 50.0, budget);
    REQUIRE(lines.world_spacing == 50.0);
}

TEST_CASE("graph and rule estimates account for their denser subdivisions")
{
    constexpr double width = 1920.0;
    constexpr double height = 1080.0;
    constexpr double spacing = 50.0;
    REQUIRE(sawer::estimate_background_grid_vertices(
                sawer::BackgroundGridPattern::graph,
                width,
                height,
                spacing)
            > sawer::estimate_background_grid_vertices(
                sawer::BackgroundGridPattern::square,
                width,
                height,
                spacing));
    REQUIRE(sawer::estimate_background_grid_vertices(
                sawer::BackgroundGridPattern::narrow_rule,
                width,
                height,
                spacing)
            > sawer::estimate_background_grid_vertices(
                sawer::BackgroundGridPattern::wide_rule,
                width,
                height,
                spacing));
}

TEST_CASE("stroke display detail retains endpoints and bounds screen error")
{
    for (const double zoom : {0.01, 0.1, 0.27, 1.0, 8.0, 64.0}) {
        const double tolerance = sawer::stroke_detail_tolerance(zoom);
        REQUIRE(tolerance * zoom <= 0.2500000001);
        REQUIRE(tolerance * zoom > 0.20);
        std::vector<sawer::Vec2d> points;
        for (int index = 0; index < 10'001; ++index) {
            points.push_back({999'000.0 + index * 0.0001,
                -999'000.0 + std::sin(index * 0.01) * 0.001});
        }
        const auto original = points;
        std::vector<sawer::Vec2d> result;
        sawer::simplify_stroke_for_rendering(points, tolerance, result);
        REQUIRE(result.front() == points.front());
        REQUIRE(result.back() == points.back());
        REQUIRE(result.size() < points.size() / 10U);
        REQUIRE(points == original);
        // Check the geometric guarantee against every source sample, rather
        // than duplicating the simplifier's retain/discard condition.
        for (const auto point : points) {
            double nearest = 1.0e20;
            for (std::size_t index = 1; index < result.size(); ++index) {
                const auto a = result[index - 1U];
                const auto b = result[index];
                const double dx = b.x - a.x;
                const double dy = b.y - a.y;
                const double length = dx * dx + dy * dy;
                const double t = length == 0.0 ? 0.0 : std::clamp(
                    ((point.x - a.x) * dx + (point.y - a.y) * dy) / length, 0.0, 1.0);
                nearest = std::min(nearest, std::hypot(point.x - a.x - t * dx,
                    point.y - a.y - t * dy));
            }
            REQUIRE(nearest * zoom <= 0.250000001);
        }
    }
}

TEST_CASE("stroke display detail preserves turns closed strokes and tiny marks")
{
    const std::vector<sawer::Vec2d> turns{{0, 0}, {5, 0}, {0, 0}, {0, 5}, {0, 0}};
    std::vector<sawer::Vec2d> result;
    sawer::simplify_stroke_for_rendering(turns, 0.25, result);
    REQUIRE(result == turns);
    sawer::simplify_stroke_for_rendering(std::array<sawer::Vec2d, 2>{{{0, 0}, {0.001, 0}}}, 25.0, result);
    REQUIRE(result.size() == 2U);
    REQUIRE(result.back().x == 0.001);
    sawer::simplify_stroke_for_rendering(std::array<sawer::Vec2d, 1>{{{3, 4}}}, 25.0, result);
    REQUIRE(result == std::vector<sawer::Vec2d>{{3, 4}});
    sawer::simplify_stroke_for_rendering({}, 0.25, result);
    REQUIRE(result.empty());
}

TEST_CASE("stroke display detail has stable conservative zoom buckets")
{
    const double high = std::exp2(-1.0 / 4.0);
    REQUIRE(sawer::stroke_detail_bucket(high) == sawer::stroke_detail_bucket(high * 0.999));
    REQUIRE(sawer::stroke_detail_bucket(high * 1.001) > sawer::stroke_detail_bucket(high));
    REQUIRE(sawer::stroke_detail_tolerance(high) == sawer::stroke_detail_tolerance(high * 0.999));
}

TEST_CASE("background shader parameters preserve lattice phase at extreme coordinates")
{
    for (const auto pattern : {sawer::BackgroundGridPattern::dot, sawer::BackgroundGridPattern::graph,
            sawer::BackgroundGridPattern::diamond, sawer::BackgroundGridPattern::triangle,
            sawer::BackgroundGridPattern::narrow_rule}) {
        for (const double zoom : {0.001, 0.27, 1.0, 64.0}) {
            const sawer::Vec2d camera{999'000.125, -998'999.875};
            const auto p = sawer::background_parameters(pattern, {240, 242, 247, 255}, std::nullopt,
                camera, {1920, 1080}, zoom, {3840, 2160});
            REQUIRE(p.scale[0] == 2.0F);
            REQUIRE(p.scale[1] == 2.0F);
            REQUIRE(std::abs(p.phase[0]) <= p.scale[2] * 2.500001F);
            REQUIRE(std::abs(p.phase[1]) <= p.scale[2] * 2.500001F);
            const double nearest_x = std::round(camera.x / (static_cast<double>(p.scale[2]) / zoom))
                * (static_cast<double>(p.scale[2]) / zoom);
            const double shifted = (nearest_x - camera.x) * zoom + p.phase[0];
            REQUIRE(std::abs(std::remainder(shifted, static_cast<double>(p.scale[2]))) < 0.01);
        }
    }
}
