#include "input/DrawingSettings.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("drawing settings expose only meaningful stabilization levels")
{
    sawer::DrawingSettings settings;
    REQUIRE(settings.stabilization_enabled());
#if defined(_WIN32)
    REQUIRE(settings.stabilization == sawer::StrokeStabilization::light);
    REQUIRE(settings.stabilization_scale() == 0.8);
    REQUIRE(settings.completion_smoothing_radius() == 5U);
    REQUIRE(settings.gaussian_sigma() == Catch::Approx(9.6));
#else
    REQUIRE(settings.stabilization == sawer::StrokeStabilization::standard);
    REQUIRE(settings.stabilization_scale() == 1.0);
    REQUIRE(settings.completion_smoothing_radius() == 14U);
    REQUIRE(settings.gaussian_sigma() == 24.0);
#endif
    REQUIRE(settings.sampling_distance() == 0.01);
    REQUIRE(settings.simplification_tolerance() == 0.15);

    settings.stabilization = sawer::StrokeStabilization::light;
    REQUIRE(settings.stabilization_enabled());
#if defined(_WIN32)
    REQUIRE(settings.completion_smoothing_radius() == 5U);
    REQUIRE(settings.gaussian_sigma() == Catch::Approx(9.6));
#else
    REQUIRE(settings.completion_smoothing_radius() == 6U);
    REQUIRE(settings.gaussian_sigma() == 12.0);
#endif
    REQUIRE(settings.sampling_distance() == 0.01);

    settings.stabilization = sawer::StrokeStabilization::strong;
    REQUIRE(settings.stabilization_enabled());
#if defined(_WIN32)
    REQUIRE(settings.completion_smoothing_radius() == 18U);
    REQUIRE(settings.gaussian_sigma() == Catch::Approx(28.8));
#else
    REQUIRE(settings.completion_smoothing_radius() == 22U);
    REQUIRE(settings.gaussian_sigma() == 36.0);
#endif
    REQUIRE(settings.sampling_distance() == 0.01);

    settings.stabilization = sawer::StrokeStabilization::off;
    REQUIRE_FALSE(settings.stabilization_enabled());
    REQUIRE(settings.completion_smoothing_radius() == 0U);
    REQUIRE(settings.gaussian_sigma() == 0.0);
    REQUIRE(settings.sampling_distance() == 0.75);
}
