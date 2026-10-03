#include "ui/NavigationTransition.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_CASE("navigation animates screen changes without delaying the initial view")
{
    sawer::NavigationTransition transition;
    REQUIRE_FALSE(transition.update_view(true));
    REQUIRE_FALSE(transition.animating());
    REQUIRE(transition.opacity() == 0.0);

    REQUIRE(transition.update_view(false));
    REQUIRE(transition.animating());
    REQUIRE(transition.opacity() == 1.0);
    transition.tick(sawer::NavigationTransition::duration_seconds / 2.0);
    REQUIRE(transition.opacity() == Catch::Approx(0.5));
    REQUIRE_FALSE(transition.update_view(false));
    REQUIRE(transition.opacity() == Catch::Approx(0.5));
    transition.tick(1.0);
    REQUIRE_FALSE(transition.animating());
    REQUIRE(transition.opacity() == 0.0);

    REQUIRE(transition.update_view(true));
    REQUIRE(transition.opacity() == 1.0);
}

TEST_CASE("navigation settles monotonically and tolerates interrupted switches")
{
    sawer::NavigationTransition transition;
    static_cast<void>(transition.update_view(false));
    static_cast<void>(transition.update_view(true));
    double previous = transition.opacity();
    for (int frame = 0; frame < 30; ++frame) {
        transition.tick(1.0 / 120.0);
        REQUIRE(transition.opacity() <= previous);
        REQUIRE(transition.opacity() >= 0.0);
        previous = transition.opacity();
    }
    REQUIRE_FALSE(transition.animating());
    static_cast<void>(transition.update_view(false));
    transition.tick(0.04);
    REQUIRE(transition.update_view(true));
    REQUIRE(transition.opacity() == 1.0);
    transition.tick(-1.0);
    transition.tick(std::numeric_limits<double>::quiet_NaN());
    transition.tick(std::numeric_limits<double>::infinity());
    REQUIRE(transition.opacity() == 1.0);
    transition.tick(sawer::NavigationTransition::duration_seconds);
    REQUIRE_FALSE(transition.animating());
    REQUIRE(transition.opacity() == 0.0);
}
