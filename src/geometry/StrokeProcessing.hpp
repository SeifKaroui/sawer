#pragma once

#include "geometry/Geometry.hpp"

#include <cstddef>
#include <vector>

namespace sawer {

inline constexpr std::size_t maximum_stroke_samples = 1'000'000U;
inline constexpr std::size_t maximum_processed_stroke_points =
    maximum_stroke_point_count;

// Builds the visible and stored pencil path together. Completed curve
// segments are never revisited; only the short uncommitted tail is replaced
// when a new point arrives. finish() therefore has bounded work and leaves the
// exact geometry that was last shown by the live preview.
class IncrementalStrokeCurve final {
public:
    void reset(Vec2d initial_point, double flatness_tolerance);

    [[nodiscard]] bool push(
        Vec2d point,
        std::vector<Vec2d>& output);

    [[nodiscard]] bool finish(
        Vec2d endpoint,
        std::vector<Vec2d>& output);

    [[nodiscard]] std::size_t stable_point_count() const noexcept;
    [[nodiscard]] bool finished() const noexcept;

private:
    void append_preview(std::vector<Vec2d>& output) const;

    Vec2d before_{};
    Vec2d start_{};
    Vec2d end_{};
    double flatness_tolerance_{0.05};
    std::size_t stable_point_count_{1U};
    std::size_t knot_count_{};
    bool finished_{};
};

[[nodiscard]] bool append_filtered_point(
    std::vector<Vec2d>& points,
    Vec2d point,
    double minimum_distance);

[[nodiscard]] std::vector<Vec2d> simplify_stroke(
    const std::vector<Vec2d>& points,
    double tolerance);

void smooth_stroke_points(
    std::vector<Vec2d>& points,
    std::size_t radius);

[[nodiscard]] Vec2d trailing_smoothed_stroke_point(
    const std::vector<Vec2d>& points,
    std::size_t radius) noexcept;

[[nodiscard]] std::vector<Vec2d> interpolate_stroke_curve(
    const std::vector<Vec2d>& points,
    double flatness_tolerance);

void interpolate_stroke_curve(
    const std::vector<Vec2d>& points,
    double flatness_tolerance,
    std::vector<Vec2d>& output);

void append_smooth_tail(
    std::vector<Vec2d>& points,
    Vec2d endpoint,
    double flatness_tolerance);

// Produces the canonical points stored in a completed pencil stroke. Keeping
// this platform-neutral completion path in geometry code ensures that input
// batches from Windows, Wayland, and X11 all receive the same smoothing,
// simplification, interpolation, and board-bound clamping.
[[nodiscard]] std::vector<Vec2d> complete_stroke_points(
    std::vector<Vec2d> points,
    Vec2d release_point,
    double zoom,
    std::size_t smoothing_radius,
    bool stabilized);

[[nodiscard]] Vec2d constrain_line_endpoint(
    Vec2d start,
    Vec2d end,
    bool constrained);

[[nodiscard]] Vec2d constrain_shape_endpoint(
    Vec2d start,
    Vec2d end,
    bool constrained);

} // namespace sawer
