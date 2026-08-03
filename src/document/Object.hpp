#pragma once

#include "document/ObjectId.hpp"
#include "geometry/Geometry.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace sawer {

struct Color final {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t alpha{255U};

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

struct Style final {
    Color stroke{30U, 34U, 42U, 255U};
    std::optional<Color> fill;
    double stroke_width{4.0};

    friend constexpr bool operator==(const Style&, const Style&) = default;
};

struct Line final {
    Vec2d start;
    Vec2d end;

    friend constexpr bool operator==(const Line&, const Line&) = default;
};

struct Stroke final {
    std::vector<Vec2d> points;

    friend bool operator==(const Stroke&, const Stroke&) = default;
};

struct RectangleShape final {
    Vec2d first;
    Vec2d second;
    // Corner radius as a fraction of the shorter side. The valid range is
    // 0.0 (square) through 0.5 (fully rounded).
    double roundness{};

    friend constexpr bool operator==(
        const RectangleShape&,
        const RectangleShape&) = default;
};

struct Ellipse final {
    Vec2d first;
    Vec2d second;

    friend constexpr bool operator==(const Ellipse&, const Ellipse&) = default;
};

using AssetId = std::array<std::uint8_t, 32U>;

struct ImageAsset final {
    AssetId id{};
    std::uint32_t pixel_width{};
    std::uint32_t pixel_height{};
    std::vector<std::uint8_t> png;
    std::vector<std::uint8_t> preview;
};

struct Image final {
    std::shared_ptr<const ImageAsset> asset;
    Vec2d first;
    Vec2d second;

    friend bool operator==(const Image&, const Image&) = default;
};

using ObjectGeometry =
    std::variant<Line, Stroke, RectangleShape, Ellipse, Image>;

struct Object final {
    ObjectId id;
    std::int64_t z_order{};
    Style style;
    Aabb bounds;
    ObjectGeometry geometry;
    std::uint64_t revision{1U};

    [[nodiscard]] static Object make_line(
        ObjectId id,
        std::int64_t z_order,
        Line line,
        Style style = {});
    [[nodiscard]] static Object make_stroke(
        ObjectId id,
        std::int64_t z_order,
        Stroke stroke,
        Style style = {});
    [[nodiscard]] static Object make_rectangle(
        ObjectId id,
        std::int64_t z_order,
        RectangleShape rectangle,
        Style style = {});
    [[nodiscard]] static Object make_ellipse(
        ObjectId id,
        std::int64_t z_order,
        Ellipse ellipse,
        Style style = {});
    [[nodiscard]] static Object make_image(
        ObjectId id,
        std::int64_t z_order,
        Image image);

    void translate(Vec2d delta);
    [[nodiscard]] bool within_board_bounds() const noexcept;
    [[nodiscard]] bool can_translate(Vec2d delta) const noexcept;
    void recompute_bounds();
};

struct ObjectDraft final {
    ObjectGeometry geometry;
    Style style;
    // A generation identifies one pointer gesture. Revision advances only
    // when that gesture's preview geometry changes, allowing the renderer to
    // retain and incrementally extend the active mesh across frames.
    std::uint64_t generation{};
    std::uint64_t revision{1U};
};

} // namespace sawer
