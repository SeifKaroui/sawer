#pragma once

#include "document/Object.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sawer {

class StrokeSegmentIndex final {
public:
    StrokeSegmentIndex() = default;
    explicit StrokeSegmentIndex(const Stroke& stroke);

    void rebuild(const Stroke& stroke);
    void query(const Aabb& bounds, std::vector<std::uint32_t>& result) const;
    [[nodiscard]] std::size_t estimated_memory_bytes() const noexcept;
    [[nodiscard]] double average_segment_length() const noexcept;

private:
    struct ChunkKey final {
        std::int32_t x{};
        std::int32_t y{};

        friend constexpr bool operator==(
            const ChunkKey&,
            const ChunkKey&) = default;
    };

    struct ChunkKeyHash final {
        [[nodiscard]] std::size_t operator()(const ChunkKey& key) const noexcept;
    };

    std::unordered_map<ChunkKey, std::vector<std::uint32_t>, ChunkKeyHash>
        chunks_;
    std::vector<std::uint32_t> oversized_segments_;
    // Pathological zigzags use conservative blocks instead of duplicating
    // every segment into dozens of chunks. No source-point pointers escape.
    std::vector<Aabb> segment_blocks_;
    std::size_t block_segment_count_{};
    double average_segment_length_{1.0};
};

} // namespace sawer
