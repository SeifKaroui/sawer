#include "document/Document.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sawer {
namespace {

constexpr std::size_t stroke_segment_cache_budget =
    128U * 1024U * 1024U;
constexpr std::size_t maximum_stroke_segment_cache_entries = 256U;

bool object_order(const Object* const first, const Object* const second)
{
    return first->z_order < second->z_order
        || (first->z_order == second->z_order && first->id < second->id);
}

bool valid_object(const Object& object) noexcept
{
    if (!std::isfinite(object.style.stroke_width)
        || object.style.stroke_width <= 0.0
        || object.style.stroke_width > 10'000.0) {
        return false;
    }
    if (const auto* const stroke =
            std::get_if<Stroke>(&object.geometry)) {
        return !stroke->points.empty()
            && stroke->points.size() <= maximum_stroke_point_count;
    }
    if (const auto* const rectangle =
            std::get_if<RectangleShape>(&object.geometry)) {
        return std::isfinite(rectangle->roundness)
            && rectangle->roundness >= 0.0
            && rectangle->roundness <= 0.5;
    }
    return true;
}

void advance_z_order(
    std::int64_t& next_z_order,
    const std::int64_t object_z_order)
{
    if (object_z_order == std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error{"Object z-order is exhausted"};
    }
    next_z_order = std::max(next_z_order, object_z_order + 1);
}

} // namespace

bool Document::insert(Object object)
{
    if (object.z_order == std::numeric_limits<std::int64_t>::max()
        || !valid_object(object)
        || !object.within_board_bounds()) {
        return false;
    }
    const auto [iterator, inserted] =
        objects_.emplace(object.id, std::move(object));
    if (!inserted) {
        return false;
    }
    spatial_index_.insert(iterator->first, iterator->second.bounds);
    advance_z_order(next_z_order_, iterator->second.z_order);
    ++revision_;
    return true;
}

std::optional<Object> Document::remove(const ObjectId id)
{
    const auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return std::nullopt;
    }

    Object removed = std::move(iterator->second);
    spatial_index_.remove(id);
    erase_stroke_segment_index(id);
    objects_.erase(iterator);
    ++revision_;
    return removed;
}

bool Document::replace(Object object)
{
    if (object.z_order == std::numeric_limits<std::int64_t>::max()
        || !valid_object(object)
        || !object.within_board_bounds()) {
        return false;
    }
    return exchange(std::move(object)).has_value();
}

std::optional<Object> Document::exchange(Object object)
{
    if (object.z_order == std::numeric_limits<std::int64_t>::max()
        || !valid_object(object)
        || !object.within_board_bounds()) {
        return std::nullopt;
    }
    const auto iterator = objects_.find(object.id);
    if (iterator == objects_.end()) {
        return std::nullopt;
    }

    object.revision = iterator->second.revision + 1U;
    object.recompute_bounds();
    Object previous = std::move(iterator->second);
    iterator->second = std::move(object);
    erase_stroke_segment_index(iterator->first);
    spatial_index_.update(iterator->first, iterator->second.bounds);
    advance_z_order(next_z_order_, iterator->second.z_order);
    ++revision_;
    return previous;
}

bool Document::translate(const ObjectId id, const Vec2d delta)
{
    const auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return false;
    }
    if (!iterator->second.can_translate(delta)) {
        return false;
    }
    iterator->second.translate(delta);
    erase_stroke_segment_index(iterator->first);
    ++iterator->second.revision;
    spatial_index_.update(iterator->first, iterator->second.bounds);
    ++revision_;
    return true;
}

std::optional<Style> Document::exchange_style(
    const ObjectId id,
    Style style)
{
    const auto iterator = objects_.find(id);
    if (iterator == objects_.end()) {
        return std::nullopt;
    }

    Style previous = std::move(iterator->second.style);
    iterator->second.style = std::move(style);
    ++iterator->second.revision;
    iterator->second.recompute_bounds();
    spatial_index_.update(iterator->first, iterator->second.bounds);
    ++revision_;
    return previous;
}

const Object* Document::find(const ObjectId id) const noexcept
{
    const auto iterator = objects_.find(id);
    return iterator != objects_.end() ? &iterator->second : nullptr;
}

std::vector<const Object*> Document::query(const Aabb& bounds) const
{
    std::vector<const Object*> result;
    std::vector<ObjectId> id_scratch;
    query(bounds, result, id_scratch);
    return result;
}

void Document::query(
    const Aabb& bounds,
    std::vector<const Object*>& result,
    std::vector<ObjectId>& id_scratch) const
{
    result.clear();
    spatial_index_.query(bounds, id_scratch);
    result.reserve(id_scratch.size());
    for (const auto id : id_scratch) {
        const auto* const object = find(id);
        if (object != nullptr && object->bounds.intersects(bounds)) {
            result.push_back(object);
        }
    }

    std::ranges::sort(result, object_order);
}

std::vector<const Object*> Document::all_objects() const
{
    std::vector<const Object*> result;
    result.reserve(objects_.size());
    for (const auto& [id, object] : objects_) {
        static_cast<void>(id);
        result.push_back(&object);
    }
    std::ranges::sort(result, object_order);
    return result;
}

std::size_t Document::size() const noexcept
{
    return objects_.size();
}

std::size_t Document::spatial_chunk_count() const noexcept
{
    return spatial_index_.chunk_count();
}

std::size_t Document::oversized_object_count() const noexcept
{
    return spatial_index_.oversized_count();
}

std::uint64_t Document::revision() const noexcept
{
    return revision_;
}

DocumentMemoryStats Document::memory_stats() const noexcept
{
    DocumentMemoryStats stats;
    stats.object_count = objects_.size();
    constexpr std::size_t node_links = 2U * sizeof(void*);
    stats.object_storage_bytes =
        sizeof(*this) - sizeof(spatial_index_)
            - sizeof(stroke_segment_indices_)
        + objects_.bucket_count() * sizeof(void*)
        + objects_.size()
            * (sizeof(decltype(objects_)::value_type) + node_links);
    for (const auto& [id, object] : objects_) {
        static_cast<void>(id);
        if (const auto* const stroke =
                std::get_if<Stroke>(&object.geometry)) {
            stats.stroke_point_count += stroke->points.size();
            stats.stroke_point_capacity_bytes +=
                stroke->points.capacity() * sizeof(Vec2d);
        }
    }
    stats.spatial_index_bytes =
        spatial_index_.estimated_memory_bytes();
    stats.spatial_index_bytes += stroke_segment_index_bytes_;
    stats.total_bytes = stats.object_storage_bytes
        + stats.stroke_point_capacity_bytes
        + stats.spatial_index_bytes;
    return stats;
}

void Document::query_stroke_segments(
    const ObjectId id,
    const Aabb& bounds,
    std::vector<std::uint32_t>& result) const
{
    const StrokeSegmentIndex* const index = stroke_segment_index(id);
    if (index == nullptr) {
        result.clear();
        return;
    }
    index->query(bounds, result);
}

const StrokeSegmentIndex* Document::stroke_segment_index(
    const ObjectId id,
    bool* const built) const
{
    if (built != nullptr) {
        *built = false;
    }
    const Object* const object = find(id);
    if (object == nullptr) {
        return nullptr;
    }
    const auto* const stroke = std::get_if<Stroke>(&object->geometry);
    if (stroke == nullptr) {
        return nullptr;
    }
    auto index = stroke_segment_indices_.find(id);
    if (index == stroke_segment_indices_.end()) {
        if (stroke_segment_indices_.size()
                >= maximum_stroke_segment_cache_entries
            || stroke_segment_index_bytes_
                >= stroke_segment_cache_budget) {
            clear_transient_caches();
        }
        index = stroke_segment_indices_
                    .emplace(id, StrokeSegmentIndex{*stroke})
                    .first;
        stroke_segment_index_bytes_ +=
            index->second.estimated_memory_bytes();
        if (built != nullptr) {
            *built = true;
        }
    }
    return &index->second;
}

void Document::clear_transient_caches() const noexcept
{
    decltype(stroke_segment_indices_){}.swap(stroke_segment_indices_);
    stroke_segment_index_bytes_ = 0U;
}

std::int64_t Document::next_z_order()
{
    if (next_z_order_ == std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error{"Document z-order is exhausted"};
    }
    return next_z_order_++;
}

bool Document::dirty() const noexcept
{
    return dirty_;
}

void Document::clear() noexcept
{
    decltype(objects_){}.swap(objects_);
    spatial_index_ = {};
    decltype(stroke_segment_indices_){}.swap(stroke_segment_indices_);
    stroke_segment_index_bytes_ = 0U;
    next_z_order_ = 0;
    ++revision_;
    dirty_ = false;
}

void Document::erase_stroke_segment_index(const ObjectId id) noexcept
{
    const auto found = stroke_segment_indices_.find(id);
    if (found == stroke_segment_indices_.end()) {
        return;
    }
    const std::size_t bytes = found->second.estimated_memory_bytes();
    stroke_segment_index_bytes_ =
        bytes <= stroke_segment_index_bytes_
        ? stroke_segment_index_bytes_ - bytes
        : 0U;
    stroke_segment_indices_.erase(found);
}

void Document::set_dirty(const bool dirty) noexcept
{
    dirty_ = dirty;
}

} // namespace sawer
