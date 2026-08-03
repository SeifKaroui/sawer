#pragma once

#include "document/Object.hpp"
#include "document/SpatialChunkIndex.hpp"
#include "document/StrokeSegmentIndex.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sawer {

class CommandHistory;

struct DocumentMemoryStats final {
    std::size_t object_count{};
    std::size_t stroke_point_count{};
    std::size_t stroke_point_capacity_bytes{};
    std::size_t object_storage_bytes{};
    std::size_t spatial_index_bytes{};
    std::size_t total_bytes{};
};

class Document final {
public:
    [[nodiscard]] bool insert(Object object);
    [[nodiscard]] std::optional<Object> remove(ObjectId id);
    [[nodiscard]] bool replace(Object object);
    // Replaces an object while transferring the prior state to the caller.
    // Commands use this to swap undo states without copying large strokes.
    [[nodiscard]] std::optional<Object> exchange(Object object);
    // Replaces only presentation state while returning the prior style.
    // Style commands use this path so changing a million-point stroke does
    // not copy or retain its geometry.
    [[nodiscard]] std::optional<Style> exchange_style(
        ObjectId id,
        Style style);
    [[nodiscard]] bool translate(ObjectId id, Vec2d delta);

    [[nodiscard]] const Object* find(ObjectId id) const noexcept;
    [[nodiscard]] std::vector<const Object*> query(const Aabb& bounds) const;
    void query(
        const Aabb& bounds,
        std::vector<const Object*>& result,
        std::vector<ObjectId>& id_scratch) const;
    [[nodiscard]] std::vector<const Object*> all_objects() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t spatial_chunk_count() const noexcept;
    [[nodiscard]] std::size_t oversized_object_count() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] DocumentMemoryStats memory_stats() const noexcept;
    void query_stroke_segments(
        ObjectId id,
        const Aabb& bounds,
        std::vector<std::uint32_t>& result) const;
    [[nodiscard]] const StrokeSegmentIndex* stroke_segment_index(
        ObjectId id,
        bool* built = nullptr) const;
    void clear_transient_caches() const noexcept;
    [[nodiscard]] std::int64_t next_z_order();
    [[nodiscard]] bool dirty() const noexcept;
    void clear() noexcept;

private:
    friend class CommandHistory;

    void erase_stroke_segment_index(ObjectId id) noexcept;
    void set_dirty(bool dirty) noexcept;

    std::unordered_map<ObjectId, Object, ObjectIdHash> objects_;
    SpatialChunkIndex spatial_index_;
    mutable std::unordered_map<
        ObjectId,
        StrokeSegmentIndex,
        ObjectIdHash> stroke_segment_indices_;
    mutable std::size_t stroke_segment_index_bytes_{};
    std::int64_t next_z_order_{};
    std::uint64_t revision_{};
    bool dirty_{};
};

} // namespace sawer
