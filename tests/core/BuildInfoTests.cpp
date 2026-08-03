#include "core/BuildInfo.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("application metadata is stable")
{
    STATIC_REQUIRE(sawer::BuildInfo::name == std::string_view{"Sawer"});
    STATIC_REQUIRE(sawer::BuildInfo::version == std::string_view{"0.9.0"});
    STATIC_REQUIRE(sawer::BuildInfo::identifier == std::string_view{"io.sawer.app"});
}

TEST_CASE("build configuration is known")
{
    const auto configuration = sawer::build_configuration();
    REQUIRE((configuration == "Debug" || configuration == "Release"));
}
