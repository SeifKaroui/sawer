#pragma once

#include "document/Object.hpp"

#include <cstdint>
#include <span>

namespace sawer {

// Computes the standard SHA-256 digest. Asset IDs deliberately use the
// canonical PNG byte stream rather than decoded pixels, making an asset ID
// stable across platforms and independent of its placement on a board.
[[nodiscard]] AssetId sha256(std::span<const std::uint8_t> bytes) noexcept;

} // namespace sawer
