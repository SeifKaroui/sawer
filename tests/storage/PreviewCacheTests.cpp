#include "storage/PreviewCache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("preview cache round trips only through preferences storage")
{
    const auto root = std::filesystem::temp_directory_path()
        / ("sawer-preview-cache-"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()));
    const auto board_directory = root / "boards";
    const auto cache_directory = root / "preferences" / "previews";
    const auto board_path = board_directory / "Portable.sawer";
    std::filesystem::create_directories(board_directory);

    sawer::BoardPreview preview;
    preview.has_content = true;
    preview.rgba.resize(
        static_cast<std::size_t>(sawer::BoardPreview::pixel_width)
            * sawer::BoardPreview::pixel_height * 4U);
    for (std::size_t index = 0U; index < preview.rgba.size(); ++index) {
        preview.rgba[index] =
            static_cast<std::uint8_t>(index % 251U);
    }
    preview.dark_rgba = preview.rgba;
    preview.dark_rgba.front() = 235U;
    const auto modified = std::filesystem::file_time_type::clock::now();
    constexpr std::uintmax_t file_size = 42'000U;
    sawer::store_preview_cache(
        cache_directory,
        board_path,
        modified,
        file_size,
        preview);

    const auto loaded = sawer::load_preview_cache(
        cache_directory,
        board_path,
        modified,
        file_size);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_content);
    REQUIRE(loaded->rgba == preview.rgba);
    REQUIRE(loaded->dark_rgba == preview.dark_rgba);
    REQUIRE_FALSE(sawer::load_preview_cache(
        cache_directory,
        board_path,
        modified,
        file_size + 1U).has_value());

    std::size_t board_side_files = 0U;
    for (const auto& entry :
         std::filesystem::directory_iterator{board_directory}) {
        static_cast<void>(entry);
        ++board_side_files;
    }
    REQUIRE(board_side_files == 0U);

    const auto cache_file = std::filesystem::directory_iterator{cache_directory}->path();
    // A previous single-palette cache is disposable and must be rebuilt.
    {
        std::fstream output{cache_file, std::ios::in | std::ios::out | std::ios::binary};
        output.seekp(7);
        output.put('2');
    }
    REQUIRE_FALSE(sawer::load_preview_cache(
        cache_directory, board_path, modified, file_size).has_value());
    sawer::store_preview_cache(cache_directory, board_path, modified, file_size, preview);
    std::filesystem::resize_file(cache_file, std::filesystem::file_size(cache_file) - 1U);
    REQUIRE_FALSE(sawer::load_preview_cache(
        cache_directory, board_path, modified, file_size).has_value());

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
