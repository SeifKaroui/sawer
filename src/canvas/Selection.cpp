#include "canvas/Selection.hpp"

#include "canvas/Camera.hpp"
#include "geometry/StrokeProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace sawer {
namespace {

double segment_distance_squared(
    const Vec2d point,
    const Vec2d first,
    const Vec2d second)
{
    const double dx = second.x - first.x;
    const double dy = second.y - first.y;
    const double length_squared = dx * dx + dy * dy;
    if (length_squared <= 1.0e-18) {
        const double px = point.x - first.x;
        const double py = point.y - first.y;
        return px * px + py * py;
    }
    const double projection = std::clamp(
        ((point.x - first.x) * dx + (point.y - first.y) * dy)
            / length_squared,
        0.0,
        1.0);
    const double px = point.x - (first.x + projection * dx);
    const double py = point.y - (first.y + projection * dy);
    return px * px + py * py;
}

Aabb geometry_bounds(const Vec2d first, const Vec2d second) noexcept
{
    return Aabb::from_points(first, second);
}

double rounded_rectangle_signed_distance(
    const RectangleShape& rectangle,
    const Vec2d point) noexcept
{
    const Aabb bounds = geometry_bounds(rectangle.first, rectangle.second);
    const double half_width = (bounds.max_x - bounds.min_x) * 0.5;
    const double half_height = (bounds.max_y - bounds.min_y) * 0.5;
    const double radius = std::clamp(rectangle.roundness, 0.0, 0.5)
        * std::min(half_width * 2.0, half_height * 2.0);
    const Vec2d center{
        (bounds.min_x + bounds.max_x) * 0.5,
        (bounds.min_y + bounds.max_y) * 0.5,
    };
    const double x =
        std::abs(point.x - center.x) - (half_width - radius);
    const double y =
        std::abs(point.y - center.y) - (half_height - radius);
    return std::hypot(std::max(x, 0.0), std::max(y, 0.0))
        + std::min(std::max(x, y), 0.0) - radius;
}

bool hit_object(
    const Document& document,
    const Object& object,
    const Vec2d point,
    const double tolerance)
{
    const double radius = tolerance + object.style.stroke_width * 0.5;
    const double radius_squared = radius * radius;
    if (const auto* const line = std::get_if<Line>(&object.geometry)) {
        return segment_distance_squared(point, line->start, line->end)
            <= radius_squared;
    }
    if (const auto* const stroke = std::get_if<Stroke>(&object.geometry)) {
        if (stroke->points.empty()) {
            return false;
        }
        if (stroke->points.size() == 1U) {
            return segment_distance_squared(
                       point, stroke->points.front(), stroke->points.front())
                <= radius_squared;
        }
        std::vector<std::uint32_t> segments;
        document.query_stroke_segments(
            object.id,
            {
                point.x - radius,
                point.y - radius,
                point.x + radius,
                point.y + radius,
            },
            segments);
        for (const auto index : segments) {
            if (segment_distance_squared(
                    point, stroke->points[index - 1U], stroke->points[index])
                <= radius_squared) {
                return true;
            }
        }
        return false;
    }
    if (const auto* const rectangle =
            std::get_if<RectangleShape>(&object.geometry)) {
        const double distance =
            rounded_rectangle_signed_distance(*rectangle, point);
        if (object.style.fill.has_value() && distance <= 0.0) {
            return true;
        }
        return std::abs(distance) <= radius;
    }
    if (const auto* const image = std::get_if<Image>(&object.geometry)) {
        return geometry_bounds(image->first, image->second).intersects({
            point.x, point.y, point.x, point.y});
    }
    const auto* const ellipse = std::get_if<Ellipse>(&object.geometry);
    if (ellipse == nullptr) {
        return false;
    }
    const Aabb bounds = geometry_bounds(ellipse->first, ellipse->second);
    const double radius_x = (bounds.max_x - bounds.min_x) * 0.5;
    const double radius_y = (bounds.max_y - bounds.min_y) * 0.5;
    if (radius_x <= 1.0e-12 || radius_y <= 1.0e-12) {
        return segment_distance_squared(
                   point,
                   {bounds.min_x, bounds.min_y},
                   {bounds.max_x, bounds.max_y})
            <= radius_squared;
    }
    const double normalized_x =
        (point.x - (bounds.min_x + bounds.max_x) * 0.5) / radius_x;
    const double normalized_y =
        (point.y - (bounds.min_y + bounds.max_y) * 0.5) / radius_y;
    const double normalized_radius = std::hypot(normalized_x, normalized_y);
    if (object.style.fill.has_value() && normalized_radius <= 1.0) {
        return true;
    }
    return std::abs(normalized_radius - 1.0) * std::min(radius_x, radius_y)
        <= radius;
}

bool near(const Vec2d first, const Vec2d second, const double tolerance)
{
    return std::hypot(first.x - second.x, first.y - second.y) <= tolerance;
}

Vec2d clamped_board_point(const Vec2d point)
{
    constexpr double edge = Camera::board_half_extent;
    return {
        std::clamp(point.x, -edge, edge),
        std::clamp(point.y, -edge, edge),
    };
}

} // namespace

Vec2d SelectionTransform::apply(const Vec2d point) const noexcept
{
    return {
        anchor.x + (point.x - anchor.x) * scale_x + translation.x,
        anchor.y + (point.y - anchor.y) * scale_y + translation.y,
    };
}

Aabb SelectionTransform::apply(const Aabb bounds) const noexcept
{
    const Vec2d first = apply(Vec2d{bounds.min_x, bounds.min_y});
    const Vec2d second = apply(Vec2d{bounds.max_x, bounds.max_y});
    return Aabb::from_points(first, second);
}

Aabb SelectionTransform::inverse(const Aabb bounds) const noexcept
{
    if (std::abs(scale_x) <= 1.0e-12
        || std::abs(scale_y) <= 1.0e-12) {
        return bounds;
    }
    const auto inverse_point = [&](const Vec2d point) {
        return Vec2d{
            anchor.x
                + (point.x - anchor.x - translation.x) / scale_x,
            anchor.y
                + (point.y - anchor.y - translation.y) / scale_y,
        };
    };
    return Aabb::from_points(
        inverse_point({bounds.min_x, bounds.min_y}),
        inverse_point({bounds.max_x, bounds.max_y}));
}

bool SelectionTransform::identity() const noexcept
{
    return scale_x == 1.0 && scale_y == 1.0
        && translation == Vec2d{};
}

SelectionPreview::SelectionPreview(
    std::vector<ObjectId> selected_ids,
    SelectionTransform selected_transform,
    std::optional<Object> selected_replacement)
    : ids{std::move(selected_ids)}
    , transform{selected_transform}
    , replacement{std::move(selected_replacement)}
    , lookup_ids_{ids}
{
    std::ranges::sort(lookup_ids_);
    const auto unique_end = std::ranges::unique(lookup_ids_).begin();
    lookup_ids_.erase(unique_end, lookup_ids_.end());
}

bool SelectionPreview::contains(const ObjectId id) const noexcept
{
    return std::ranges::binary_search(lookup_ids_, id);
}

void Selection::clear() noexcept
{
    ids_.clear();
    marquee_.reset();
}

void Selection::select(const ObjectId id)
{
    ids_ = {id};
    marquee_.reset();
}

void Selection::select(std::vector<ObjectId> ids)
{
    ids_ = std::move(ids);
    std::ranges::sort(ids_);
    const auto unique_end = std::ranges::unique(ids_).begin();
    ids_.erase(unique_end, ids_.end());
    marquee_.reset();
}

void Selection::prune(const Document& document)
{
    std::erase_if(ids_, [&](const ObjectId id) {
        return document.find(id) == nullptr;
    });
}

void Selection::set_marquee(const std::optional<Aabb> bounds) noexcept
{
    marquee_ = bounds;
}

bool Selection::contains(const ObjectId id) const noexcept
{
    return std::ranges::find(ids_, id) != ids_.end();
}

bool Selection::empty() const noexcept
{
    return ids_.empty();
}

const std::vector<ObjectId>& Selection::ids() const noexcept
{
    return ids_;
}

std::optional<Aabb> Selection::bounds(const Document& document) const
{
    std::optional<Aabb> result;
    for (const auto id : ids_) {
        const Object* const object = document.find(id);
        if (object == nullptr) {
            continue;
        }
        if (!result.has_value()) {
            result = object->bounds;
        } else {
            result->min_x = std::min(result->min_x, object->bounds.min_x);
            result->min_y = std::min(result->min_y, object->bounds.min_y);
            result->max_x = std::max(result->max_x, object->bounds.max_x);
            result->max_y = std::max(result->max_y, object->bounds.max_y);
        }
    }
    return result;
}

const std::optional<Aabb>& Selection::marquee() const noexcept
{
    return marquee_;
}

std::optional<ObjectId> hit_test(
    const Document& document,
    const Vec2d point,
    const double tolerance)
{
    const Aabb query{
        point.x - tolerance,
        point.y - tolerance,
        point.x + tolerance,
        point.y + tolerance,
    };
    const auto candidates = document.query(query);
    for (auto iterator = candidates.rbegin(); iterator != candidates.rend();
         ++iterator) {
        if (hit_object(document, **iterator, point, tolerance)) {
            return (*iterator)->id;
        }
    }
    return std::nullopt;
}

std::vector<ObjectId> marquee_hit_test(
    const Document& document,
    const Aabb& bounds)
{
    std::vector<ObjectId> ids;
    for (const Object* const object : document.query(bounds)) {
        if (object->bounds.min_x >= bounds.min_x
            && object->bounds.min_y >= bounds.min_y
            && object->bounds.max_x <= bounds.max_x
            && object->bounds.max_y <= bounds.max_y) {
            ids.push_back(object->id);
        }
    }
    return ids;
}

SelectionHandle selection_handle_at(
    const Document& document,
    const Selection& selection,
    const Vec2d point,
    const double tolerance)
{
    if (selection.empty()) {
        return SelectionHandle::none;
    }
    if (selection.ids().size() > 1U) {
        const auto bounds = selection.bounds(document);
        if (!bounds.has_value()) {
            return SelectionHandle::none;
        }
        if (near(point, {bounds->min_x, bounds->min_y}, tolerance)) {
            return SelectionHandle::top_left;
        }
        if (near(point, {bounds->max_x, bounds->min_y}, tolerance)) {
            return SelectionHandle::top_right;
        }
        if (near(point, {bounds->max_x, bounds->max_y}, tolerance)) {
            return SelectionHandle::bottom_right;
        }
        if (near(point, {bounds->min_x, bounds->max_y}, tolerance)) {
            return SelectionHandle::bottom_left;
        }
        return SelectionHandle::none;
    }
    const Object* const object = document.find(selection.ids().front());
    if (object == nullptr) {
        return SelectionHandle::none;
    }
    if (const auto* const line = std::get_if<Line>(&object->geometry)) {
        if (near(point, line->start, tolerance)) return SelectionHandle::line_start;
        if (near(point, line->end, tolerance)) return SelectionHandle::line_end;
        return SelectionHandle::none;
    }
    Vec2d first;
    Vec2d second;
    if (const auto* const rectangle =
            std::get_if<RectangleShape>(&object->geometry)) {
        first = rectangle->first;
        second = rectangle->second;
    } else if (const auto* const ellipse =
                   std::get_if<Ellipse>(&object->geometry)) {
        first = ellipse->first;
        second = ellipse->second;
    } else if (const auto* const image = std::get_if<Image>(&object->geometry)) {
        first = image->first;
        second = image->second;
    } else {
        return SelectionHandle::none;
    }
    const Aabb bounds = geometry_bounds(first, second);
    if (near(point, {bounds.min_x, bounds.min_y}, tolerance)) return SelectionHandle::top_left;
    if (near(point, {bounds.max_x, bounds.min_y}, tolerance)) return SelectionHandle::top_right;
    if (near(point, {bounds.max_x, bounds.max_y}, tolerance)) return SelectionHandle::bottom_right;
    if (near(point, {bounds.min_x, bounds.max_y}, tolerance)) return SelectionHandle::bottom_left;
    return SelectionHandle::none;
}

void translate_object(Object& object, const Vec2d delta)
{
    object.translate(delta);
}

void resize_object(
    Object& object,
    const SelectionHandle handle,
    const Vec2d requested_point,
    const bool preserve_proportions)
{
    if (auto* const line = std::get_if<Line>(&object.geometry)) {
        if (handle == SelectionHandle::line_start) {
            line->start = constrain_line_endpoint(
                line->end, requested_point, preserve_proportions);
        }
        if (handle == SelectionHandle::line_end) {
            line->end = constrain_line_endpoint(
                line->start, requested_point, preserve_proportions);
        }
        object.recompute_bounds();
        return;
    }

    Vec2d* first = nullptr;
    Vec2d* second = nullptr;
    if (auto* const rectangle = std::get_if<RectangleShape>(&object.geometry)) {
        first = &rectangle->first;
        second = &rectangle->second;
    } else if (auto* const ellipse = std::get_if<Ellipse>(&object.geometry)) {
        first = &ellipse->first;
        second = &ellipse->second;
    } else if (auto* const image = std::get_if<Image>(&object.geometry)) {
        first = &image->first;
        second = &image->second;
    }
    if (first == nullptr || second == nullptr) {
        return;
    }

    const Image* const image = std::get_if<Image>(&object.geometry);
    const Aabb bounds = geometry_bounds(*first, *second);
    Vec2d anchor;
    switch (handle) {
    case SelectionHandle::top_left:
        anchor = {bounds.max_x, bounds.max_y};
        break;
    case SelectionHandle::top_right:
        anchor = {bounds.min_x, bounds.max_y};
        break;
    case SelectionHandle::bottom_right:
        anchor = {bounds.min_x, bounds.min_y};
        break;
    case SelectionHandle::bottom_left:
        anchor = {bounds.max_x, bounds.min_y};
        break;
    default:
        return;
    }
    Vec2d point = constrain_shape_endpoint(
        anchor, requested_point, preserve_proportions);
    if (preserve_proportions && image != nullptr) {
        const double width = std::abs(second->x - first->x);
        const double height = std::abs(second->y - first->y);
        if (width > 1.0e-12 && height > 1.0e-12) {
            const double requested_x = requested_point.x - anchor.x;
            const double requested_y = requested_point.y - anchor.y;
            const double scale = std::max(
                std::abs(requested_x) / width,
                std::abs(requested_y) / height);
            point = {
                anchor.x + std::copysign(width * scale, requested_x),
                anchor.y + std::copysign(height * scale, requested_y),
            };
        }
    }
    switch (handle) {
    case SelectionHandle::top_left:
        *first = point;
        *second = {bounds.max_x, bounds.max_y};
        break;
    case SelectionHandle::top_right:
        *first = {bounds.min_x, bounds.max_y};
        *second = point;
        break;
    case SelectionHandle::bottom_right:
        *first = {bounds.min_x, bounds.min_y};
        *second = point;
        break;
    case SelectionHandle::bottom_left:
        *first = {bounds.max_x, bounds.min_y};
        *second = point;
        break;
    default: break;
    }
    object.recompute_bounds();
}

void resize_object_in_group(
    Object& object,
    const Aabb group_bounds,
    const SelectionHandle handle,
    const Vec2d requested_point,
    const bool preserve_aspect)
{
    const SelectionTransform group_transform = group_resize_transform(
        group_bounds, handle, requested_point, preserve_aspect);
    if (group_transform.identity()) {
        return;
    }
    const auto transform = [&](Vec2d& value) {
        value = group_transform.apply(value);
    };
    std::visit(
        [&](auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, Line>) {
                transform(shape.start);
                transform(shape.end);
            } else if constexpr (std::is_same_v<Shape, Stroke>) {
                for (auto& stroke_point : shape.points) {
                    transform(stroke_point);
                }
            } else {
                transform(shape.first);
                transform(shape.second);
            }
        },
        object.geometry);
    object.recompute_bounds();
}

SelectionTransform group_resize_transform(
    const Aabb group_bounds,
    const SelectionHandle handle,
    const Vec2d requested_point,
    const bool preserve_aspect)
{
    const Vec2d point = clamped_board_point(requested_point);
    Vec2d anchor;
    Vec2d moving_corner;
    switch (handle) {
    case SelectionHandle::top_left:
        anchor = {group_bounds.max_x, group_bounds.max_y};
        moving_corner = {group_bounds.min_x, group_bounds.min_y};
        break;
    case SelectionHandle::top_right:
        anchor = {group_bounds.min_x, group_bounds.max_y};
        moving_corner = {group_bounds.max_x, group_bounds.min_y};
        break;
    case SelectionHandle::bottom_right:
        anchor = {group_bounds.min_x, group_bounds.min_y};
        moving_corner = {group_bounds.max_x, group_bounds.max_y};
        break;
    case SelectionHandle::bottom_left:
        anchor = {group_bounds.max_x, group_bounds.min_y};
        moving_corner = {group_bounds.min_x, group_bounds.max_y};
        break;
    default:
        return {};
    }

    const double width = moving_corner.x - anchor.x;
    const double height = moving_corner.y - anchor.y;
    double scale_x = std::abs(width) <= 1.0e-12
        ? 1.0
        : (point.x - anchor.x) / width;
    double scale_y = std::abs(height) <= 1.0e-12
        ? 1.0
        : (point.y - anchor.y) / height;
    if (preserve_aspect) {
        const double diagonal_squared = width * width + height * height;
        if (diagonal_squared > 1.0e-18) {
            // Project the requested corner onto the original diagonal. One
            // uniform scale keeps the group's original width-to-height ratio
            // and makes the constrained handle movement stable in any corner.
            const double uniform_scale =
                ((point.x - anchor.x) * width
                    + (point.y - anchor.y) * height)
                / diagonal_squared;
            scale_x = uniform_scale;
            scale_y = uniform_scale;
        }
    }
    return {
        .anchor = anchor,
        .translation = {},
        .scale_x = scale_x,
        .scale_y = scale_y,
    };
}

} // namespace sawer
