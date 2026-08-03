#include "app/Clipboard.hpp"
#include <SDL3/SDL_clipboard.h>

#include <string>
#include <utility>

namespace sawer {
namespace {

const void* SDLCALL clipboard_data(
    void* const userdata,
    const char* const mime_type,
    std::size_t* const size)
{
    if (size == nullptr) return nullptr;
    *size = 0U;
    const auto* const payload = static_cast<const Clipboard::Payload*>(userdata);
    if (payload == nullptr || mime_type == nullptr) return nullptr;
    for (const auto& [type, bytes] : *payload) {
        if (type == mime_type) {
            *size = bytes.size();
            return bytes.empty() ? nullptr : bytes.data();
        }
    }
    return nullptr;
}

void SDLCALL clipboard_cleanup(void* const userdata)
{
    delete static_cast<Clipboard::Payload*>(userdata);
}

} // namespace

std::optional<std::vector<std::uint8_t>> Clipboard::read(const std::string_view mime_type) const {
    const std::string type{mime_type}; std::size_t size{};
    void* const data = SDL_GetClipboardData(type.c_str(), &size);
    if (data == nullptr) return std::nullopt;
    std::vector<std::uint8_t> result(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
    SDL_free(data); return result;
}
bool Clipboard::has(const std::string_view mime_type) const { const std::string type{mime_type}; return SDL_HasClipboardData(type.c_str()); }

bool Clipboard::offer(Payload payload) const
{
    if (payload.empty()) return false;
    std::vector<const char*> mime_types;
    mime_types.reserve(payload.size());
    for (const auto& [type, bytes] : payload) {
        static_cast<void>(bytes);
        if (type.empty()) return false;
        mime_types.push_back(type.c_str());
    }
    auto* const provider = new Payload{std::move(payload)};
    if (!SDL_SetClipboardData(
            clipboard_data, clipboard_cleanup, provider,
            mime_types.data(), mime_types.size())) {
        delete provider;
        return false;
    }
    return true;
}
} // namespace sawer
