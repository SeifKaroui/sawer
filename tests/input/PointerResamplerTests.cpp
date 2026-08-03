#include "geometry/StrokeProcessing.hpp"
#include "input/DrawingSettings.hpp"
#include "input/PointerResampler.hpp"
#include "input/VelocityGaussianStabilizer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

sawer::PointerSample sample(
    const double x,
    const std::uint64_t timestamp) noexcept
{
    return {
        .source = sawer::PointerSource::mouse,
        .position = {x, 0.0},
        .pressure = 1.0F,
        .timestamp = timestamp,
        .buttons = sawer::PointerButtons::primary,
    };
}

std::vector<sawer::PointerSample> resample(
    const std::vector<sawer::PointerSample>& events)
{
    sawer::PointerResampler resampler;
    resampler.reset(events.front());
    std::vector<sawer::PointerSample> output{events.front()};
    for (std::size_t index = 1U; index < events.size(); ++index) {
        const auto generated = resampler.push(
            events[index], 1.0, index + 1U == events.size());
        output.insert(output.end(), generated.begin(), generated.end());
    }
    return output;
}

std::vector<sawer::Vec2d> stabilize(
    const std::vector<sawer::PointerSample>& events)
{
    const auto resampled = resample(events);
    sawer::VelocityGaussianStabilizer stabilizer;
    stabilizer.reset(0.5, resampled.front(), 1.0);
    std::vector<sawer::Vec2d> output{resampled.front().position};
    for (std::size_t index = 1U; index < resampled.size(); ++index) {
        output.push_back(stabilizer.push(resampled[index]).position);
    }
    return output;
}

std::vector<sawer::Vec2d> complete_stabilized_stroke(
    const std::vector<sawer::PointerSample>& events)
{
    const sawer::DrawingSettings settings;
    const auto resampled = resample(events);
    sawer::VelocityGaussianStabilizer stabilizer;
    stabilizer.reset(
        settings.gaussian_sigma(), resampled.front(), 1.0);

    std::vector<sawer::Vec2d> points{resampled.front().position};
    for (std::size_t index = 1U; index < resampled.size(); ++index) {
        const auto stabilized = stabilizer.push(resampled[index]);
        static_cast<void>(sawer::append_filtered_point(
            points,
            stabilized.position,
            settings.sampling_distance()));
    }
    return sawer::complete_stroke_points(
        std::move(points),
        events.back().position,
        1.0,
        settings.completion_smoothing_radius(),
        true);
}

std::vector<sawer::PointerSample> segmented_trace(
    const std::size_t subdivisions)
{
    const std::vector<sawer::Vec2d> corners{
        {0.0, 0.0},
        {20.0, 0.0},
        {20.0, 20.0},
        {40.0, 20.0},
    };
    std::vector<sawer::PointerSample> events;
    for (std::size_t segment = 0U;
         segment + 1U < corners.size(); ++segment) {
        const std::size_t first_step = segment == 0U ? 0U : 1U;
        for (std::size_t step = first_step;
             step <= subdivisions; ++step) {
            const double amount =
                static_cast<double>(step)
                / static_cast<double>(subdivisions);
            const auto start = corners[segment];
            const auto end = corners[segment + 1U];
            const double elapsed_pixels =
                static_cast<double>(segment * 20U)
                + amount * 20.0;
            events.push_back({
                .source = sawer::PointerSource::mouse,
                .position = {
                    start.x + (end.x - start.x) * amount,
                    start.y + (end.y - start.y) * amount,
                },
                .pressure = 1.0F,
                .timestamp = static_cast<std::uint64_t>(
                    elapsed_pixels * 1'000'000.0),
                .buttons = sawer::PointerButtons::primary,
            });
        }
    }
    return events;
}

} // namespace

TEST_CASE("pointer resampling is independent of event cadence")
{
    const std::vector<sawer::PointerSample> sparse{
        sample(0.0, 0U),
        sample(8.0, 8'000'000U),
    };
    std::vector<sawer::PointerSample> dense;
    for (std::uint64_t index = 0U; index <= 32U; ++index) {
        dense.push_back(sample(
            static_cast<double>(index) * 0.25,
            index * 250'000U));
    }

    const auto sparse_output = resample(sparse);
    const auto dense_output = resample(dense);
    REQUIRE(sparse_output.size() == dense_output.size());
    for (std::size_t index = 0U; index < sparse_output.size(); ++index) {
        REQUIRE(sparse_output[index].position == dense_output[index].position);
        REQUIRE(sparse_output[index].pressure == dense_output[index].pressure);
        REQUIRE(sparse_output[index].timestamp == dense_output[index].timestamp);
        REQUIRE(sparse_output[index].source == dense_output[index].source);
        REQUIRE(sparse_output[index].buttons == dense_output[index].buttons);
    }
}

TEST_CASE("stabilization receives the same samples for sparse and dense input")
{
    const std::vector<sawer::PointerSample> sparse{
        sample(0.0, 0U),
        sample(12.0, 12'000'000U),
    };
    std::vector<sawer::PointerSample> dense;
    for (std::uint64_t index = 0U; index <= 48U; ++index) {
        dense.push_back(sample(
            static_cast<double>(index) * 0.25,
            index * 250'000U));
    }

    REQUIRE(stabilize(sparse) == stabilize(dense));
}

TEST_CASE("completed stroke geometry is independent of platform event batching")
{
    // One event per segment models a compositor-batched Wayland/X11 trace;
    // quarter-pixel events model the much denser stream commonly seen on
    // Windows. Both describe the same timed pointer path.
    const auto batched = segmented_trace(1U);
    const auto dense = segmented_trace(80U);

    const auto batched_stroke = complete_stabilized_stroke(batched);
    const auto dense_stroke = complete_stabilized_stroke(dense);
    REQUIRE(batched_stroke == dense_stroke);
    REQUIRE(batched_stroke.front() == batched.front().position);
    REQUIRE(batched_stroke.back() == batched.back().position);
}

TEST_CASE("pointer resampling bounds pathological motion gaps")
{
    sawer::PointerResampler resampler;
    resampler.reset(sample(0.0, 0U));
    const auto output = resampler.push(
        sample(10'000.0, 10'000'000U), 1.0, true);

    REQUIRE(output.size() == sawer::PointerResampler::capacity);
    REQUIRE(output.back().position.x == 10'000.0);
}
