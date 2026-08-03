#pragma once

#include "ui/BoardPreview.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace sawer {

// Disposable thumbnail caches live only under the application preferences
// directory. A board's path, size, and modification time identify one cache
// generation; no metadata is written beside the board.
[[nodiscard]] std::optional<BoardPreview> load_preview_cache(
    const std::filesystem::path& cache_directory,
    const std::filesystem::path& board_path,
    std::filesystem::file_time_type modified,
    std::uintmax_t file_size);

void store_preview_cache(
    const std::filesystem::path& cache_directory,
    const std::filesystem::path& board_path,
    std::filesystem::file_time_type modified,
    std::uintmax_t file_size,
    const BoardPreview& preview);

} // namespace sawer
