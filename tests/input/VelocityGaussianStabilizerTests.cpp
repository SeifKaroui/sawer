#include "input/DrawingSettings.hpp"
#include "input/VelocityGaussianStabilizer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace {

using Catch::Approx;

sawer::PointerSample sample(
    const double x,
    const double y,
    const float pressure,
    const std::uint64_t milliseconds)
{
    return {
        .source = sawer::PointerSource::mouse,
        .position = {x, y},
        .pressure = pressure,
        .timestamp = milliseconds * 1'000'000U,
        .buttons = sawer::PointerButtons::primary,
    };
}

struct StabilizationMetrics final {
    double jitter{};
    double lag{};
};

StabilizationMetrics measure_normal_mouse_jitter(const double sigma)
{
    sawer::VelocityGaussianStabilizer stabilizer;
    stabilizer.reset(sigma, sample(0.0, 0.0, 1.0F, 0U));

    double squared_jitter = 0.0;
    double lag = 0.0;
    constexpr std::size_t warmup = 10U;
    constexpr std::size_t sample_count = 40U;
    for (std::size_t index = 1U; index <= sample_count; ++index) {
        const double raw_x = static_cast<double>(index) * 2.0;
        const double raw_y = index % 2U == 0U ? -2.0 : 2.0;
        const auto filtered = stabilizer.push(sample(
            raw_x,
            raw_y,
            1.0F,
            static_cast<std::uint64_t>(index) * 8U));
        if (index > warmup) {
            squared_jitter += filtered.position.y * filtered.position.y;
            lag += raw_x - filtered.position.x;
        }
    }
    constexpr double measured =
        static_cast<double>(sample_count - warmup);
    return {
        .jitter = std::sqrt(squared_jitter / measured),
        .lag = lag / measured,
    };
}

TEST_CASE("velocity Gaussian smooths slow pointer motion")
{
    sawer::VelocityGaussianStabilizer stabilizer;
    stabilizer.reset(0.5, sample(0.0, 0.0, 1.0F, 0U));

    const auto first = stabilizer.push(sample(1.0, 0.0, 1.0F, 10U));
    const auto second = stabilizer.push(sample(2.0, 0.0, 1.0F, 20U));

    REQUIRE(first.position.x > 0.45);
    REQUIRE(first.position.x < 0.55);
    REQUIRE(second.position.x > first.position.x);
    REQUIRE(second.position.x < 1.2);
}

TEST_CASE("velocity Gaussian stays responsive during fast motion")
{
    sawer::VelocityGaussianStabilizer stabilizer;
    stabilizer.reset(0.5, sample(0.0, 0.0, 0.2F, 0U));

    const auto fast = stabilizer.push(sample(10.0, 5.0, 0.8F, 1U));
    REQUIRE(fast.position.x == Approx(10.0).margin(1.0e-6));
    REQUIRE(fast.position.y == Approx(5.0).margin(1.0e-6));
    REQUIRE(fast.pressure == Approx(0.8F).margin(1.0e-6));
}

TEST_CASE("strong stabilization clearly exceeds the default correction")
{
    sawer::DrawingSettings settings;
    sawer::VelocityGaussianStabilizer standard;
    sawer::VelocityGaussianStabilizer strong;
    const auto initial = sample(0.0, 0.0, 1.0F, 0U);
    standard.reset(settings.gaussian_sigma(), initial);
    settings.stabilization = sawer::StrokeStabilization::strong;
    strong.reset(settings.gaussian_sigma(), initial);

    static_cast<void>(standard.push(sample(1.0, 0.0, 1.0F, 10U)));
    static_cast<void>(strong.push(sample(1.0, 0.0, 1.0F, 10U)));
    const auto standard_point =
        standard.push(sample(2.0, 0.0, 1.0F, 20U));
    const auto strong_point = strong.push(sample(2.0, 0.0, 1.0F, 20U));

    REQUIRE(strong_point.position.x < standard_point.position.x);
}

TEST_CASE("strong stabilization adds correction at mouse speed")
{
    sawer::DrawingSettings settings;
    const auto standard =
        measure_normal_mouse_jitter(settings.gaussian_sigma());
    settings.stabilization = sawer::StrokeStabilization::strong;
    const auto strong =
        measure_normal_mouse_jitter(settings.gaussian_sigma());

    REQUIRE(strong.lag > standard.lag);
}

TEST_CASE("configured strong stabilization exceeds the previous profile")
{
    const auto previous_strong = measure_normal_mouse_jitter(32.0);
    const auto configured_strong = measure_normal_mouse_jitter(36.0);

    REQUIRE(configured_strong.lag > previous_strong.lag);
}

TEST_CASE("velocity Gaussian uses screen-space speed")
{
    sawer::VelocityGaussianStabilizer normal;
    sawer::VelocityGaussianStabilizer zoomed;
    normal.reset(0.5, sample(0.0, 0.0, 1.0F, 0U), 1.0);
    zoomed.reset(0.5, sample(0.0, 0.0, 1.0F, 0U), 4.0);

    const auto normal_point = normal.push(sample(1.0, 0.0, 1.0F, 10U));
    const auto zoomed_point = zoomed.push(sample(1.0, 0.0, 1.0F, 10U));
    REQUIRE(zoomed_point.position.x > normal_point.position.x);
}

TEST_CASE("velocity Gaussian tolerates non-monotonic event timestamps")
{
    sawer::VelocityGaussianStabilizer stabilizer;
    stabilizer.reset(0.5, sample(0.0, 0.0, 1.0F, 20U));

    const auto older = stabilizer.push(sample(1.0, 0.0, 1.0F, 10U));
    const auto newer = stabilizer.push(sample(2.0, 0.0, 1.0F, 30U));
    REQUIRE(std::isfinite(older.position.x));
    REQUIRE(std::isfinite(newer.position.x));
    REQUIRE(newer.position.x >= older.position.x);
}

} // namespace
