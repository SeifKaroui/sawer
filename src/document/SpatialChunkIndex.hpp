#pragma once

#include "document/ObjectId.hpp"
#include "geometry/Geometry.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sawer {

class SpatialChunkIndex final {
public:
    static constexpr double chunk_size = 2'048.0;

    void insert(ObjectId id, const Aabb& bounds);
    void remove(ObjectId id);
    void update(ObjectId id, const Aabb& bounds);

    [[nodiscard]] std::vector<ObjectId> query(const Aabb& bounds) const;
    void query(const Aabb& bounds, std::vector<ObjectId>& result) const;
    [[nodiscard]] std::size_t chunk_count() const noexcept;
    [[nodiscard]] std::size_t oversized_count() const noexcept;
    [[nodiscard]] std::size_t estimated_memory_bytes() const noexcept;

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

    [[nodiscard]] static std::size_t covered_chunk_count(
        const Aabb& bounds) noexcept;

    static constexpr std::size_t maximum_object_chunks = 256U;
    static constexpr std::size_t maximum_direct_query_chunks = 4'096U;

    struct Membership final {
        std::int32_t min_x{};
        std::int32_t min_y{};
        std::uint16_t width{};
        std::uint16_t height{};
        mutable std::uint32_t query_generation{};
    };

    void begin_query_generation() const noexcept;
    void append_unique(
        ObjectId id,
        std::vector<ObjectId>& result) const;

    std::unordered_map<ChunkKey, std::vector<ObjectId>, ChunkKeyHash> chunks_;
    std::unordered_map<ObjectId, Membership, ObjectIdHash> membership_;
    std::unordered_map<ObjectId, Aabb, ObjectIdHash> oversized_;
    mutable std::uint32_t query_generation_{};
};

} // namespace sawer
