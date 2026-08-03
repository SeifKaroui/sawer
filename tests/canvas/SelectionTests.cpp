#include "canvas/Selection.hpp"
#include "document/Command.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

namespace {

sawer::Object line_object(
    const std::uint64_t id,
    const std::int64_t z,
    const sawer::Vec2d first,
    const sawer::Vec2d second)
{
    return sawer::Object::make_line(
        sawer::ObjectId::from_u64(id), z, {first, second},
        {
            .stroke = {225U, 232U, 244U, 255U},
            .fill = std::nullopt,
            .stroke_width = 4.0,
        });
}

} // namespace

TEST_CASE("hit testing chooses the topmost nearby segment")
{
    sawer::Document document;
    REQUIRE(document.insert(line_object(1U, 1, {-20.0, 0.0}, {20.0, 0.0})));
    REQUIRE(document.insert(line_object(2U, 5, {-20.0, 0.0}, {20.0, 0.0})));
    REQUIRE(document.insert(line_object(3U, 8, {5000.0, 0.0}, {5100.0, 0.0})));

    REQUIRE(sawer::hit_test(document, {0.0, 3.0}, 2.0)
            == sawer::ObjectId::from_u64(2U));
    REQUIRE_FALSE(sawer::hit_test(document, {0.0, 20.0}, 2.0).has_value());
}

TEST_CASE("long stroke hit testing queries only nearby segments")
{
    sawer::Stroke stroke;
    stroke.points.reserve(100'000U);
    for (std::size_t index = 0U; index < 100'000U; ++index) {
        stroke.points.push_back({
            -500'000.0 + static_cast<double>(index) * 10.0,
            index % 2U == 0U ? 0.0 : 1.0,
        });
    }
    const auto id = sawer::ObjectId::from_u64(4U);
    sawer::Document document;
    REQUIRE(document.insert(sawer::Object::make_stroke(
        id, 0, std::move(stroke))));

    std::vector<std::uint32_t> nearby_segments;
    document.query_stroke_segments(
        id, {-10.0, -10.0, 10.0, 10.0}, nearby_segments);
    REQUIRE(nearby_segments.size() < 500U);
    REQUIRE(sawer::hit_test(document, {0.0, 0.5}, 1.0) == id);
    REQUIRE_FALSE(
        sawer::hit_test(document, {0.0, 30.0}, 1.0).has_value());
}

TEST_CASE("marquee selection uses indexed object bounds")
{
    sawer::Document document;
    REQUIRE(document.insert(line_object(1U, 0, {0.0, 0.0}, {20.0, 20.0})));
    REQUIRE(document.insert(line_object(2U, 1, {100.0, 100.0}, {120.0, 120.0})));

    const auto ids = sawer::marquee_hit_test(
        document, {-10.0, -10.0, 30.0, 30.0});
    REQUIRE(ids.size() == 1U);
    REQUIRE(ids.front() == sawer::ObjectId::from_u64(1U));
}

TEST_CASE("selection removes duplicate ids and previews use stable lookup")
{
    const auto first = sawer::ObjectId::from_u64(1U);
    const auto second = sawer::ObjectId::from_u64(2U);
    sawer::Selection selection;
    selection.select({second, first, second, first});

    REQUIRE(selection.ids() == std::vector<sawer::ObjectId>{first, second});

    const sawer::SelectionPreview preview{{second, first, second}};
    REQUIRE(preview.contains(first));
    REQUIRE(preview.contains(second));
    REQUIRE_FALSE(preview.contains(sawer::ObjectId::from_u64(3U)));
}

TEST_CASE("line endpoints and shape corners can be edited")
{
    auto line = line_object(1U, 0, {0.0, 0.0}, {20.0, 0.0});
    sawer::resize_object(
        line, sawer::SelectionHandle::line_end, {40.0, 15.0});
    REQUIRE(std::get<sawer::Line>(line.geometry).end == sawer::Vec2d{40.0, 15.0});

    auto rectangle = sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(2U), 1,
        {{0.0, 0.0}, {20.0, 10.0}});
    sawer::resize_object(
        rectangle, sawer::SelectionHandle::bottom_right, {50.0, 30.0});
    REQUIRE(std::get<sawer::RectangleShape>(rectangle.geometry).second
            == sawer::Vec2d{50.0, 30.0});
    sawer::translate_object(rectangle, {-5.0, 8.0});
    REQUIRE(std::get<sawer::RectangleShape>(rectangle.geometry).first
            == sawer::Vec2d{-5.0, 8.0});
}

TEST_CASE("collapsed ellipses remain selectable for recovery")
{
    sawer::Document document;
    const auto id = sawer::ObjectId::from_u64(5U);
    REQUIRE(document.insert(sawer::Object::make_ellipse(
        id,
        0,
        {{12.0, -20.0}, {12.0, 20.0}},
        {
            .stroke = {},
            .fill = std::nullopt,
            .stroke_width = 4.0,
        })));

    REQUIRE(sawer::hit_test(document, {12.0, 0.0}, 1.0) == id);
}

TEST_CASE("image placements use unpadded bounds and rectangular editing")
{
    auto asset = std::make_shared<sawer::ImageAsset>();
    asset->pixel_width = 640U;
    asset->pixel_height = 480U;
    const auto id = sawer::ObjectId::from_u64(75U);
    auto image = sawer::Object::make_image(
        id, 3, {asset, {10.0, 20.0}, {110.0, 70.0}});
    REQUIRE(image.bounds.min_x == 10.0);
    REQUIRE(image.bounds.max_y == 70.0);

    sawer::Document document;
    REQUIRE(document.insert(image));
    REQUIRE(sawer::hit_test(document, {50.0, 45.0}, 0.0) == id);
    sawer::resize_object(
        image, sawer::SelectionHandle::bottom_right, {210.0, 120.0}, true);
    const auto& placement = std::get<sawer::Image>(image.geometry);
    REQUIRE(std::abs(placement.second.x - placement.first.x)
            / std::abs(placement.second.y - placement.first.y)
            == Catch::Approx(2.0));
}

TEST_CASE("shift constrains single-object line and shape resize")
{
    auto line = line_object(6U, 0, {0.0, 0.0}, {10.0, 0.0});
    sawer::resize_object(
        line,
        sawer::SelectionHandle::line_end,
        {20.0, 3.0},
        true);
    const auto& resized_line = std::get<sawer::Line>(line.geometry);
    REQUIRE(resized_line.end.y == Catch::Approx(0.0).margin(1.0e-9));

    auto rectangle = sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(7U),
        0,
        {{0.0, 0.0}, {10.0, 5.0}});
    sawer::resize_object(
        rectangle,
        sawer::SelectionHandle::bottom_right,
        {30.0, 12.0},
        true);
    const auto& resized =
        std::get<sawer::RectangleShape>(rectangle.geometry);
    REQUIRE(std::abs(resized.second.x - resized.first.x)
            == Catch::Approx(
                std::abs(resized.second.y - resized.first.y)));
}

TEST_CASE("a grouped edit is one history entry with every affected id")
{
    sawer::Document document;
    REQUIRE(document.insert(line_object(1U, 0, {0.0, 0.0}, {10.0, 0.0})));
    REQUIRE(document.insert(line_object(2U, 1, {0.0, 10.0}, {10.0, 10.0})));

    std::vector<std::unique_ptr<sawer::Command>> edits;
    for (const std::uint64_t id : {1U, 2U}) {
        auto replacement = *document.find(sawer::ObjectId::from_u64(id));
        sawer::translate_object(replacement, {25.0, 30.0});
        edits.push_back(
            std::make_unique<sawer::ModifyObjectCommand>(replacement));
    }

    sawer::CommandHistory history;
    history.execute(
        std::make_unique<sawer::CompositeCommand>(std::move(edits)), document);
    REQUIRE(history.size() == 1U);
    const auto affected = history.undo(document);
    REQUIRE(affected.size() == 2U);
    REQUIRE(std::get<sawer::Line>(
                document.find(sawer::ObjectId::from_u64(1U))->geometry)
                .start
            == sawer::Vec2d{0.0, 0.0});
}

TEST_CASE("multiple selection exposes four group resize handles")
{
    sawer::Document document;
    REQUIRE(document.insert(line_object(1U, 0, {0.0, 0.0}, {10.0, 0.0})));
    REQUIRE(document.insert(line_object(2U, 1, {20.0, 10.0}, {30.0, 10.0})));
    sawer::Selection selection;
    selection.select({
        sawer::ObjectId::from_u64(1U),
        sawer::ObjectId::from_u64(2U),
    });
    const auto bounds = selection.bounds(document);
    REQUIRE(bounds.has_value());

    REQUIRE(sawer::selection_handle_at(
        document, selection, {bounds->min_x, bounds->min_y}, 1.0)
        == sawer::SelectionHandle::top_left);
    REQUIRE(sawer::selection_handle_at(
        document, selection, {bounds->max_x, bounds->min_y}, 1.0)
        == sawer::SelectionHandle::top_right);
    REQUIRE(sawer::selection_handle_at(
        document, selection, {bounds->max_x, bounds->max_y}, 1.0)
        == sawer::SelectionHandle::bottom_right);
    REQUIRE(sawer::selection_handle_at(
        document, selection, {bounds->min_x, bounds->max_y}, 1.0)
        == sawer::SelectionHandle::bottom_left);
}

TEST_CASE("group resize scales every selected object around opposite corner")
{
    auto first = line_object(1U, 0, {0.0, 0.0}, {10.0, 0.0});
    auto second = line_object(2U, 1, {20.0, 10.0}, {30.0, 10.0});
    const sawer::Aabb bounds{
        std::min(first.bounds.min_x, second.bounds.min_x),
        std::min(first.bounds.min_y, second.bounds.min_y),
        std::max(first.bounds.max_x, second.bounds.max_x),
        std::max(first.bounds.max_y, second.bounds.max_y),
    };
    const sawer::Vec2d doubled_corner{
        bounds.min_x + (bounds.max_x - bounds.min_x) * 2.0,
        bounds.min_y + (bounds.max_y - bounds.min_y) * 2.0,
    };

    sawer::resize_object_in_group(
        first, bounds, sawer::SelectionHandle::bottom_right, doubled_corner);
    sawer::resize_object_in_group(
        second, bounds, sawer::SelectionHandle::bottom_right, doubled_corner);

    const auto& first_line = std::get<sawer::Line>(first.geometry);
    const auto& second_line = std::get<sawer::Line>(second.geometry);
    REQUIRE(first_line.start == (sawer::Vec2d{2.0, 2.0}));
    REQUIRE(first_line.end == (sawer::Vec2d{22.0, 2.0}));
    REQUIRE(second_line.start == (sawer::Vec2d{42.0, 22.0}));
    REQUIRE(second_line.end == (sawer::Vec2d{62.0, 22.0}));
}

TEST_CASE("group resize preview transform matches committed geometry")
{
    auto object =
        line_object(1U, 0, {4.0, 6.0}, {14.0, 10.0});
    const sawer::Aabb group_bounds{-2.0, -4.0, 22.0, 16.0};
    const sawer::Vec2d requested_corner{46.0, 36.0};
    const auto transform = sawer::group_resize_transform(
        group_bounds,
        sawer::SelectionHandle::bottom_right,
        requested_corner);
    const auto original_line = std::get<sawer::Line>(object.geometry);
    const sawer::Aabb original_bounds = object.bounds;

    sawer::resize_object_in_group(
        object,
        group_bounds,
        sawer::SelectionHandle::bottom_right,
        requested_corner);
    const auto& committed_line = std::get<sawer::Line>(object.geometry);

    REQUIRE(transform.apply(original_line.start)
            == committed_line.start);
    REQUIRE(transform.apply(original_line.end)
            == committed_line.end);
    const sawer::Aabb preview_bounds =
        transform.apply(original_bounds);
    const sawer::Aabb restored_bounds =
        transform.inverse(preview_bounds);
    REQUIRE(restored_bounds.min_x
            == Catch::Approx(original_bounds.min_x));
    REQUIRE(restored_bounds.min_y
            == Catch::Approx(original_bounds.min_y));
    REQUIRE(restored_bounds.max_x
            == Catch::Approx(original_bounds.max_x));
    REQUIRE(restored_bounds.max_y
            == Catch::Approx(original_bounds.max_y));
}

TEST_CASE("shift group resize preserves the original proportions")
{
    auto first = line_object(1U, 0, {0.0, 0.0}, {10.0, 0.0});
    auto second = line_object(2U, 1, {20.0, 10.0}, {30.0, 10.0});
    const sawer::Aabb bounds{
        std::min(first.bounds.min_x, second.bounds.min_x),
        std::min(first.bounds.min_y, second.bounds.min_y),
        std::max(first.bounds.max_x, second.bounds.max_x),
        std::max(first.bounds.max_y, second.bounds.max_y),
    };
    constexpr double original_ratio = 3.0;

    sawer::resize_object_in_group(
        first,
        bounds,
        sawer::SelectionHandle::bottom_right,
        {bounds.max_x + 40.0, bounds.max_y + 5.0},
        true);
    sawer::resize_object_in_group(
        second,
        bounds,
        sawer::SelectionHandle::bottom_right,
        {bounds.max_x + 40.0, bounds.max_y + 5.0},
        true);

    const auto& first_line = std::get<sawer::Line>(first.geometry);
    const auto& second_line = std::get<sawer::Line>(second.geometry);
    const double resized_ratio =
        (second_line.end.x - first_line.start.x)
        / (second_line.start.y - first_line.start.y);
    REQUIRE(resized_ratio == Catch::Approx(original_ratio).margin(1.0e-9));
}
