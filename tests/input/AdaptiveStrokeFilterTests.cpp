#include "input/AdaptiveStrokeFilter.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

namespace {

sawer::PointerSample sample(const double x, const double y, const std::uint64_t ms)
{
    return {
        .source = sawer::PointerSource::mouse,
        .position = {x, y},
        .pressure = 1.0F,
        .timestamp = ms * 1'000'000U,
        .buttons = sawer::PointerButtons::primary,
    };
}

} // namespace

TEST_CASE("responsive stroke filter keeps a hard screen-space trailing bound")
{
    sawer::AdaptiveStrokeFilter filter;
    constexpr sawer::AdaptiveStrokeSettings settings{
        .strength = 0.32,
        .max_trailing_pixels = 8.0,
    };
    filter.reset(settings, sample(0.0, 0.0, 0U));

    for (std::size_t index = 1U; index <= 1'000U; ++index) {
        const auto filtered = filter.push(sample(
            static_cast<double>(index), 0.0, static_cast<std::uint64_t>(index)));
        REQUIRE(static_cast<double>(index) - filtered.position.x <= 8.0 + 1.0e-9);
    }
}

TEST_CASE("adaptive filter has comparable behaviour across input rates")
{
    constexpr std::array rates{125U, 250U, 500U, 1'000U};
    std::array<double, rates.size()> endpoints{};
    for (std::size_t rate_index = 0U; rate_index < rates.size(); ++rate_index) {
        const auto rate = rates[rate_index];
        sawer::AdaptiveStrokeFilter filter;
        filter.reset({}, sample(0.0, 0.0, 0U));
        const std::size_t count = rate;
        for (std::size_t index = 1U; index <= count; ++index) {
            const double x = static_cast<double>(index) * 500.0 / rate;
            const std::uint64_t milliseconds = index * 1'000U / rate;
            endpoints[rate_index] = filter.push(sample(x, 0.0, milliseconds)).position.x;
        }
        REQUIRE(500.0 - endpoints[rate_index] <= 8.0 + 1.0e-9);
    }
    REQUIRE(*std::max_element(endpoints.begin(), endpoints.end())
            - *std::min_element(endpoints.begin(), endpoints.end())
            < 2.0);
}

TEST_CASE("filter completion preserves the exact release point")
{
    sawer::AdaptiveStrokeFilter filter;
    filter.reset({}, sample(0.0, 0.0, 0U));
    static_cast<void>(filter.push(sample(5.0, 2.0, 10U)));
    const auto result = filter.finish(sample(7.25, -1.5, 12U));
    REQUIRE(result.position.x == Catch::Approx(7.25));
    REQUIRE(result.position.y == Catch::Approx(-1.5));
}
