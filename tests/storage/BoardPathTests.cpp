#include "storage/BoardPath.hpp"

#include "document/ObjectId.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path()
            / ("sawer-board-path-"
               + sawer::ObjectId::random().to_string())}
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("board paths avoid existing filenames")
{
    const TemporaryDirectory directory;
    const auto first =
        sawer::unique_board_path(directory.path(), "Untitled");
    REQUIRE(first.filename() == "Untitled.sawer");

    std::ofstream existing{first, std::ios::binary};
    existing << "x";
    existing.close();

    const auto second =
        sawer::unique_board_path(directory.path(), "Untitled");
    REQUIRE(second.filename() == "Untitled 2.sawer");
}

TEST_CASE("board names are sanitized into safe filename stems")
{
    REQUIRE(sawer::sanitize_board_name("  Trip plans  ") == "Trip plans");
    REQUIRE(sawer::sanitize_board_name("a/b:c*?d") == "abcd");
    REQUIRE(sawer::sanitize_board_name("   ") == "Untitled");
    REQUIRE(sawer::sanitize_board_name("") == "Untitled");
}

TEST_CASE("board name truncation never splits a UTF-8 codepoint")
{
    const std::string long_name = std::string(95U, 'a') + "\xE2\x82\xAC";
    const std::string sanitized = sawer::sanitize_board_name(long_name);

    REQUIRE(sanitized == std::string(95U, 'a'));
    REQUIRE(sanitized.size() <= sawer::board_name_max_bytes);
}

TEST_CASE("modified timestamps format as local date and time")
{
    const auto now = std::filesystem::file_time_type::clock::now();
    const std::string formatted = sawer::format_modified_time(now);
    REQUIRE(formatted.size() == 16U);
    REQUIRE(formatted.find('-') != std::string::npos);
    REQUIRE(formatted.find(':') != std::string::npos);
}
