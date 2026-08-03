#include "storage/BoardPath.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace sawer {
namespace {

bool to_local_time(
    const std::filesystem::file_time_type modified, std::tm& out) noexcept
{
    const auto system_time =
        std::chrono::clock_cast<std::chrono::system_clock>(modified);
    const std::time_t time = std::chrono::system_clock::to_time_t(system_time);
#if defined(_WIN32)
    return localtime_s(&out, &time) == 0;
#else
    return localtime_r(&time, &out) != nullptr;
#endif
}

} // namespace

std::filesystem::path unique_board_path(
    const std::filesystem::path& directory,
    const std::string_view base_name)
{
    const std::string base = base_name.empty()
        ? std::string{untitled_board_name}
        : std::string{base_name};
    std::filesystem::path candidate =
        directory / (base + std::string{board_extension});
    for (int suffix = 2; std::filesystem::exists(candidate); ++suffix) {
        candidate = directory
            / (base + " " + std::to_string(suffix)
               + std::string{board_extension});
    }
    return candidate;
}

std::string sanitize_board_name(const std::string_view name)
{
    std::string cleaned;
    cleaned.reserve(name.size());
    for (const char character : name) {
        const bool illegal = character == '\\' || character == '/'
            || character == ':' || character == '*' || character == '?'
            || character == '"' || character == '<' || character == '>'
            || character == '|'
            || static_cast<unsigned char>(character) < 0x20U;
        if (!illegal) {
            cleaned.push_back(character);
        }
    }

    const auto first = cleaned.find_first_not_of(" .\t");
    const auto last = cleaned.find_last_not_of(" .\t");
    if (first == std::string::npos) {
        return std::string{untitled_board_name};
    }
    cleaned = cleaned.substr(first, last - first + 1U);

    if (cleaned.size() > board_name_max_bytes) {
        std::size_t limit = board_name_max_bytes;
        while (limit > 0U
               && (static_cast<unsigned char>(cleaned[limit]) & 0xC0U)
                   == 0x80U) {
            --limit;
        }
        cleaned.resize(limit);
    }
    return cleaned.empty() ? std::string{untitled_board_name} : cleaned;
}

std::string format_modified_time(
    const std::filesystem::file_time_type modified)
{
    std::tm calendar{};
    if (!to_local_time(modified, calendar)) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(
        buffer.data(), buffer.size(), "%Y-%m-%d %H:%M", &calendar);
    return std::string{buffer.data(), written};
}

} // namespace sawer
