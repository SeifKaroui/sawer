#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sawer {

// SDL, UI text, and persisted JSON use UTF-8. Windows native paths use UTF-16.
[[nodiscard]] inline std::filesystem::path path_from_utf8(
    const std::string_view text)
{
    std::u8string utf8;
    utf8.reserve(text.size());
    for (const char byte : text) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path{utf8};
}

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

} // namespace sawer
