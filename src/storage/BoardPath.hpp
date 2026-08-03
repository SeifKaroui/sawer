#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace sawer {

inline constexpr std::string_view board_extension{".sawer"};
inline constexpr std::string_view untitled_board_name{"Untitled"};
inline constexpr std::size_t board_name_max_bytes = 96U;

// Returns a non-existing board path in directory, adding " 2", " 3", and so
// on when the requested filename is already in use.
[[nodiscard]] std::filesystem::path unique_board_path(
    const std::filesystem::path& directory,
    std::string_view base_name);

// Cleans a user-entered title into a safe single-segment filename stem.
[[nodiscard]] std::string sanitize_board_name(std::string_view name);

// Formats a board timestamp as local "YYYY-MM-DD HH:MM" text.
[[nodiscard]] std::string format_modified_time(
    std::filesystem::file_time_type modified);

} // namespace sawer
