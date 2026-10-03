#include "document/StrokeSegmentIndex.hpp"

#include "document/SpatialChunkIndex.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sawer {
namespace {

constexpr std::size_t maximum_segment_chunks = 64U;
constexpr std::size_t maximum_direct_query_chunks = 4'096U;
constexpr std::size_t maximum_index_references = 1'000'000U;
constexpr std::size_t segments_per_block = 256U;

std::int32_t chunk_coordinate(const double value) noexcept
{
    const double coordinate =
        std::floor(value / SpatialChunkIndex::chunk_size);
    return static_cast<std::int32_t>(std::clamp(
        coordinate,
        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

std::uint64_t covered_chunks(
    const std::int64_t min_x,
    const std::int64_t min_y,
    const std::int64_t max_x,
    const std::int64_t max_y) noexcept
{
    const auto width = static_cast<std::uint64_t>(
        std::max<std::int64_t>(max_x - min_x + 1, 1));
    const auto height = static_cast<std::uint64_t>(
        std::max<std::int64_t>(max_y - min_y + 1, 1));
    if (height != 0U
        && width > std::numeric_limits<std::uint64_t>::max() / height) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return width * height;
}

} // namespace

StrokeSegmentIndex::StrokeSegmentIndex(const Stroke& stroke)
{
    rebuild(stroke);
}

void StrokeSegmentIndex::rebuild(const Stroke& stroke)
{
    chunks_.clear();
    oversized_segments_.clear();
    segment_blocks_.clear();
    block_segment_count_ = 0U;
    std::size_t references = 0U;
    bool use_blocks = false;
    double total_length = 0.0;
    for (std::size_t index = 1U; index < stroke.points.size(); ++index) {
        const Vec2d first = stroke.points[index - 1U];
        const Vec2d second = stroke.points[index];
        total_length += std::hypot(
            second.x - first.x, second.y - first.y);
        if (use_blocks) continue;
        const auto min_x = static_cast<std::int64_t>(
            chunk_coordinate(std::min(first.x, second.x)));
        const auto min_y = static_cast<std::int64_t>(
            chunk_coordinate(std::min(first.y, second.y)));
        const auto max_x = static_cast<std::int64_t>(
            chunk_coordinate(std::max(first.x, second.x)));
        const auto max_y = static_cast<std::int64_t>(
            chunk_coordinate(std::max(first.y, second.y)));
        const auto covered = covered_chunks(min_x, min_y, max_x, max_y);
        references += covered > maximum_segment_chunks ? 1U : static_cast<std::size_t>(covered);
        if (references > maximum_index_references) {
            decltype(chunks_){}.swap(chunks_);
            std::vector<std::uint32_t>{}.swap(oversized_segments_);
            use_blocks = true;
            continue;
        }
        if (covered > maximum_segment_chunks) {
            oversized_segments_.push_back(
                static_cast<std::uint32_t>(index));
            continue;
        }
        for (std::int64_t y = min_y; y <= max_y; ++y) {
            for (std::int64_t x = min_x; x <= max_x; ++x) {
                chunks_[{
                    static_cast<std::int32_t>(x),
                    static_cast<std::int32_t>(y)}].push_back(
                        static_cast<std::uint32_t>(index));
            }
        }
    }
    if (use_blocks) {
        block_segment_count_ = stroke.points.size() - 1U;
        segment_blocks_.reserve((block_segment_count_ + segments_per_block - 1U) / segments_per_block);
        for (std::size_t first = 1U; first < stroke.points.size(); first += segments_per_block) {
            Aabb bounds = Aabb::from_points(stroke.points[first - 1U], stroke.points[first]);
            const auto end = std::min(stroke.points.size(), first + segments_per_block);
            for (std::size_t point = first + 1U; point < end; ++point) {
                bounds.min_x = std::min(bounds.min_x, stroke.points[point].x);
                bounds.min_y = std::min(bounds.min_y, stroke.points[point].y);
                bounds.max_x = std::max(bounds.max_x, stroke.points[point].x);
                bounds.max_y = std::max(bounds.max_y, stroke.points[point].y);
            }
            segment_blocks_.push_back(bounds);
        }
    }
    average_segment_length_ = stroke.points.size() > 1U
        ? std::max(
            total_length
                / static_cast<double>(stroke.points.size() - 1U),
            1.0e-6)
        : 1.0;
}

void StrokeSegmentIndex::query(
    const Aabb& bounds,
    std::vector<std::uint32_t>& result) const
{
    if (!segment_blocks_.empty()) {
        result.clear();
        for (std::size_t block = 0U; block < segment_blocks_.size(); ++block) {
            if (!segment_blocks_[block].intersects(bounds)) continue;
            const std::size_t first = block * segments_per_block + 1U;
            const auto end = std::min(block_segment_count_ + 1U, first + segments_per_block);
            for (std::size_t segment = first; segment < end; ++segment)
                result.push_back(static_cast<std::uint32_t>(segment));
        }
        return;
    }
    result = oversized_segments_;
    const auto min_x = static_cast<std::int64_t>(
        chunk_coordinate(bounds.min_x));
    const auto min_y = static_cast<std::int64_t>(
        chunk_coordinate(bounds.min_y));
    const auto max_x = static_cast<std::int64_t>(
        chunk_coordinate(bounds.max_x));
    const auto max_y = static_cast<std::int64_t>(
        chunk_coordinate(bounds.max_y));
    if (covered_chunks(min_x, min_y, max_x, max_y)
        <= maximum_direct_query_chunks) {
        for (std::int64_t y = min_y; y <= max_y; ++y) {
            for (std::int64_t x = min_x; x <= max_x; ++x) {
                const auto chunk = chunks_.find({
                    static_cast<std::int32_t>(x),
                    static_cast<std::int32_t>(y)});
                if (chunk != chunks_.end()) {
                    result.insert(
                        result.end(),
                        chunk->second.begin(),
                        chunk->second.end());
                }
            }
        }
    } else {
        for (const auto& [chunk, segments] : chunks_) {
            if (chunk.x >= min_x && chunk.x <= max_x
                && chunk.y >= min_y && chunk.y <= max_y) {
                result.insert(
                    result.end(), segments.begin(), segments.end());
            }
        }
    }
    std::ranges::sort(result);
    const auto unique_end = std::ranges::unique(result).begin();
    result.erase(unique_end, result.end());
}

std::size_t StrokeSegmentIndex::estimated_memory_bytes() const noexcept
{
    constexpr std::size_t node_links = 2U * sizeof(void*);
    std::size_t bytes = sizeof(*this)
        + chunks_.bucket_count() * sizeof(void*)
        + chunks_.size()
            * (sizeof(decltype(chunks_)::value_type) + node_links)
        + oversized_segments_.capacity() * sizeof(std::uint32_t)
        + segment_blocks_.capacity() * sizeof(Aabb);
    for (const auto& [key, segments] : chunks_) {
        static_cast<void>(key);
        bytes += segments.capacity() * sizeof(std::uint32_t);
    }
    return bytes;
}

double StrokeSegmentIndex::average_segment_length() const noexcept
{
    return average_segment_length_;
}

std::size_t StrokeSegmentIndex::ChunkKeyHash::operator()(
    const ChunkKey& key) const noexcept
{
    const auto x = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(key.x));
    const auto y = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(key.y));
    return static_cast<std::size_t>(
        (x * 0x9E3779B185EBCA87ULL) ^ (y + 0xC2B2AE3D27D4EB4FULL));
}

} // namespace sawer
