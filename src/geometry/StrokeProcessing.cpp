#include "geometry/StrokeProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace sawer {
namespace {

double squared_distance(const Vec2d first, const Vec2d second) noexcept
{
    const double x = second.x - first.x;
    const double y = second.y - first.y;
    return x * x + y * y;
}

double point_segment_distance(
    const Vec2d point,
    const Vec2d start,
    const Vec2d end) noexcept
{
    const double length_squared = squared_distance(start, end);
    if (length_squared <= 1.0e-18) {
        return std::sqrt(squared_distance(point, start));
    }

    const double projection = std::clamp(
        ((point.x - start.x) * (end.x - start.x)
         + (point.y - start.y) * (end.y - start.y))
            / length_squared,
        0.0,
        1.0);
    const Vec2d closest{
        start.x + projection * (end.x - start.x),
        start.y + projection * (end.y - start.y),
    };
    return std::sqrt(squared_distance(point, closest));
}

void flatten_quadratic(
    std::vector<Vec2d>& output,
    const Vec2d start,
    const Vec2d control,
    const Vec2d end,
    const double tolerance_squared,
    const int depth)
{
    if (output.size() >= maximum_processed_stroke_points) {
        return;
    }
    const Vec2d first_midpoint{
        (start.x + control.x) * 0.5,
        (start.y + control.y) * 0.5,
    };
    const Vec2d second_midpoint{
        (control.x + end.x) * 0.5,
        (control.y + end.y) * 0.5,
    };
    const Vec2d curve_midpoint{
        (first_midpoint.x + second_midpoint.x) * 0.5,
        (first_midpoint.y + second_midpoint.y) * 0.5,
    };
    const Vec2d chord_midpoint{
        (start.x + end.x) * 0.5,
        (start.y + end.y) * 0.5,
    };
    if (depth >= 12
        || squared_distance(curve_midpoint, chord_midpoint)
            <= tolerance_squared) {
        output.push_back(end);
        return;
    }
    flatten_quadratic(
        output, start, first_midpoint, curve_midpoint,
        tolerance_squared, depth + 1);
    flatten_quadratic(
        output, curve_midpoint, second_midpoint, end,
        tolerance_squared, depth + 1);
}

double point_line_distance_squared(
    const Vec2d point, const Vec2d start, const Vec2d end) noexcept
{
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double length_squared = dx * dx + dy * dy;
    if (length_squared <= 1.0e-18) {
        return squared_distance(point, start);
    }
    const double cross =
        (point.x - start.x) * dy - (point.y - start.y) * dx;
    return cross * cross / length_squared;
}

void flatten_cubic(
    std::vector<Vec2d>& output,
    const Vec2d start,
    const Vec2d first_control,
    const Vec2d second_control,
    const Vec2d end,
    const double tolerance_squared,
    const int depth)
{
    if (output.size() >= maximum_processed_stroke_points) {
        return;
    }
    if (depth >= 12
        || (point_line_distance_squared(first_control, start, end)
                <= tolerance_squared
            && point_line_distance_squared(second_control, start, end)
                <= tolerance_squared)) {
        output.push_back(end);
        return;
    }

    const Vec2d a{
        (start.x + first_control.x) * 0.5,
        (start.y + first_control.y) * 0.5,
    };
    const Vec2d b{
        (first_control.x + second_control.x) * 0.5,
        (first_control.y + second_control.y) * 0.5,
    };
    const Vec2d c{
        (second_control.x + end.x) * 0.5,
        (second_control.y + end.y) * 0.5,
    };
    const Vec2d d{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
    const Vec2d e{(b.x + c.x) * 0.5, (b.y + c.y) * 0.5};
    const Vec2d midpoint{(d.x + e.x) * 0.5, (d.y + e.y) * 0.5};
    flatten_cubic(
        output, start, a, d, midpoint, tolerance_squared, depth + 1);
    flatten_cubic(
        output, midpoint, e, c, end, tolerance_squared, depth + 1);
}

Vec2d curve_tangent(
    const std::vector<Vec2d>& points, const std::size_t index) noexcept
{
    if (index == 0U) {
        return {
            points[1U].x - points[0U].x,
            points[1U].y - points[0U].y,
        };
    }
    if (index + 1U == points.size()) {
        return {
            points[index].x - points[index - 1U].x,
            points[index].y - points[index - 1U].y,
        };
    }

    const double before = std::hypot(
        points[index].x - points[index - 1U].x,
        points[index].y - points[index - 1U].y);
    const double after = std::hypot(
        points[index + 1U].x - points[index].x,
        points[index + 1U].y - points[index].y);
    const double span_x = points[index + 1U].x - points[index - 1U].x;
    const double span_y = points[index + 1U].y - points[index - 1U].y;
    const double span = std::hypot(span_x, span_y);
    if (span <= 1.0e-9 || before <= 1.0e-9 || after <= 1.0e-9) {
        return {};
    }
    const double magnitude = std::min(before, after);
    return {span_x / span * magnitude, span_y / span * magnitude};
}

Vec2d clamp_board_point(const Vec2d point) noexcept
{
    return {
        std::clamp(point.x, -board_half_extent, board_half_extent),
        std::clamp(point.y, -board_half_extent, board_half_extent),
    };
}

Vec2d clamp_ray_to_board(
    const Vec2d start,
    const Vec2d endpoint) noexcept
{
    const Vec2d delta{endpoint.x - start.x, endpoint.y - start.y};
    double scale = 1.0;
    if (delta.x > 0.0) {
        scale = std::min(
            scale, (board_half_extent - start.x) / delta.x);
    } else if (delta.x < 0.0) {
        scale = std::min(
            scale, (-board_half_extent - start.x) / delta.x);
    }
    if (delta.y > 0.0) {
        scale = std::min(
            scale, (board_half_extent - start.y) / delta.y);
    } else if (delta.y < 0.0) {
        scale = std::min(
            scale, (-board_half_extent - start.y) / delta.y);
    }
    scale = std::clamp(scale, 0.0, 1.0);
    return {
        start.x + delta.x * scale,
        start.y + delta.y * scale,
    };
}

} // namespace

bool append_filtered_point(
    std::vector<Vec2d>& points,
    const Vec2d point,
    const double minimum_distance)
{
    if (!points.empty()
        && squared_distance(points.back(), point)
            < minimum_distance * minimum_distance) {
        return false;
    }
    points.push_back(point);
    return true;
}

void smooth_stroke_points(
    std::vector<Vec2d>& points,
    const std::size_t radius)
{
    if (radius == 0U
        || points.size() <= radius * 2U + 2U) {
        return;
    }

    const std::vector<Vec2d> source = points;
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (std::size_t index = 0U; index <= radius * 2U; ++index) {
        sum_x += source[index].x;
        sum_y += source[index].y;
    }

    const double count = static_cast<double>(radius * 2U + 1U);
    for (std::size_t index = radius;
         index + radius < source.size();
         ++index) {
        points[index] = {sum_x / count, sum_y / count};
        if (index + radius + 1U < source.size()) {
            const std::size_t removed = index - radius;
            const std::size_t added = index + radius + 1U;
            sum_x += source[added].x - source[removed].x;
            sum_y += source[added].y - source[removed].y;
        }
    }

    // The physical mouse-down and mouse-up positions are document semantics,
    // not noise, and must remain exact.
    points.front() = source.front();
    points.back() = source.back();
}

Vec2d trailing_smoothed_stroke_point(
    const std::vector<Vec2d>& points,
    const std::size_t radius) noexcept
{
    if (points.empty()) {
        return {};
    }
    if (radius == 0U) {
        return points.back();
    }

    const std::size_t window = radius * 2U + 1U;
    const std::size_t first =
        points.size() > window ? points.size() - window : 0U;
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (std::size_t index = first; index < points.size(); ++index) {
        sum_x += points[index].x;
        sum_y += points[index].y;
    }
    const double count = static_cast<double>(points.size() - first);
    return {sum_x / count, sum_y / count};
}

std::vector<Vec2d> simplify_stroke(
    const std::vector<Vec2d>& points,
    const double tolerance)
{
    if (points.size() < 3U || tolerance <= 0.0) {
        return points;
    }

    std::vector<bool> retained(points.size(), false);
    retained.front() = true;
    retained.back() = true;
    struct Range final {
        std::size_t first{};
        std::size_t last{};
    };
    std::vector<Range> pending{{0U, points.size() - 1U}};
    // Pathological zig-zags can make recursive Douglas-Peucker quadratic and
    // overflow the call stack. Bound the comparisons; if the budget is
    // exhausted, retain unresolved spans exactly instead of freezing or
    // corrupting the stroke.
    const std::size_t comparison_budget =
        points.size() > std::numeric_limits<std::size_t>::max() / 16U
        ? std::numeric_limits<std::size_t>::max()
        : points.size() * 16U;
    std::size_t comparisons = 0U;
    while (!pending.empty()) {
        const Range range = pending.back();
        pending.pop_back();
        if (range.last <= range.first + 1U) {
            continue;
        }
        if (comparisons >= comparison_budget) {
            for (std::size_t index = range.first + 1U;
                 index < range.last; ++index) {
                retained[index] = true;
            }
            continue;
        }

        double maximum_distance = 0.0;
        std::size_t maximum_index = range.first;
        for (std::size_t index = range.first + 1U;
             index < range.last; ++index) {
            const double distance = point_segment_distance(
                points[index], points[range.first], points[range.last]);
            if (distance > maximum_distance) {
                maximum_distance = distance;
                maximum_index = index;
            }
            ++comparisons;
        }
        if (maximum_distance > tolerance) {
            retained[maximum_index] = true;
            pending.push_back({range.first, maximum_index});
            pending.push_back({maximum_index, range.last});
        }
    }

    std::vector<Vec2d> simplified;
    simplified.reserve(points.size());
    for (std::size_t index = 0U; index < points.size(); ++index) {
        if (retained[index]) {
            simplified.push_back(points[index]);
        }
    }
    return simplified;
}

std::vector<Vec2d> interpolate_stroke_curve(
    const std::vector<Vec2d>& points,
    const double flatness_tolerance)
{
    std::vector<Vec2d> curve;
    interpolate_stroke_curve(points, flatness_tolerance, curve);
    return curve;
}

void interpolate_stroke_curve(
    const std::vector<Vec2d>& points,
    const double flatness_tolerance,
    std::vector<Vec2d>& curve)
{
    curve.clear();
    if (points.size() < 3U) {
        curve.insert(curve.end(), points.begin(), points.end());
        return;
    }
    const double tolerance = std::max(flatness_tolerance, 1.0e-4);
    const std::size_t requested_capacity =
        points.size() > maximum_processed_stroke_points / 2U
        ? maximum_processed_stroke_points
        : points.size() * 2U;
    if (curve.capacity() < requested_capacity) {
        curve.reserve(requested_capacity);
    }
    curve.push_back(points.front());
    for (std::size_t index = 0U; index + 1U < points.size(); ++index) {
        const Vec2d start = points[index];
        const Vec2d end = points[index + 1U];
        if (squared_distance(start, end) <= 1.0e-18) {
            continue;
        }
        const Vec2d start_tangent = curve_tangent(points, index);
        const Vec2d end_tangent = curve_tangent(points, index + 1U);
        const Vec2d first_control{
            start.x + start_tangent.x / 3.0,
            start.y + start_tangent.y / 3.0,
        };
        const Vec2d second_control{
            end.x - end_tangent.x / 3.0,
            end.y - end_tangent.y / 3.0,
        };
        flatten_cubic(
            curve, start, first_control, second_control, end,
            tolerance * tolerance, 0);
        if (curve.size() >= maximum_processed_stroke_points) {
            curve.back() = points.back();
            break;
        }
    }
}

void append_smooth_tail(
    std::vector<Vec2d>& points,
    const Vec2d endpoint,
    const double flatness_tolerance)
{
    if (points.empty()) {
        points.push_back(endpoint);
        return;
    }
    if (points.size() >= maximum_processed_stroke_points) {
        points.back() = endpoint;
        return;
    }
    const Vec2d start = points.back();
    const double gap_x = endpoint.x - start.x;
    const double gap_y = endpoint.y - start.y;
    const double gap = std::hypot(gap_x, gap_y);
    if (gap <= 1.0e-9) {
        return;
    }
    if (points.size() < 2U) {
        points.push_back(endpoint);
        return;
    }

    const Vec2d previous = points[points.size() - 2U];
    const double tangent_x = start.x - previous.x;
    const double tangent_y = start.y - previous.y;
    const double tangent_length = std::hypot(tangent_x, tangent_y);
    const double forward = tangent_x * gap_x + tangent_y * gap_y;
    if (tangent_length <= 1.0e-9 || forward <= 0.0) {
        // A reversal is a deliberate cusp, so do not round it into a loop.
        points.push_back(endpoint);
        return;
    }

    const double handle = std::min(tangent_length, gap) * 0.5;
    const Vec2d control{
        start.x + tangent_x / tangent_length * handle,
        start.y + tangent_y / tangent_length * handle,
    };
    const double tolerance = std::max(flatness_tolerance, 1.0e-4);
    flatten_quadratic(
        points, start, control, endpoint, tolerance * tolerance, 0);
}

std::vector<Vec2d> complete_stroke_points(
    std::vector<Vec2d> points,
    const Vec2d release_point,
    const double zoom,
    const std::size_t smoothing_radius,
    const bool stabilized)
{
    const double safe_zoom = std::max(zoom, 1.0e-9);
    if (stabilized) {
        // The velocity filter intentionally trails the pointer. Preserve its
        // tangent while joining it to the physical mouse-up position.
        append_smooth_tail(points, release_point, 0.05 / safe_zoom);
    }
    smooth_stroke_points(points, smoothing_radius);

    auto simplified = simplify_stroke(points, 0.15 / safe_zoom);
    auto curved = interpolate_stroke_curve(
        simplified, std::min(0.05, 0.12 / safe_zoom));
    for (auto& point : curved) {
        point.x = std::clamp(
            point.x, -board_half_extent, board_half_extent);
        point.y = std::clamp(
            point.y, -board_half_extent, board_half_extent);
    }
    return curved;
}

Vec2d constrain_line_endpoint(
    const Vec2d start,
    const Vec2d end,
    const bool constrained)
{
    const Vec2d clamped_end = clamp_board_point(end);
    if (!constrained) {
        return clamped_end;
    }

    const double delta_x = clamped_end.x - start.x;
    const double delta_y = clamped_end.y - start.y;
    const double length = std::hypot(delta_x, delta_y);
    if (length <= 1.0e-12) {
        return clamped_end;
    }

    constexpr double angle_step = 3.14159265358979323846 / 4.0;
    const double angle =
        std::round(std::atan2(delta_y, delta_x) / angle_step) * angle_step;
    return clamp_ray_to_board(start, {
        start.x + std::cos(angle) * length,
        start.y + std::sin(angle) * length,
    });
}

Vec2d constrain_shape_endpoint(
    const Vec2d start,
    const Vec2d end,
    const bool constrained)
{
    const Vec2d clamped_end = clamp_board_point(end);
    if (!constrained) {
        return clamped_end;
    }

    const double delta_x = clamped_end.x - start.x;
    const double delta_y = clamped_end.y - start.y;
    double extent = std::max(std::abs(delta_x), std::abs(delta_y));
    const double available_x = delta_x < 0.0
        ? start.x + board_half_extent
        : board_half_extent - start.x;
    const double available_y = delta_y < 0.0
        ? start.y + board_half_extent
        : board_half_extent - start.y;
    extent = std::min({extent, available_x, available_y});
    return {
        start.x + std::copysign(extent, delta_x == 0.0 ? 1.0 : delta_x),
        start.y + std::copysign(extent, delta_y == 0.0 ? 1.0 : delta_y),
    };
}

} // namespace sawer
