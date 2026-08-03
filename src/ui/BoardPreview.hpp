#pragma once

#include <cstdint>
#include <vector>

namespace sawer {

class Document;

// Theme-independent transparent thumbnail pixels. Board ink keeps its actual
// stroke and fill colors while the Home card supplies the themed background.
struct BoardPreview final {
    static constexpr std::uint32_t pixel_width = 256U;
    static constexpr std::uint32_t pixel_height = 144U;

    bool has_content{false};
    std::vector<std::uint8_t> rgba;
};

// Rasterizes the complete final document state into a bounded screen-space
// image. Every object is visited in z-order; sub-pixel detail is combined into
// bounded RGBA pixels instead of becoming unbounded Home-frame geometry.
[[nodiscard]] BoardPreview rasterize_board_preview(const Document& document);

} // namespace sawer
