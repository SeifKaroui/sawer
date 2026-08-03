#include "document/SpatialChunkIndex.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sawer {
namespace {

std::int32_t chunk_coordinate(const double value) noexcept
{
    const double coordinate =
        std::floor(value / SpatialChunkIndex::chunk_size);
    return static_cast<std::int32_t>(std::clamp(
        coordinate,
        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

} // namespace

void SpatialChunkIndex::insert(const ObjectId id, const Aabb& bounds)
{
    remove(id);
    if (covered_chunk_count(bounds) > maximum_object_chunks) {
        oversized_.insert_or_assign(id, bounds);
        membership_.insert_or_assign(id, Membership{});
        return;
    }
    const auto min_x = chunk_coordinate(bounds.min_x);
    const auto min_y = chunk_coordinate(bounds.min_y);
    const auto max_x = chunk_coordinate(bounds.max_x);
    const auto max_y = chunk_coordinate(bounds.max_y);
    for (std::int64_t y = min_y; y <= max_y; ++y) {
        for (std::int64_t x = min_x; x <= max_x; ++x) {
            chunks_[{
                static_cast<std::int32_t>(x),
                static_cast<std::int32_t>(y)}].push_back(id);
        }
    }
    membership_.insert_or_assign(
        id,
        Membership{
            .min_x = min_x,
            .min_y = min_y,
            .width = static_cast<std::uint16_t>(max_x - min_x + 1),
            .height = static_cast<std::uint16_t>(max_y - min_y + 1),
        });
}

void SpatialChunkIndex::remove(const ObjectId id)
{
    oversized_.erase(id);
    const auto membership = membership_.find(id);
    if (membership == membership_.end()) {
        return;
    }

    const Membership& range = membership->second;
    for (std::uint16_t y_offset = 0U;
         y_offset < range.height;
         ++y_offset) {
        for (std::uint16_t x_offset = 0U;
             x_offset < range.width;
             ++x_offset) {
            const ChunkKey key{
                static_cast<std::int32_t>(range.min_x + x_offset),
                static_cast<std::int32_t>(range.min_y + y_offset),
            };
            const auto chunk = chunks_.find(key);
            if (chunk == chunks_.end()) {
                continue;
            }

            std::erase(chunk->second, id);
            if (chunk->second.empty()) {
                chunks_.erase(chunk);
            }
        }
    }
    membership_.erase(membership);
}

void SpatialChunkIndex::update(const ObjectId id, const Aabb& bounds)
{
    insert(id, bounds);
}

std::vector<ObjectId> SpatialChunkIndex::query(const Aabb& bounds) const
{
    std::vector<ObjectId> result;
    query(bounds, result);
    return result;
}

void SpatialChunkIndex::query(
    const Aabb& bounds,
    std::vector<ObjectId>& result) const
{
    result.clear();
    begin_query_generation();
    if (covered_chunk_count(bounds) <= maximum_direct_query_chunks) {
        const auto min_x = chunk_coordinate(bounds.min_x);
        const auto min_y = chunk_coordinate(bounds.min_y);
        const auto max_x = chunk_coordinate(bounds.max_x);
        const auto max_y = chunk_coordinate(bounds.max_y);
        for (std::int64_t y = min_y; y <= max_y; ++y) {
            for (std::int64_t x = min_x; x <= max_x; ++x) {
                const auto chunk = chunks_.find({
                    static_cast<std::int32_t>(x),
                    static_cast<std::int32_t>(y)});
                if (chunk == chunks_.end()) {
                    continue;
                }
                for (const auto id : chunk->second) {
                    append_unique(id, result);
                }
            }
        }
    } else {
        const auto min_x = chunk_coordinate(bounds.min_x);
        const auto min_y = chunk_coordinate(bounds.min_y);
        const auto max_x = chunk_coordinate(bounds.max_x);
        const auto max_y = chunk_coordinate(bounds.max_y);
        for (const auto& [key, ids] : chunks_) {
            if (key.x >= min_x && key.x <= max_x
                && key.y >= min_y && key.y <= max_y) {
                for (const auto id : ids) {
                    append_unique(id, result);
                }
            }
        }
    }
    for (const auto& [id, object_bounds] : oversized_) {
        if (object_bounds.intersects(bounds)) {
            append_unique(id, result);
        }
    }
}

void SpatialChunkIndex::begin_query_generation() const noexcept
{
    ++query_generation_;
    if (query_generation_ != 0U) {
        return;
    }
    query_generation_ = 1U;
    for (const auto& [id, membership] : membership_) {
        static_cast<void>(id);
        membership.query_generation = 0U;
    }
}

void SpatialChunkIndex::append_unique(
    const ObjectId id,
    std::vector<ObjectId>& result) const
{
    const auto membership = membership_.find(id);
    if (membership == membership_.end()
        || membership->second.query_generation == query_generation_) {
        return;
    }
    membership->second.query_generation = query_generation_;
    result.push_back(id);
}

std::size_t SpatialChunkIndex::chunk_count() const noexcept
{
    return chunks_.size();
}

std::size_t SpatialChunkIndex::oversized_count() const noexcept
{
    return oversized_.size();
}

std::size_t SpatialChunkIndex::estimated_memory_bytes() const noexcept
{
    static_assert(sizeof(Membership) == 16U);
    static_assert(sizeof(ChunkKey) == 8U);
    constexpr std::size_t node_links = 2U * sizeof(void*);
    std::size_t bytes = sizeof(*this)
        + chunks_.bucket_count() * sizeof(void*)
        + membership_.bucket_count() * sizeof(void*)
        + oversized_.bucket_count() * sizeof(void*)
        + chunks_.size()
            * (sizeof(decltype(chunks_)::value_type) + node_links)
        + membership_.size()
            * (sizeof(decltype(membership_)::value_type) + node_links)
        + oversized_.size()
            * (sizeof(decltype(oversized_)::value_type) + node_links);
    for (const auto& [key, ids] : chunks_) {
        static_cast<void>(key);
        bytes += ids.capacity() * sizeof(ObjectId);
    }
    return bytes;
}

std::size_t SpatialChunkIndex::ChunkKeyHash::operator()(
    const ChunkKey& key) const noexcept
{
    const auto x = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(key.x));
    const auto y = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(key.y));
    return static_cast<std::size_t>(
        (x * 0x9E3779B185EBCA87ULL) ^ (y + 0xC2B2AE3D27D4EB4FULL));
}

std::size_t SpatialChunkIndex::covered_chunk_count(
    const Aabb& bounds) noexcept
{
    const auto min_x = static_cast<std::int64_t>(
        chunk_coordinate(bounds.min_x));
    const auto min_y = static_cast<std::int64_t>(
        chunk_coordinate(bounds.min_y));
    const auto max_x = static_cast<std::int64_t>(
        chunk_coordinate(bounds.max_x));
    const auto max_y = static_cast<std::int64_t>(
        chunk_coordinate(bounds.max_y));
    const auto width = static_cast<std::uint64_t>(
        std::max<std::int64_t>(max_x - min_x + 1, 1));
    const auto height = static_cast<std::uint64_t>(
        std::max<std::int64_t>(max_y - min_y + 1, 1));
    if (height != 0U
        && width > std::numeric_limits<std::uint64_t>::max() / height) {
        return std::numeric_limits<std::size_t>::max();
    }
    const auto count = width * height;
    return count > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(count);
}

} // namespace sawer
