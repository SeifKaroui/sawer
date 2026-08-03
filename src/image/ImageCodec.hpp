#pragma once

#include "document/Object.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sawer {

struct DecodedImage final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

struct CanonicalImage final {
    std::shared_ptr<const ImageAsset> asset;
    std::shared_ptr<const DecodedImage> decoded;
};

// Decodes the untrusted BMP clipboard subset used by Windows clipboard
// providers: BITMAPINFOHEADER, 24-bit BGR and 32-bit BGRA, uncompressed.
// PNG is intentionally kept behind the same boundary for a later decoder.
[[nodiscard]] DecodedImage decode_bmp_rgba(std::span<const std::uint8_t> bytes);
[[nodiscard]] DecodedImage decode_image_rgba(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> encode_png_rgba(const DecodedImage& image);
// Produces the immutable board asset from untrusted PNG or BMP input. The
// preview is a deterministic, alpha-correct RGBA downscale encoded as PNG.
[[nodiscard]] CanonicalImage canonicalize_image(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maximum_preview_dimension = 256U);

} // namespace sawer
