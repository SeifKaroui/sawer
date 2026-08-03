#include "core/ThirdPartyNotices.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("third-party notices are complete and embedded")
{
    const std::string_view notices = sawer::third_party_notices();

    REQUIRE(notices.starts_with("# Third-party notices\n"));
    REQUIRE(notices.ends_with('\n'));
    REQUIRE(notices.find("SDL 3.4.10") != std::string_view::npos);
    REQUIRE(notices.find("The FreeType Project LICENSE")
        != std::string_view::npos);
    REQUIRE(notices.find("SIL OPEN FONT LICENSE Version 1.1")
        != std::string_view::npos);
    REQUIRE(notices.find("MIT License") != std::string_view::npos);
    REQUIRE(notices.find("Lucide") != std::string_view::npos);
    REQUIRE(notices.find('\r') == std::string_view::npos);
}
