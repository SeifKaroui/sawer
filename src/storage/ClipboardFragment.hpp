#pragma once

#include "document/Document.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sawer {

inline constexpr std::string_view sawer_clipboard_mime =
    "application/x-sawer-fragment";

struct ClipboardFragment final {
    std::vector<Object> objects;
};

// Sawer clipboard data is a self-contained, versioned CBOR fragment. Image
// assets are embedded once and referenced by digest, so mixed selections can
// cross board and process boundaries without companion files.
[[nodiscard]] std::vector<std::uint8_t> serialize_clipboard_fragment(
    const Document& document,
    std::span<const ObjectId> ids);

[[nodiscard]] ClipboardFragment deserialize_clipboard_fragment(
    std::span<const std::uint8_t> bytes);

} // namespace sawer
