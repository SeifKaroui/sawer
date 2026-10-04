#include "storage/PreviewCache.hpp"

#include "document/ObjectId.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace sawer {
namespace {

constexpr std::array<char, 8> cache_magic{
    'S', 'A', 'W', 'E', 'R', 'P', 'V', '3'};
constexpr std::size_t maximum_cache_files = 128U;

std::uint64_t fnv1a(
    std::uint64_t hash,
    const void* const data,
    const std::size_t size) noexcept
{
    const auto* const bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

std::filesystem::path cache_path(
    const std::filesystem::path& directory,
    const std::filesystem::path& board_path,
    const std::filesystem::file_time_type modified,
    const std::uintmax_t file_size)
{
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    const auto path_bytes = board_path.lexically_normal().generic_u8string();
    hash = fnv1a(hash, path_bytes.data(), path_bytes.size());
    const auto modified_ticks = modified.time_since_epoch().count();
    hash = fnv1a(hash, &modified_ticks, sizeof(modified_ticks));
    hash = fnv1a(hash, &file_size, sizeof(file_size));
    constexpr std::uint32_t raster_version = 4U;
    hash = fnv1a(hash, &raster_version, sizeof(raster_version));

    constexpr std::string_view digits{"0123456789abcdef"};
    std::array<char, 16> encoded{};
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        encoded[encoded.size() - index - 1U] =
            digits[hash & 0xFU];
        hash >>= 4U;
    }
    return directory
        / (std::string{encoded.data(), encoded.size()} + ".sawer-preview");
}

void trim_cache_directory(const std::filesystem::path& directory) noexcept
{
    std::error_code error;
    std::vector<std::pair<
        std::filesystem::file_time_type,
        std::filesystem::path>> files;
    for (std::filesystem::directory_iterator iterator{directory, error};
         !error && iterator != std::filesystem::directory_iterator{};
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error)
            || iterator->path().extension() != ".sawer-preview") {
            continue;
        }
        files.emplace_back(
            iterator->last_write_time(error), iterator->path());
        if (error) {
            error.clear();
        }
    }
    if (files.size() <= maximum_cache_files) {
        return;
    }
    std::ranges::sort(
        files,
        {},
        [](const auto& entry) { return entry.first; });
    for (std::size_t index = 0U;
         index < files.size() - maximum_cache_files;
         ++index) {
        std::filesystem::remove(files[index].second, error);
        error.clear();
    }
}

} // namespace

std::optional<BoardPreview> load_preview_cache(
    const std::filesystem::path& cache_directory,
    const std::filesystem::path& board_path,
    const std::filesystem::file_time_type modified,
    const std::uintmax_t file_size)
{
    std::ifstream input{
        cache_path(cache_directory, board_path, modified, file_size),
        std::ios::binary};
    if (!input) {
        return std::nullopt;
    }

    std::array<char, cache_magic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    input.read(
        reinterpret_cast<char*>(&width),
        static_cast<std::streamsize>(sizeof(width)));
    input.read(
        reinterpret_cast<char*>(&height),
        static_cast<std::streamsize>(sizeof(height)));
    if (!input || magic != cache_magic
        || width != BoardPreview::pixel_width
        || height != BoardPreview::pixel_height) {
        return std::nullopt;
    }

    BoardPreview preview;
    preview.has_content = true;
    preview.rgba.resize(
        static_cast<std::size_t>(width) * height * 4U);
    input.read(
        reinterpret_cast<char*>(preview.rgba.data()),
        static_cast<std::streamsize>(preview.rgba.size()));
    preview.dark_rgba.resize(preview.rgba.size());
    input.read(
        reinterpret_cast<char*>(preview.dark_rgba.data()),
        static_cast<std::streamsize>(preview.dark_rgba.size()));
    if (!input
        || input.peek() != std::char_traits<char>::eof()) {
        return std::nullopt;
    }
    return preview;
}

void store_preview_cache(
    const std::filesystem::path& cache_directory,
    const std::filesystem::path& board_path,
    const std::filesystem::file_time_type modified,
    const std::uintmax_t file_size,
    const BoardPreview& preview)
{
    const std::size_t expected_size =
        static_cast<std::size_t>(BoardPreview::pixel_width)
        * BoardPreview::pixel_height * 4U;
    if (!preview.has_content || preview.rgba.size() != expected_size
        || preview.dark_rgba.size() != expected_size) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(cache_directory, error);
    if (error) {
        return;
    }
    const auto destination =
        cache_path(cache_directory, board_path, modified, file_size);
    const auto temporary = cache_directory
        / (destination.filename().string() + ".tmp-"
            + ObjectId::random().to_string());
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return;
        }
        output.write(
            cache_magic.data(),
            static_cast<std::streamsize>(cache_magic.size()));
        constexpr std::uint32_t width = BoardPreview::pixel_width;
        constexpr std::uint32_t height = BoardPreview::pixel_height;
        output.write(
            reinterpret_cast<const char*>(&width),
            static_cast<std::streamsize>(sizeof(width)));
        output.write(
            reinterpret_cast<const char*>(&height),
            static_cast<std::streamsize>(sizeof(height)));
        output.write(
            reinterpret_cast<const char*>(preview.rgba.data()),
            static_cast<std::streamsize>(preview.rgba.size()));
        output.write(
            reinterpret_cast<const char*>(preview.dark_rgba.data()),
            static_cast<std::streamsize>(preview.dark_rgba.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        error.clear();
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    if (error) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return;
    }
    trim_cache_directory(cache_directory);
}

} // namespace sawer
