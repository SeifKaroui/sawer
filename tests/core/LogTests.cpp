#include "core/Log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

TEST_CASE("logger writes a structured line")
{
    std::ostringstream output;
    auto* const previous = std::clog.rdbuf(output.rdbuf());

    sawer::log::write(sawer::log::Level::warning, "test message");

    std::clog.rdbuf(previous);
    REQUIRE(output.str() == "[sawer][warning] test message\n");
}

TEST_CASE("logger keeps and exports a bounded local diagnostic file")
{
    const std::string unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path();
    const auto active = directory / ("sawer-log-" + unique + ".log");
    const auto exported =
        directory / ("sawer-log-export-" + unique + ".log");

    REQUIRE(sawer::log::set_file(active));
    sawer::log::write(sawer::log::Level::error, "exported diagnostic");
    REQUIRE(sawer::log::file_path() == active);
    REQUIRE(sawer::log::export_file(exported));
    REQUIRE(sawer::log::set_file({}));

    std::ifstream input{exported, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    REQUIRE(contents.find("[sawer][error] exported diagnostic")
            != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(active, ignored);
    std::filesystem::remove(exported, ignored);
}
