#include "document/Command.hpp"
#include "document/Document.hpp"
#include "document/Object.hpp"
#include "document/ObjectId.hpp"
#include "document/SpatialChunkIndex.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <limits>
#include <unordered_set>

namespace {

using Catch::Approx;

sawer::Object make_line(
    const std::uint64_t id,
    const sawer::Vec2d start,
    const sawer::Vec2d end,
    const std::int64_t z_order = 0)
{
    return sawer::Object::make_line(
        sawer::ObjectId::from_u64(id),
        z_order,
        {start, end},
        {
            .stroke = {20U, 40U, 60U, 255U},
            .fill = std::nullopt,
            .stroke_width = 4.0,
        });
}

TEST_CASE("object identifiers have UUID v4 layout")
{
    const auto first = sawer::ObjectId::random();
    const auto second = sawer::ObjectId::random();

    REQUIRE(first != second);
    REQUIRE(first.to_string().size() == 36U);
    REQUIRE((first.bytes()[6] & 0xF0U) == 0x40U);
    REQUIRE((first.bytes()[8] & 0xC0U) == 0x80U);
}

TEST_CASE("default style uses the regular pencil width")
{
    REQUIRE(sawer::Style{}.stroke_width == 5.0);
}

TEST_CASE("line bounds include stroke width")
{
    const auto object = make_line(1U, {20.0, -10.0}, {-30.0, 40.0});

    REQUIRE(object.bounds.min_x == Approx(-32.0));
    REQUIRE(object.bounds.min_y == Approx(-12.0));
    REQUIRE(object.bounds.max_x == Approx(22.0));
    REQUIRE(object.bounds.max_y == Approx(42.0));
}

TEST_CASE("stroke and shape bounds include their outlines")
{
    const auto stroke = sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(100U),
        0,
        {{{-5.0, 10.0}, {20.0, -30.0}, {50.0, 4.0}}},
        {
            .stroke = {},
            .fill = std::nullopt,
            .stroke_width = 6.0,
        });
    REQUIRE(stroke.bounds.min_x == Approx(-8.0));
    REQUIRE(stroke.bounds.min_y == Approx(-33.0));
    REQUIRE(stroke.bounds.max_x == Approx(53.0));

    const auto rectangle = sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(101U),
        1,
        {{30.0, 40.0}, {-10.0, -20.0}},
        {
            .stroke = {},
            .fill = std::nullopt,
            .stroke_width = 4.0,
        });
    REQUIRE(rectangle.bounds.min_x == Approx(-12.0));
    REQUIRE(rectangle.bounds.max_y == Approx(42.0));

    const auto ellipse = sawer::Object::make_ellipse(
        sawer::ObjectId::from_u64(102U),
        2,
        {{-50.0, -25.0}, {50.0, 25.0}},
        {
            .stroke = {},
            .fill = std::nullopt,
            .stroke_width = 2.0,
        });
    REQUIRE(ellipse.bounds.min_x == Approx(-51.0));
    REQUIRE(ellipse.bounds.max_y == Approx(26.0));
}

TEST_CASE("spatial chunks deduplicate spanning objects")
{
    sawer::SpatialChunkIndex index;
    const auto id = sawer::ObjectId::from_u64(42U);
    index.insert(id, {-3'000.0, -3'000.0, 3'000.0, 3'000.0});

    const auto result = index.query({-4'000.0, -4'000.0, 4'000.0, 4'000.0});
    REQUIRE(result.size() == 1U);
    REQUIRE(result.front() == id);
    REQUIRE(index.chunk_count() > 1U);

    index.update(id, {10'000.0, 10'000.0, 10'010.0, 10'010.0});
    REQUIRE(index.query({-4'000.0, -4'000.0, 4'000.0, 4'000.0}).empty());
    REQUIRE(index.query({9'000.0, 9'000.0, 11'000.0, 11'000.0}).size() == 1U);
}

TEST_CASE("board spanning objects avoid pathological chunk membership")
{
    sawer::SpatialChunkIndex index;
    const auto id = sawer::ObjectId::from_u64(43U);
    index.insert(
        id,
        {-1'000'000.0, -1'000'000.0, 1'000'000.0, 1'000'000.0});

    REQUIRE(index.oversized_count() == 1U);
    REQUIRE(index.chunk_count() == 0U);
    REQUIRE(index.query({-100.0, -100.0, 100.0, 100.0}) ==
            std::vector<sawer::ObjectId>{id});
    REQUIRE(index.query({1'100'000.0, 1'100'000.0, 1'200'000.0, 1'200'000.0}).empty());

    index.remove(id);
    REQUIRE(index.oversized_count() == 0U);
}

TEST_CASE("spatial index safely handles extreme finite bounds")
{
    sawer::SpatialChunkIndex index;
    const auto id = sawer::ObjectId::from_u64(44U);
    index.insert(id, {-1.0e300, -1.0e300, 1.0e300, 1.0e300});

    REQUIRE(index.oversized_count() == 1U);
    REQUIRE(index.chunk_count() == 0U);
    REQUIRE(index.query({-1.0, -1.0, 1.0, 1.0})
            == std::vector<sawer::ObjectId>{id});
}

TEST_CASE("document queries visible objects in z order")
{
    sawer::Document document;
    REQUIRE(document.insert(make_line(1U, {-10.0, 0.0}, {10.0, 0.0}, 7)));
    REQUIRE(document.insert(make_line(2U, {-10.0, 5.0}, {10.0, 5.0}, 2)));
    REQUIRE(document.insert(make_line(3U, {50'000.0, 0.0}, {50'010.0, 0.0}, 1)));

    const auto visible = document.query({-100.0, -100.0, 100.0, 100.0});
    REQUIRE(visible.size() == 2U);
    REQUIRE(visible[0]->z_order == 2);
    REQUIRE(visible[1]->z_order == 7);
}

TEST_CASE("equal z orders are deterministic by object id")
{
    sawer::Document document;
    REQUIRE(document.insert(make_line(3U, {-5.0, 0.0}, {5.0, 0.0}, 4)));
    REQUIRE(document.insert(make_line(1U, {-5.0, 1.0}, {5.0, 1.0}, 4)));
    REQUIRE(document.insert(make_line(2U, {-5.0, 2.0}, {5.0, 2.0}, 4)));

    const auto visible = document.query({-10.0, -10.0, 10.0, 10.0});
    REQUIRE(visible.size() == 3U);
    REQUIRE(visible[0]->id == sawer::ObjectId::from_u64(1U));
    REQUIRE(visible[1]->id == sawer::ObjectId::from_u64(2U));
    REQUIRE(visible[2]->id == sawer::ObjectId::from_u64(3U));
}

TEST_CASE("document rejects exhausted z orders without signed overflow")
{
    sawer::Document document;
    REQUIRE_FALSE(document.insert(make_line(
        1U,
        {0.0, 0.0},
        {1.0, 1.0},
        std::numeric_limits<std::int64_t>::max())));
    REQUIRE(document.insert(make_line(
        2U,
        {0.0, 0.0},
        {1.0, 1.0},
        std::numeric_limits<std::int64_t>::max() - 1)));
    REQUIRE_THROWS_AS(document.next_z_order(), std::overflow_error);
}

TEST_CASE("document mutations keep geometry inside finite board coordinates")
{
    sawer::Document document;
    REQUIRE_FALSE(document.insert(make_line(
        1U,
        {sawer::board_half_extent + 1.0, 0.0},
        {0.0, 0.0})));

    const auto id = sawer::ObjectId::from_u64(2U);
    REQUIRE(document.insert(make_line(
        2U,
        {sawer::board_half_extent - 10.0, 0.0},
        {sawer::board_half_extent, 0.0})));
    REQUIRE_FALSE(document.translate(id, {1.0, 0.0}));
    REQUIRE(std::get<sawer::Line>(document.find(id)->geometry).end.x
            == sawer::board_half_extent);
}

TEST_CASE("document revision changes only after successful mutations")
{
    sawer::Document document;
    const auto original = make_line(
        7U, {-10.0, 0.0}, {10.0, 0.0});
    const std::uint64_t initial_revision = document.revision();

    REQUIRE(document.insert(original));
    REQUIRE(document.revision() == initial_revision + 1U);
    REQUIRE_FALSE(document.insert(original));
    REQUIRE(document.revision() == initial_revision + 1U);

    auto replacement = original;
    replacement.geometry =
        sawer::Line{{20.0, 5.0}, {40.0, 5.0}};
    REQUIRE(document.replace(replacement));
    REQUIRE(document.revision() == initial_revision + 2U);
    REQUIRE_FALSE(document.replace(make_line(
        8U, {0.0, 0.0}, {1.0, 1.0})));
    REQUIRE(document.revision() == initial_revision + 2U);

    REQUIRE(document.remove(original.id).has_value());
    REQUIRE(document.revision() == initial_revision + 3U);
    REQUIRE_FALSE(document.remove(original.id).has_value());
    REQUIRE(document.revision() == initial_revision + 3U);

    document.clear();
    REQUIRE(document.revision() == initial_revision + 4U);
}

TEST_CASE("clearing a document releases retained container capacity")
{
    sawer::Document document;
    for (std::size_t index = 0U; index < 4'096U; ++index) {
        REQUIRE(document.insert(make_line(
            index + 1U,
            {static_cast<double>(index), 0.0},
            {static_cast<double>(index) + 1.0, 1.0})));
    }
    const auto populated = document.memory_stats();
    REQUIRE(populated.object_storage_bytes > 512U * 1024U);

    document.clear();
    const auto cleared = document.memory_stats();
    REQUIRE(cleared.object_count == 0U);
    REQUIRE(cleared.object_storage_bytes < 1'024U);
    REQUIRE(cleared.spatial_index_bytes < 1'024U);
}

TEST_CASE("commands support undo redo and clean-state tracking")
{
    sawer::Document document;
    sawer::CommandHistory history;
    const auto original = make_line(9U, {0.0, 0.0}, {20.0, 10.0});

    history.execute(
        std::make_unique<sawer::AddObjectCommand>(original),
        document);
    REQUIRE(document.size() == 1U);
    REQUIRE(document.dirty());

    history.mark_saved(document);
    REQUIRE_FALSE(document.dirty());

    auto changed = original;
    changed.geometry = sawer::Line{{100.0, 100.0}, {120.0, 110.0}};
    changed.recompute_bounds();
    history.execute(
        std::make_unique<sawer::ModifyObjectCommand>(changed),
        document);
    REQUIRE(document.dirty());
    REQUIRE(document.query({90.0, 90.0, 130.0, 120.0}).size() == 1U);
    REQUIRE(document.query({-10.0, -10.0, 30.0, 20.0}).empty());

    REQUIRE_FALSE(history.undo(document).empty());
    REQUIRE_FALSE(document.dirty());
    REQUIRE(document.query({-10.0, -10.0, 30.0, 20.0}).size() == 1U);

    REQUIRE_FALSE(history.redo(document).empty());
    REQUIRE(document.dirty());

    history.execute(
        std::make_unique<sawer::DeleteObjectCommand>(original.id),
        document);
    REQUIRE(document.size() == 0U);
    REQUIRE_FALSE(history.undo(document).empty());
    REQUIRE(document.size() == 1U);
}

TEST_CASE("executing after undo discards the redo branch")
{
    sawer::Document document;
    sawer::CommandHistory history;
    history.execute(
        std::make_unique<sawer::AddObjectCommand>(
            make_line(1U, {0.0, 0.0}, {1.0, 1.0})),
        document);
    history.execute(
        std::make_unique<sawer::AddObjectCommand>(
            make_line(2U, {2.0, 2.0}, {3.0, 3.0})),
        document);

    REQUIRE_FALSE(history.undo(document).empty());
    REQUIRE(history.can_redo());

    history.execute(
        std::make_unique<sawer::AddObjectCommand>(
            make_line(3U, {4.0, 4.0}, {5.0, 5.0})),
        document);
    REQUIRE_FALSE(history.can_redo());
    REQUIRE(history.size() == 2U);
}

TEST_CASE("command history transfers stroke storage without copying")
{
    sawer::Stroke stroke;
    stroke.points.reserve(4'096U);
    for (std::size_t index = 0U; index < 4'096U; ++index) {
        stroke.points.push_back({
            static_cast<double>(index),
            static_cast<double>(index % 31U),
        });
    }
    const sawer::Vec2d* const added_storage = stroke.points.data();
    auto object = sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(51U),
        0,
        std::move(stroke));

    sawer::Document document;
    sawer::CommandHistory history;
    history.execute(
        std::make_unique<sawer::AddObjectCommand>(std::move(object)),
        document);
    const auto* stored = document.find(sawer::ObjectId::from_u64(51U));
    REQUIRE(stored != nullptr);
    REQUIRE(std::get<sawer::Stroke>(stored->geometry).points.data()
            == added_storage);
    REQUIRE(history.retained_bytes()
            < 4'096U * sizeof(sawer::Vec2d));

    REQUIRE_FALSE(history.undo(document).empty());
    REQUIRE(history.retained_bytes()
            >= 4'096U * sizeof(sawer::Vec2d));
    REQUIRE_FALSE(history.redo(document).empty());
    stored = document.find(sawer::ObjectId::from_u64(51U));
    REQUIRE(stored != nullptr);
    REQUIRE(std::get<sawer::Stroke>(stored->geometry).points.data()
            == added_storage);

    auto replacement = *stored;
    auto& replacement_points =
        std::get<sawer::Stroke>(replacement.geometry).points;
    replacement_points.push_back({5'000.0, 25.0});
    const sawer::Vec2d* const replacement_storage =
        replacement_points.data();
    history.execute(
        std::make_unique<sawer::ModifyObjectCommand>(
            std::move(replacement)),
        document);
    stored = document.find(sawer::ObjectId::from_u64(51U));
    REQUIRE(std::get<sawer::Stroke>(stored->geometry).points.data()
            == replacement_storage);

    REQUIRE_FALSE(history.undo(document).empty());
    stored = document.find(sawer::ObjectId::from_u64(51U));
    REQUIRE(std::get<sawer::Stroke>(stored->geometry).points.data()
            == added_storage);
    REQUIRE_FALSE(history.redo(document).empty());
    stored = document.find(sawer::ObjectId::from_u64(51U));
    REQUIRE(std::get<sawer::Stroke>(stored->geometry).points.data()
            == replacement_storage);
}

TEST_CASE("style commands never copy retained stroke points")
{
    sawer::Stroke stroke;
    stroke.points.reserve(4'096U);
    for (std::size_t index = 0U; index < 4'096U; ++index) {
        stroke.points.push_back({
            static_cast<double>(index),
            static_cast<double>(index % 19U),
        });
    }
    const sawer::Vec2d* const point_storage = stroke.points.data();
    auto object = sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(71U),
        0,
        std::move(stroke));
    const sawer::Style original_style = object.style;

    sawer::Document document;
    sawer::CommandHistory history;
    history.execute(
        std::make_unique<sawer::AddObjectCommand>(std::move(object)),
        document);
    const std::size_t retained_before_style = history.retained_bytes();

    sawer::Style changed_style = original_style;
    changed_style.stroke = {12U, 90U, 220U, 255U};
    changed_style.stroke_width = 11.0;
    history.execute(
        std::make_unique<sawer::ChangeStyleCommand>(
            sawer::ObjectId::from_u64(71U),
            changed_style),
        document);

    const auto stored = [&]() {
        return document.find(sawer::ObjectId::from_u64(71U));
    };
    REQUIRE(stored() != nullptr);
    REQUIRE(stored()->style == changed_style);
    REQUIRE(std::get<sawer::Stroke>(stored()->geometry).points.data()
            == point_storage);
    REQUIRE(history.retained_bytes() - retained_before_style
            < 1'024U);

    REQUIRE_FALSE(history.undo(document).empty());
    REQUIRE(stored()->style == original_style);
    REQUIRE(std::get<sawer::Stroke>(stored()->geometry).points.data()
            == point_storage);
    REQUIRE_FALSE(history.redo(document).empty());
    REQUIRE(stored()->style == changed_style);
    REQUIRE(std::get<sawer::Stroke>(stored()->geometry).points.data()
            == point_storage);
}

TEST_CASE("move commands keep large stroke history bounded")
{
    sawer::Stroke stroke;
    stroke.points.reserve(100'000U);
    for (std::size_t index = 0U; index < 100'000U; ++index) {
        stroke.points.push_back({
            static_cast<double>(index),
            static_cast<double>(index % 17U),
        });
    }
    const auto id = sawer::ObjectId::from_u64(81U);
    const sawer::Vec2d* const storage = stroke.points.data();

    sawer::Document document;
    REQUIRE(document.insert(sawer::Object::make_stroke(
        id, 0, std::move(stroke))));
    sawer::CommandHistory history;
    for (std::size_t index = 0U; index < 100U; ++index) {
        history.execute(
            std::make_unique<sawer::MoveObjectCommand>(
                id, sawer::Vec2d{1.0, -0.5}),
            document);
    }

    const auto* const moved = document.find(id);
    REQUIRE(moved != nullptr);
    REQUIRE(std::get<sawer::Stroke>(moved->geometry).points.data() == storage);
    REQUIRE(std::get<sawer::Stroke>(moved->geometry).points.front()
            == sawer::Vec2d{100.0, -50.0});
    REQUIRE(history.retained_bytes() < 64U * 1'024U);

    REQUIRE_FALSE(history.undo(document).empty());
    REQUIRE(std::get<sawer::Stroke>(document.find(id)->geometry).points.front()
            == sawer::Vec2d{99.0, -49.5});
}

TEST_CASE("long stroke segment caches are shared and geometry invalidates them")
{
    constexpr std::size_t point_count = 8'192U;
    sawer::Stroke stroke;
    stroke.points.reserve(point_count);
    for (std::size_t index = 0U; index < point_count; ++index) {
        stroke.points.push_back({
            static_cast<double>(index),
            static_cast<double>(index % 31U),
        });
    }
    const auto id = sawer::ObjectId::from_u64(811U);
    sawer::Document document;
    REQUIRE(document.insert(sawer::Object::make_stroke(
        id, 0, std::move(stroke))));

    bool built = false;
    const auto* const first = document.stroke_segment_index(id, &built);
    REQUIRE(first != nullptr);
    REQUIRE(built);
    const std::size_t indexed_bytes =
        document.memory_stats().spatial_index_bytes;

    const auto* const reused = document.stroke_segment_index(id, &built);
    REQUIRE(reused == first);
    REQUIRE_FALSE(built);

    sawer::Style style = document.find(id)->style;
    style.stroke_width = 9.0;
    REQUIRE(document.exchange_style(id, style).has_value());
    REQUIRE(document.stroke_segment_index(id, &built) == first);
    REQUIRE_FALSE(built);

    REQUIRE(document.translate(id, {10.0, -5.0}));
    REQUIRE(document.memory_stats().spatial_index_bytes < indexed_bytes);
    REQUIRE(document.stroke_segment_index(id, &built) != nullptr);
    REQUIRE(built);

    document.clear_transient_caches();
    REQUIRE(document.memory_stats().spatial_index_bytes < indexed_bytes);
}

TEST_CASE("document enforces object resource and style invariants")
{
    sawer::Document document;

    REQUIRE_FALSE(document.insert(sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(812U), 0, {})));

    sawer::Stroke oversized;
    oversized.points.resize(sawer::maximum_stroke_point_count + 1U);
    REQUIRE_FALSE(document.insert(sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(813U), 0, std::move(oversized))));

    sawer::Style invalid_style;
    invalid_style.stroke_width = -1.0;
    REQUIRE_FALSE(document.insert(sawer::Object::make_line(
        sawer::ObjectId::from_u64(814U),
        0,
        {{0.0, 0.0}, {1.0, 1.0}},
        invalid_style)));

    REQUIRE_FALSE(document.insert(sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(815U),
        0,
        {{0.0, 0.0}, {1.0, 1.0}, 0.75})));
}

TEST_CASE("command history drops its oldest entries at the fixed limit")
{
    const auto id = sawer::ObjectId::from_u64(82U);
    sawer::Document document;
    REQUIRE(document.insert(make_line(id.bytes().back(), {0.0, 0.0}, {1.0, 1.0})));
    sawer::CommandHistory history;
    for (std::size_t index = 0U; index < 1'100U; ++index) {
        history.execute(
            std::make_unique<sawer::MoveObjectCommand>(
                id, sawer::Vec2d{1.0, 0.0}),
            document);
    }

    REQUIRE(history.size() == 1'000U);
    for (std::size_t index = 0U; index < 1'000U; ++index) {
        REQUIRE_FALSE(history.undo(document).empty());
    }
    REQUIRE_FALSE(history.can_undo());
    REQUIRE(std::get<sawer::Line>(document.find(id)->geometry).start.x
            == Catch::Approx(100.0));
}

TEST_CASE("composite commands roll back a partial failure")
{
    const auto id = sawer::ObjectId::from_u64(83U);
    sawer::Document document;
    REQUIRE(document.insert(make_line(83U, {0.0, 0.0}, {1.0, 1.0})));

    std::vector<std::unique_ptr<sawer::Command>> deletes;
    deletes.push_back(std::make_unique<sawer::DeleteObjectCommand>(id));
    deletes.push_back(std::make_unique<sawer::DeleteObjectCommand>(id));
    sawer::CompositeCommand command{std::move(deletes)};

    REQUIRE_THROWS(command.apply(document));
    REQUIRE(document.find(id) != nullptr);
}

TEST_CASE("command history accounts a shared image asset only once")
{
    auto asset = std::make_shared<sawer::ImageAsset>();
    asset->pixel_width = 1U;
    asset->pixel_height = 1U;
    asset->png.resize(1U * 1024U * 1024U, 7U);
    sawer::Document document;
    const auto first = sawer::ObjectId::from_u64(90U);
    const auto second = sawer::ObjectId::from_u64(91U);
    REQUIRE(document.insert(sawer::Object::make_image(
        first, 0, {asset, {0.0, 0.0}, {10.0, 10.0}})));
    REQUIRE(document.insert(sawer::Object::make_image(
        second, 1, {asset, {20.0, 20.0}, {30.0, 30.0}})));
    sawer::CommandHistory history;
    history.execute(
        std::make_unique<sawer::DeleteObjectCommand>(first), document);
    history.execute(
        std::make_unique<sawer::DeleteObjectCommand>(second), document);
    REQUIRE(history.retained_bytes() > asset->png.size());
    REQUIRE(history.retained_bytes() < asset->png.size() * 2U);
}

} // namespace
