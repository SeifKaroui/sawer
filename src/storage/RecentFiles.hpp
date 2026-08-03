#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sawer {

class RecentFiles final {
public:
    explicit RecentFiles(
        std::filesystem::path settings_path,
        std::size_t limit = 20U);

    void touch(const std::filesystem::path& board_path);
    void replace(
        const std::filesystem::path& old_path,
        const std::filesystem::path& new_path);
    void set_zoom(
        const std::filesystem::path& board_path,
        double zoom);
    [[nodiscard]] std::optional<double> zoom(
        const std::filesystem::path& board_path) const;
    [[nodiscard]] const std::vector<std::filesystem::path>& entries() const noexcept;

private:
    void load();
    void save() const;
    void prune_zoom_levels();

    std::filesystem::path settings_path_;
    std::size_t limit_;
    std::vector<std::filesystem::path> entries_;
    std::unordered_map<std::filesystem::path, double> zoom_levels_;
};

} // namespace sawer
