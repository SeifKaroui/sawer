#include "core/Log.hpp"

#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>

namespace sawer::log {
namespace {

std::mutex output_mutex;
std::ofstream output_file;
std::filesystem::path output_path;
constexpr std::uintmax_t maximum_log_bytes = 4U * 1024U * 1024U;

std::string_view level_name(const Level level) noexcept
{
    switch (level) {
    case Level::debug:
        return "debug";
    case Level::info:
        return "info";
    case Level::warning:
        return "warning";
    case Level::error:
        return "error";
    }

    return "unknown";
}

} // namespace

void write(const Level level, const std::string_view message)
{
    const std::scoped_lock lock{output_mutex};
    std::clog << "[sawer][" << level_name(level) << "] " << message << '\n';
    if (output_file) {
        output_file << "[sawer][" << level_name(level) << "] "
                    << message << '\n';
        output_file.flush();
    }
}

bool set_file(const std::filesystem::path& path) noexcept
{
    const std::scoped_lock lock{output_mutex};
    output_file.close();
    output_path.clear();
    if (path.empty()) {
        return true;
    }

    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
    }
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size > maximum_log_bytes) {
        auto previous = path;
        previous += ".previous";
        std::filesystem::remove(previous, error);
        error.clear();
        std::filesystem::rename(path, previous, error);
        if (error) {
            return false;
        }
    }
    error.clear();
    output_file.open(path, std::ios::binary | std::ios::app);
    if (!output_file) {
        return false;
    }
    output_path = path;
    return true;
}

bool export_file(const std::filesystem::path& destination) noexcept
{
    const std::scoped_lock lock{output_mutex};
    if (output_path.empty() || destination.empty()
        || destination == output_path) {
        return false;
    }
    output_file.flush();
    std::error_code error;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(
            destination.parent_path(), error);
        if (error) {
            return false;
        }
    }
    std::filesystem::copy_file(
        output_path,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

std::filesystem::path file_path()
{
    const std::scoped_lock lock{output_mutex};
    return output_path;
}

} // namespace sawer::log
