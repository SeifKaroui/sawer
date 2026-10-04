#include "storage/RecentFiles.hpp"
#include "core/Filesystem.hpp"
#include "document/ObjectId.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path test_settings_path()
{
    return std::filesystem::temp_directory_path()
        / "sawer-recent-files-preferences-test.json";
}

} // namespace

TEST_CASE("recent-file preferences default to light when no theme is saved")
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("sawer-default-theme-" + sawer::ObjectId::random().to_string());
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto settings = directory / "preferences.json";
    REQUIRE(sawer::RecentFiles{settings}.light_theme());
    std::filesystem::create_directories(directory);
    for (const auto& legacy : {nlohmann::json::array(),
            nlohmann::json{{"recent", nlohmann::json::array()}},
            nlohmann::json{{"recent", nlohmann::json::array()}, {"drawing", {{"light_theme", "invalid"}}}}}) {
        { std::ofstream output{settings}; output << legacy.dump(); }
        sawer::RecentFiles preferences{settings};
        REQUIRE(preferences.light_theme());
        preferences.set_light_theme(false);
        REQUIRE_FALSE(sawer::RecentFiles{settings}.light_theme());
    }
}

TEST_CASE("Unicode recent files and zoom levels survive preference reload and rename")
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("sawer-unicode-preferences-" + sawer::ObjectId::random().to_string());
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto settings = directory / sawer::path_from_utf8("\xe9\x85\x8d\xe7\xbd\xae.json");
    const auto board = directory / sawer::path_from_utf8("\xe7\x94\xbb\xe6\x9d\xbf.sawer");
    const auto renamed = directory / sawer::path_from_utf8("\xf0\x9f\x96\x8a notes.sawer");
    {
        sawer::RecentFiles preferences{settings};
        preferences.touch(board);
        preferences.set_zoom(board, 2.5);
        preferences.replace(board, renamed);
    }
    const sawer::RecentFiles reloaded{settings};
    REQUIRE(reloaded.entries() == std::vector<std::filesystem::path>{renamed});
    REQUIRE(reloaded.zoom(renamed) == 2.5);
    std::ifstream input{settings};
    const auto json = nlohmann::json::parse(input);
    REQUIRE(json.at("recent").at(0) == sawer::path_to_utf8(renamed));
}

TEST_CASE("recent-file preferences retain theme while ignoring retired smoothing settings")
{
    const auto path = test_settings_path();
    const auto board = path.parent_path() / "sawer-preference-board.sawer";
    // An older file may contain a Custom profile, including its tuning values.
    {
        const nlohmann::json old_settings{
            {"version", 1}, {"recent", {board.string()}},
            {"zoom", {{{"path", board.string()}, {"value", 1.5}}}},
            {"drawing", {{"profile", 3}, {"strength", 1.0},
                {"trailing_pixels", 64.0}, {"light_theme", true}}}};
        std::ofstream output{path};
        output << old_settings.dump();
    }
    {
        sawer::RecentFiles preferences{path};
        REQUIRE(preferences.light_theme());
        REQUIRE(preferences.entries().size() == 1U);
        REQUIRE(preferences.entries().front() == board);
        REQUIRE(preferences.zoom(board) == 1.5);
        preferences.set_light_theme(false);
    }
    {
        sawer::RecentFiles preferences{path};
        REQUIRE_FALSE(preferences.light_theme());
        REQUIRE(preferences.entries().size() == 1U);
        REQUIRE(preferences.zoom(board) == 1.5);
        std::ifstream input{path};
        const auto saved = nlohmann::json::parse(input);
        REQUIRE_FALSE(saved.at("drawing").contains("profile"));
        REQUIRE_FALSE(saved.at("drawing").contains("strength"));
        REQUIRE_FALSE(saved.at("drawing").contains("trailing_pixels"));
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}
