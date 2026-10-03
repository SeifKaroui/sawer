#pragma once

#include "document/Document.hpp"

#include <optional>
#include <vector>

namespace sawer {

enum class SelectionHandle {
    none,
    line_start,
    line_end,
    top_left,
    top_right,
    bottom_right,
    bottom_left,
};

struct SelectionTransform final {
    Vec2d anchor;
    Vec2d translation;
    double scale_x{1.0};
    double scale_y{1.0};

    [[nodiscard]] Vec2d apply(Vec2d point) const noexcept;
    [[nodiscard]] Aabb apply(Aabb bounds) const noexcept;
    [[nodiscard]] Aabb inverse(Aabb bounds) const noexcept;
    [[nodiscard]] bool identity() const noexcept;
};

// Ephemeral edit state rendered above the unchanged document. Moving and
// group-resizing large strokes therefore do not copy their point arrays,
// update the spatial index, or invalidate retained geometry on every sample.
struct SelectionPreview final {
    SelectionPreview(
        std::vector<ObjectId> selected_ids,
        SelectionTransform selected_transform = {},
        std::optional<Object> selected_replacement = std::nullopt);

    std::vector<ObjectId> ids;
    SelectionTransform transform;
    std::optional<Object> replacement;

    [[nodiscard]] bool contains(ObjectId id) const noexcept;

private:
    std::vector<ObjectId> lookup_ids_;
};

class Selection final {
public:
    void clear() noexcept;
    void select(ObjectId id);
    void select(std::vector<ObjectId> ids);
    void toggle(ObjectId id);
    void prune(const Document& document);
    void set_marquee(std::optional<Aabb> bounds) noexcept;

    [[nodiscard]] bool contains(ObjectId id) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const std::vector<ObjectId>& ids() const noexcept;
    [[nodiscard]] std::optional<Aabb> bounds(const Document& document) const;
    [[nodiscard]] const std::optional<Aabb>& marquee() const noexcept;

private:
    std::vector<ObjectId> ids_;
    std::optional<Aabb> marquee_;
};

[[nodiscard]] std::optional<ObjectId> hit_test(
    const Document& document,
    Vec2d point,
    double tolerance);

[[nodiscard]] std::vector<ObjectId> marquee_hit_test(
    const Document& document,
    const Aabb& bounds);

[[nodiscard]] SelectionHandle selection_handle_at(
    const Document& document,
    const Selection& selection,
    Vec2d point,
    double tolerance);

void translate_object(Object& object, Vec2d delta);
void resize_object(
    Object& object,
    SelectionHandle handle,
    Vec2d point,
    bool preserve_proportions = false);
[[nodiscard]] SelectionTransform group_resize_transform(
    Aabb group_bounds,
    SelectionHandle handle,
    Vec2d point,
    bool preserve_aspect = false);
void resize_object_in_group(
    Object& object,
    Aabb group_bounds,
    SelectionHandle handle,
    Vec2d point,
    bool preserve_aspect = false);

} // namespace sawer
