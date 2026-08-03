#pragma once

#include <filesystem>
#include <string_view>

namespace sawer::log {

enum class Level {
    debug,
    info,
    warning,
    error,
};

void write(Level level, std::string_view message);
[[nodiscard]] bool set_file(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool export_file(
    const std::filesystem::path& destination) noexcept;
[[nodiscard]] std::filesystem::path file_path();

} // namespace sawer::log
