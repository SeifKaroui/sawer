#pragma once

#include "geometry/Geometry.hpp"

#include <cstdint>

namespace sawer {

enum class PointerSource {
    mouse,
    pen,
};

enum class PointerButtons : std::uint32_t {
    none = 0U,
    primary = 1U << 0U,
    secondary = 1U << 1U,
    middle = 1U << 2U,
};

struct PointerSample final {
    PointerSource source{PointerSource::mouse};
    Vec2d position;
    float pressure{1.0F};
    std::uint64_t timestamp{};
    PointerButtons buttons{PointerButtons::none};
};

} // namespace sawer

