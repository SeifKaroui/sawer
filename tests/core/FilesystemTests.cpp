#include "core/Filesystem.hpp"
#include "document/ObjectId.hpp"
#include "storage/BoardPath.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("UTF-8 paths retain non-ANSI characters at filesystem boundaries")
{
    const auto name = sawer::path_from_utf8("\xe7\x94\xbb\xe6\x9d\xbf \xf0\x9f\x96\x8a.sawer");
    REQUIRE(sawer::path_to_utf8(name) == "\xe7\x94\xbb\xe6\x9d\xbf \xf0\x9f\x96\x8a.sawer");
#if defined(_WIN32)
    REQUIRE(name.native() == L"\u753b\u677f \U0001f58a.sawer");
#endif
    const auto directory = std::filesystem::temp_directory_path()
        / ("sawer-unicode-" + sawer::ObjectId::random().to_string());
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    std::filesystem::create_directories(directory);
    const auto file = directory / name;
    { std::ofstream output{file}; output << "Unicode path contents"; }
    REQUIRE(std::filesystem::exists(file));
    REQUIRE(sawer::path_from_utf8(sawer::path_to_utf8(file)) == file);
    const auto next = sawer::unique_board_path(directory, sawer::path_to_utf8(name.stem()));
    REQUIRE(next.filename() == sawer::path_from_utf8("\xe7\x94\xbb\xe6\x9d\xbf \xf0\x9f\x96\x8a 2.sawer"));
}
