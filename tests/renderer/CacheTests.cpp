#include "core/ObjectRecency.hpp"
#include "document/Document.hpp"
#include "renderer/VisibleObjectCache.hpp"
#include "renderer/MeshArena.hpp"

#include <catch2/catch_test_macros.hpp>
#include <random>
#include <limits>

TEST_CASE("object recency survives touches removals copies and rehashing")
{
    sawer::ObjectRecency order;
    for (std::uint64_t id = 1U; id <= 1'000U; ++id) order.touch(sawer::ObjectId::from_u64(id));
    order.touch(sawer::ObjectId::from_u64(1U));
    REQUIRE(order.oldest() == sawer::ObjectId::from_u64(2U));
    auto copy = order;
    order.erase(sawer::ObjectId::from_u64(2U));
    REQUIRE(order.oldest() == sawer::ObjectId::from_u64(3U));
    REQUIRE(copy.oldest() == sawer::ObjectId::from_u64(2U));
    copy.clear();
    REQUIRE_FALSE(copy.oldest());
    auto moved = std::move(order);
    order.touch(sawer::ObjectId::from_u64(5U));
    REQUIRE(order.oldest() == sawer::ObjectId::from_u64(5U));
    REQUIRE(moved.oldest() == sawer::ObjectId::from_u64(3U));
    order.clear();
    order.touch(sawer::ObjectId::from_u64(0U));
    order.touch(sawer::ObjectId::from_u64(0U));
    REQUIRE(order.size() == 1U);
    order.erase(sawer::ObjectId::from_u64(0U));
    REQUIRE_FALSE(order.oldest());
}

TEST_CASE("visibility reuse matches fresh queries through pan edit and removal")
{
    sawer::Document document;
    for (std::uint64_t value = 1U; value <= 50U; ++value) {
        const auto id = sawer::ObjectId::from_u64(value);
        REQUIRE(document.insert(sawer::Object::make_line(id, static_cast<std::int64_t>(value % 3U),
            {{static_cast<double>(value) * 10.0, 0.0}, {static_cast<double>(value) * 10.0, 30.0}})));
    }
    sawer::VisibleObjectCache cache;
    std::vector<const sawer::Object*> result;
    std::vector<sawer::ObjectId> scratch;
    const sawer::Aabb view{0.0, -50.0, 200.0, 50.0};
    REQUIRE_FALSE(cache.query(document, view, 128.0, result, scratch));
    REQUIRE(result == document.query(view));
    const sawer::Aabb panned{20.0, -50.0, 220.0, 50.0};
    REQUIRE(cache.query(document, panned, 128.0, result, scratch));
    REQUIRE(result == document.query(panned));
    REQUIRE(document.translate(sawer::ObjectId::from_u64(1U), {300.0, 0.0}));
    REQUIRE(cache.query(document, panned, 128.0, result, scratch));
    REQUIRE(result == document.query(panned));
    REQUIRE(document.remove(sawer::ObjectId::from_u64(20U)).has_value());
    REQUIRE(cache.query(document, panned, 128.0, result, scratch));
    REQUIRE(result == document.query(panned));
    cache.clear();
    REQUIRE_FALSE(cache.query(document, panned, 128.0, result, scratch));
}

TEST_CASE("stroke index pressure evicts one cold entry and preserves hot indexes")
{
    sawer::Document document;
    for (std::uint64_t value = 1U; value <= 257U; ++value) {
        const auto id = sawer::ObjectId::from_u64(value);
        REQUIRE(document.insert(sawer::Object::make_stroke(id, static_cast<std::int64_t>(value),
            {{{0.0, 0.0}, {1.0, 1.0}}})));
        REQUIRE(document.stroke_segment_index(id) != nullptr);
        REQUIRE(document.stroke_segment_index(sawer::ObjectId::from_u64(1U)) != nullptr);
    }
    bool built = true;
    REQUIRE(document.stroke_segment_index(sawer::ObjectId::from_u64(1U), &built) != nullptr);
    REQUIRE_FALSE(built);
    REQUIRE(document.stroke_segment_index(sawer::ObjectId::from_u64(200U), &built) != nullptr);
    REQUIRE_FALSE(built);
    REQUIRE(document.stroke_segment_index(sawer::ObjectId::from_u64(2U), &built) != nullptr);
    REQUIRE(built);
}

TEST_CASE("pathological stroke indexes use bounded conservative blocks")
{
    sawer::Stroke stroke;
    for (std::size_t point = 0U; point < 30'000U; ++point)
        stroke.points.push_back({point % 2U == 0U ? 0.0 : 12'000.0, 0.0});
    // Seven chunks per segment: force the block fallback with more segments.
    for (std::size_t point = 30'000U; point < 170'000U; ++point)
        stroke.points.push_back({point % 2U == 0U ? 0.0 : 12'000.0, 0.0});
    const sawer::StrokeSegmentIndex index{stroke};
    REQUIRE(index.estimated_memory_bytes() < 1U * 1024U * 1024U);
    std::vector<std::uint32_t> segments;
    index.query({5'000.0, -1.0, 5'001.0, 1.0}, segments);
    REQUIRE(segments.size() == stroke.points.size() - 1U);
    REQUIRE(segments.front() == 1U);
    REQUIRE(segments.back() == stroke.points.size() - 1U);
    index.query({-100.0, 100.0, -50.0, 200.0}, segments);
    REQUIRE(segments.empty());
}

TEST_CASE("mesh arena rejects overflow without changing live allocations and merges holes")
{
    sawer::MeshArena arena;
    const auto first = arena.allocate(30U);
    const auto second = arena.allocate(60U);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE_FALSE(arena.allocate(sawer::MeshArena::maximum_pages * sawer::MeshArena::page_vertices));
    REQUIRE(arena.pages() == 1U);
    arena.release(*first);
    arena.release(*second);
    const auto whole = arena.allocate(sawer::MeshArena::page_vertices);
    REQUIRE(whole);
    REQUIRE(whole->size() == 1U);
    REQUIRE(whole->front().first == 0U);
    REQUIRE_FALSE(arena.allocate(1U));
    REQUIRE_FALSE(arena.allocate(0U));
    REQUIRE_FALSE(arena.allocate(std::numeric_limits<std::size_t>::max()));
}

TEST_CASE("randomized mesh allocations do not overlap and released ranges become reusable")
{
    sawer::MeshArena arena;
    std::mt19937 random{78191U};
    std::vector<std::vector<sawer::MeshRange>> live;
    std::vector<std::vector<bool>> occupied;
    for (std::size_t operation = 0U; operation < 1'500U; ++operation) {
        if (!live.empty() && random() % 3U == 0U) {
            const std::size_t victim = random() % live.size();
            for (const auto range : live[victim]) for (std::size_t index = range.first;
                    index < range.first + range.count; ++index) {
                REQUIRE(occupied[range.page][index]);
                occupied[range.page][index] = false;
            }
            arena.release(live[victim]);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
        } else {
            const auto allocation = arena.allocate((random() % 1'000U + 1U) * 3U);
            REQUIRE(allocation);
            while (occupied.size() < arena.pages()) occupied.emplace_back(sawer::MeshArena::page_vertices, false);
            for (const auto range : *allocation) for (std::size_t index = range.first;
                    index < range.first + range.count; ++index) {
                REQUIRE_FALSE(occupied[range.page][index]);
                occupied[range.page][index] = true;
            }
            live.push_back(*allocation);
        }
    }
    for (const auto& allocation : live) arena.release(allocation);
    REQUIRE(arena.allocate(arena.pages() * sawer::MeshArena::page_vertices));
}

TEST_CASE("document change journal covers mutations and signals rollover and reset")
{
    sawer::Document document;
    const auto id = sawer::ObjectId::from_u64(901U);
    REQUIRE(document.insert(sawer::Object::make_line(id, 0, {{0, 0}, {1, 1}})));
    std::vector<sawer::DocumentChange> changes;
    REQUIRE(document.changes_since(0U, changes));
    REQUIRE(changes.size() == 1U);
    REQUIRE(changes.front().id == id);
    const auto before = document.revision();
    REQUIRE(document.translate(id, {1, 0}));
    auto style = document.find(id)->style;
    style.stroke_width = 8.0;
    REQUIRE(document.exchange_style(id, style));
    REQUIRE(document.remove(id));
    REQUIRE_FALSE(document.remove(id));
    REQUIRE(document.changes_since(before, changes));
    REQUIRE(changes.size() == 3U);
    REQUIRE(changes.back().removed);
    REQUIRE(document.insert(sawer::Object::make_line(id, 0, {{0, 0}, {1, 1}})));
    for (std::size_t edit = 0; edit < 4'200U; ++edit) REQUIRE(document.translate(id, {0.01, 0}));
    REQUIRE_FALSE(document.changes_since(before, changes));
    REQUIRE(document.changes_since(document.revision() - 4'096U, changes));
    REQUIRE(changes.size() == 4'096U);
    REQUIRE(changes.front().sequence + changes.size() - 1U == document.revision());
    const auto last = document.revision();
    document.clear();
    REQUIRE_FALSE(document.changes_since(last, changes));
    REQUIRE(document.changes_since(document.revision(), changes));
    REQUIRE(changes.empty());
}

TEST_CASE("visibility updates preserve ordering through multiple changes and document replacement")
{
    sawer::Document document;
    const auto a = sawer::ObjectId::from_u64(10U), b = sawer::ObjectId::from_u64(11U);
    REQUIRE(document.insert(sawer::Object::make_line(a, 0, {{0, 0}, {1, 1}})));
    REQUIRE(document.insert(sawer::Object::make_line(b, 1, {{0, 0}, {1, 1}})));
    sawer::VisibleObjectCache cache;
    std::vector<const sawer::Object*> result;
    std::vector<sawer::ObjectId> scratch;
    const sawer::Aabb view{-10, -10, 10, 10};
    REQUIRE_FALSE(cache.query(document, view, 20.0, result, scratch));
    auto replacement = *document.find(b);
    replacement.z_order = -1;
    REQUIRE(document.replace(replacement));
    REQUIRE(document.translate(a, {1, 0}));
    REQUIRE(cache.query(document, view, 20.0, result, scratch));
    REQUIRE(result == document.query(view));
    sawer::Document incoming;
    REQUIRE(incoming.insert(sawer::Object::make_line(a, 0, {{100, 100}, {101, 101}})));
    document = std::move(incoming);
    REQUIRE_FALSE(cache.query(document, view, 20.0, result, scratch));
    REQUIRE(result.empty());
}
