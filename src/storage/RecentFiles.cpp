#include "storage/RecentFiles.hpp"
#include "core/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace sawer {
namespace {

constexpr std::uintmax_t maximum_settings_bytes = 1024U * 1024U;

std::filesystem::path normalized_path(
    const std::filesystem::path& path)
{
    std::error_code error;
    auto normalized = std::filesystem::absolute(path, error);
    if (error) {
        normalized = path;
    }
    return normalized.lexically_normal();
}

} // namespace

RecentFiles::RecentFiles(
    std::filesystem::path settings_path,
    const std::size_t limit)
    : settings_path_{std::move(settings_path)}
    , limit_{std::max(limit, std::size_t{1U})}
{
    load();
}

void RecentFiles::touch(const std::filesystem::path& board_path)
{
    auto normalized = normalized_path(board_path);

    std::erase(entries_, normalized);
    entries_.insert(entries_.begin(), std::move(normalized));
    if (entries_.size() > limit_) {
        entries_.resize(limit_);
    }
    prune_zoom_levels();
    save();
}

void RecentFiles::replace(
    const std::filesystem::path& old_path,
    const std::filesystem::path& new_path)
{
    const auto old_normalized = normalized_path(old_path);
    auto new_normalized = normalized_path(new_path);
    const auto old_zoom = zoom_levels_.find(old_normalized);
    const std::optional<double> remembered_zoom =
        old_zoom != zoom_levels_.end()
        ? std::optional<double>{old_zoom->second}
        : std::nullopt;

    std::erase(entries_, old_normalized);
    std::erase(entries_, new_normalized);
    zoom_levels_.erase(old_normalized);
    zoom_levels_.erase(new_normalized);
    entries_.insert(entries_.begin(), std::move(new_normalized));
    if (entries_.size() > limit_) {
        entries_.resize(limit_);
    }
    if (remembered_zoom.has_value()) {
        zoom_levels_[entries_.front()] = *remembered_zoom;
    }
    prune_zoom_levels();
    save();
}

void RecentFiles::set_zoom(
    const std::filesystem::path& board_path,
    const double zoom)
{
    if (!std::isfinite(zoom) || zoom <= 0.0) {
        return;
    }
    const auto normalized = normalized_path(board_path);
    if (std::ranges::find(entries_, normalized) == entries_.end()) {
        return;
    }
    zoom_levels_[normalized] = zoom;
    save();
}

std::optional<double> RecentFiles::zoom(
    const std::filesystem::path& board_path) const
{
    const auto found = zoom_levels_.find(normalized_path(board_path));
    return found != zoom_levels_.end()
        ? std::optional<double>{found->second}
        : std::nullopt;
}

bool RecentFiles::light_theme() const noexcept
{
    return light_theme_;
}

void RecentFiles::set_light_theme(const bool light_theme)
{
    light_theme_ = light_theme;
    save();
}

const std::vector<std::filesystem::path>& RecentFiles::entries() const noexcept
{
    return entries_;
}

void RecentFiles::load()
{
    std::error_code error;
    const auto size = std::filesystem::file_size(settings_path_, error);
    if (!error && size > maximum_settings_bytes) {
        return;
    }
    std::ifstream input{settings_path_, std::ios::binary};
    if (!input) {
        return;
    }

    try {
        const auto value = nlohmann::json::parse(input);
        const auto* recent = value.is_array()
            ? &value
            : (value.is_object() && value.contains("recent")
                    ? &value.at("recent")
                    : nullptr);
        if (recent == nullptr || !recent->is_array()) {
            return;
        }
        for (const auto& entry : *recent) {
            if (entry.is_string() && entries_.size() < limit_) {
                const auto path =
                    normalized_path(path_from_utf8(entry.get<std::string>()));
                if (std::ranges::find(entries_, path) == entries_.end()) {
                    entries_.push_back(path);
                }
            }
        }
        if (value.is_object() && value.contains("zoom")
            && value.at("zoom").is_array()) {
            for (const auto& entry : value.at("zoom")) {
                if (!entry.is_object() || !entry.contains("path")
                    || !entry.at("path").is_string()
                    || !entry.contains("value")
                    || !entry.at("value").is_number()) {
                    continue;
                }
                const double zoom = entry.at("value").get<double>();
                const auto path = normalized_path(
                    path_from_utf8(entry.at("path").get<std::string>()));
                if (std::isfinite(zoom) && zoom > 0.0
                    && std::ranges::find(entries_, path) != entries_.end()) {
                    zoom_levels_[path] = zoom;
                }
            }
        }
        if (value.is_object() && value.contains("drawing")
            && value.at("drawing").is_object()) {
            const auto& drawing = value.at("drawing");
            if (drawing.contains("light_theme") && drawing.at("light_theme").is_boolean()) {
                light_theme_ = drawing.at("light_theme").get<bool>();
            }
        }
    } catch (const std::exception&) {
        entries_.clear();
        zoom_levels_.clear();
    }
}

void RecentFiles::save() const
{
    std::error_code error;
    std::filesystem::create_directories(settings_path_.parent_path(), error);

    nlohmann::json recent = nlohmann::json::array();
    for (const auto& entry : entries_) {
        recent.push_back(path_to_utf8(entry));
    }
    nlohmann::json zoom = nlohmann::json::array();
    for (const auto& entry : entries_) {
        const auto found = zoom_levels_.find(entry);
        if (found != zoom_levels_.end()) {
            zoom.push_back({
                {"path", path_to_utf8(entry)},
                {"value", found->second},
            });
        }
    }
    const nlohmann::json value{
        {"version", 1},
        {"recent", std::move(recent)},
        {"zoom", std::move(zoom)},
        {"drawing", {
            {"light_theme", light_theme_},
        }},
    };

    std::ofstream output{
        settings_path_, std::ios::binary | std::ios::trunc};
    if (output) {
        output << value.dump(2) << '\n';
    }
}

void RecentFiles::prune_zoom_levels()
{
    std::erase_if(
        zoom_levels_,
        [this](const auto& entry) {
            return std::ranges::find(entries_, entry.first)
                == entries_.end();
        });
}

} // namespace sawer
