#include "document/Object.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace sawer {

Object Object::make_line(
    ObjectId id,
    const std::int64_t z_order,
    const Line line,
    Style style)
{
    Object object{
        .id = id,
        .z_order = z_order,
        .style = style,
        .bounds = {},
        .geometry = line,
        .revision = 1U,
    };
    object.recompute_bounds();
    return object;
}

Object Object::make_stroke(
    ObjectId id,
    const std::int64_t z_order,
    Stroke stroke,
    Style style)
{
    Object object{
        .id = id,
        .z_order = z_order,
        .style = std::move(style),
        .bounds = {},
        .geometry = std::move(stroke),
        .revision = 1U,
    };
    object.recompute_bounds();
    return object;
}

Object Object::make_rectangle(
    ObjectId id,
    const std::int64_t z_order,
    const RectangleShape rectangle,
    Style style)
{
    Object object{
        .id = id,
        .z_order = z_order,
        .style = std::move(style),
        .bounds = {},
        .geometry = rectangle,
        .revision = 1U,
    };
    object.recompute_bounds();
    return object;
}

Object Object::make_ellipse(
    ObjectId id,
    const std::int64_t z_order,
    const Ellipse ellipse,
    Style style)
{
    Object object{
        .id = id,
        .z_order = z_order,
        .style = std::move(style),
        .bounds = {},
        .geometry = ellipse,
        .revision = 1U,
    };
    object.recompute_bounds();
    return object;
}

Object Object::make_image(
    ObjectId id,
    const std::int64_t z_order,
    Image image)
{
    if (!image.asset || image.asset->pixel_width == 0U
        || image.asset->pixel_height == 0U) {
        throw std::invalid_argument{"image requires a non-empty asset"};
    }
    Object object{
        .id = id,
        .z_order = z_order,
        .style = {},
        .bounds = {},
        .geometry = std::move(image),
        .revision = 1U,
    };
    object.recompute_bounds();
    return object;
}

void Object::translate(const Vec2d delta)
{
    std::visit(
        [&](auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, Line>) {
                shape.start.x += delta.x;
                shape.start.y += delta.y;
                shape.end.x += delta.x;
                shape.end.y += delta.y;
            } else if constexpr (std::is_same_v<Shape, Stroke>) {
                for (auto& point : shape.points) {
                    point.x += delta.x;
                    point.y += delta.y;
                }
            } else {
                shape.first.x += delta.x;
                shape.first.y += delta.y;
                shape.second.x += delta.x;
                shape.second.y += delta.y;
            }
        },
        geometry);
    recompute_bounds();
}

bool Object::within_board_bounds() const noexcept
{
    return can_translate({});
}

bool Object::can_translate(const Vec2d delta) const noexcept
{
    const double padding = std::holds_alternative<Image>(geometry)
        ? 0.0 : std::max(style.stroke_width, 0.0) * 0.5;
    return bounds.min_x + padding + delta.x >= -board_half_extent
        && bounds.min_y + padding + delta.y >= -board_half_extent
        && bounds.max_x - padding + delta.x <= board_half_extent
        && bounds.max_y - padding + delta.y <= board_half_extent;
}

void Object::recompute_bounds()
{
    std::visit(
        [this](const auto& shape) {
            const double padding = std::is_same_v<
                std::decay_t<decltype(shape)>, Image>
                ? 0.0 : std::max(style.stroke_width, 0.0) * 0.5;
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, Line>) {
                bounds = Aabb::from_points(
                    shape.start, shape.end, padding);
            } else if constexpr (std::is_same_v<Shape, Stroke>) {
                if (shape.points.empty()) {
                    bounds = {};
                    return;
                }
                bounds = Aabb::from_points(
                    shape.points.front(), shape.points.front(), padding);
                for (const auto point : shape.points) {
                    bounds.min_x = std::min(bounds.min_x, point.x - padding);
                    bounds.min_y = std::min(bounds.min_y, point.y - padding);
                    bounds.max_x = std::max(bounds.max_x, point.x + padding);
                    bounds.max_y = std::max(bounds.max_y, point.y + padding);
                }
            } else {
                bounds = Aabb::from_points(
                    shape.first, shape.second, padding);
            }
        },
        geometry);
}

} // namespace sawer
