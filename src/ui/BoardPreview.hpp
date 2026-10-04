#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sawer {

class Document;

// Transparent thumbnail pixels for both palettes. Display-only color mapping
// happens before compositing so overlapping colors and images stay correct.
struct BoardPreview final {
    static constexpr std::uint32_t pixel_width = 256U;
    static constexpr std::uint32_t pixel_height = 144U;

    bool has_content{false};
    std::vector<std::uint8_t> rgba;
    std::vector<std::uint8_t> dark_rgba;
};

// Rasterizes the complete final document state into a bounded screen-space
// image. Every object is visited in z-order; sub-pixel detail is combined into
// bounded RGBA pixels instead of becoming unbounded Home-frame geometry.
[[nodiscard]] BoardPreview rasterize_board_preview(const Document& document);

// Blend cached pixels during Home's palette transition without rerasterizing.
// The destination must have the same size as the preview's RGBA payload.
void write_board_preview_pixels(const BoardPreview& preview,
    std::uint8_t dark_amount, std::span<std::uint8_t> destination);

} // namespace sawer
