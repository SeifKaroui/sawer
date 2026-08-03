#include "document/Document.hpp"
#include "document/Object.hpp"
#include "image/ImageCodec.hpp"
#include "ui/BoardPreview.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

TEST_CASE("board preview raster visits the complete document")
{
    sawer::Document document;
    constexpr std::size_t early_objects = 700U;
    for (std::size_t index = 0U; index < early_objects; ++index) {
        REQUIRE(document.insert(sawer::Object::make_line(
            sawer::ObjectId::from_u64(index + 1U),
            document.next_z_order(),
            {{0.0, static_cast<double>(index % 20U)},
             {80.0, static_cast<double>(index % 20U)}})));
    }
    REQUIRE(document.insert(sawer::Object::make_line(
        sawer::ObjectId::from_u64(early_objects + 1U),
        document.next_z_order(),
        {{920.0, 0.0}, {1'000.0, 20.0}})));

    const sawer::BoardPreview preview =
        sawer::rasterize_board_preview(document);
    REQUIRE(preview.has_content);
    REQUIRE(
        preview.rgba.size()
        == static_cast<std::size_t>(sawer::BoardPreview::pixel_width)
            * sawer::BoardPreview::pixel_height * 4U);

    const std::size_t quarter =
        sawer::BoardPreview::pixel_width / 4U;
    bool left_ink = false;
    bool right_ink = false;
    for (std::size_t y = 0U;
         y < sawer::BoardPreview::pixel_height;
         ++y) {
        for (std::size_t x = 0U;
             x < sawer::BoardPreview::pixel_width;
             ++x) {
            const auto coverage = preview.rgba[
                (y * sawer::BoardPreview::pixel_width + x) * 4U + 3U];
            left_ink = left_ink || (x < quarter && coverage > 0U);
            right_ink = right_ink
                || (x >= sawer::BoardPreview::pixel_width - quarter
                    && coverage > 0U);
        }
    }
    REQUIRE(left_ink);
    REQUIRE(right_ink);
}

TEST_CASE("empty board preview has no raster payload")
{
    const sawer::Document document;
    const sawer::BoardPreview preview =
        sawer::rasterize_board_preview(document);
    REQUIRE_FALSE(preview.has_content);
    REQUIRE(preview.rgba.empty());
}

TEST_CASE("subpixel stroke samples accumulate across the thumbnail")
{
    sawer::Document document;
    sawer::Stroke stroke;
    constexpr std::size_t point_count = 20'000U;
    stroke.points.reserve(point_count);
    for (std::size_t index = 0U; index < point_count; ++index) {
        stroke.points.push_back({
            static_cast<double>(index)
                / static_cast<double>(point_count - 1U) * 1'000.0,
            10.0,
        });
    }
    REQUIRE(document.insert(sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(1U),
        document.next_z_order(),
        std::move(stroke))));

    const sawer::BoardPreview preview =
        sawer::rasterize_board_preview(document);
    const std::size_t right_edge =
        sawer::BoardPreview::pixel_width * 3U / 4U;
    bool right_ink = false;
    for (std::size_t y = 0U;
         y < sawer::BoardPreview::pixel_height;
         ++y) {
        for (std::size_t x = right_edge;
             x < sawer::BoardPreview::pixel_width;
             ++x) {
            right_ink = right_ink
                || preview.rgba[
                    (y * sawer::BoardPreview::pixel_width + x) * 4U + 3U]
                    > 0U;
        }
    }
    REQUIRE(right_ink);
}

TEST_CASE("board preview preserves stroke and fill colors")
{
    sawer::Document document;
    sawer::Style red_stroke;
    red_stroke.stroke = {220U, 35U, 45U, 255U};
    red_stroke.stroke_width = 8.0;
    REQUIRE(document.insert(sawer::Object::make_line(
        sawer::ObjectId::from_u64(1U),
        document.next_z_order(),
        {{0.0, 10.0}, {40.0, 10.0}},
        red_stroke)));

    sawer::Style blue_fill;
    blue_fill.stroke = {25U, 70U, 180U, 255U};
    blue_fill.fill = sawer::Color{45U, 105U, 230U, 255U};
    blue_fill.stroke_width = 4.0;
    REQUIRE(document.insert(sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(2U),
        document.next_z_order(),
        {{60.0, 0.0}, {100.0, 20.0}},
        blue_fill)));

    const sawer::BoardPreview preview =
        sawer::rasterize_board_preview(document);
    bool found_red = false;
    bool found_blue = false;
    for (std::size_t pixel = 0U;
         pixel < preview.rgba.size() / 4U;
         ++pixel) {
        const std::size_t offset = pixel * 4U;
        const auto red = preview.rgba[offset];
        const auto green = preview.rgba[offset + 1U];
        const auto blue = preview.rgba[offset + 2U];
        const auto alpha = preview.rgba[offset + 3U];
        found_red = found_red
            || (alpha > 200U && red > green + 80U && red > blue + 80U);
        found_blue = found_blue
            || (alpha > 200U && blue > red + 80U && blue > green + 60U);
    }
    REQUIRE(found_red);
    REQUIRE(found_blue);
}

TEST_CASE("board preview composites an embedded image proxy with alpha")
{
    const sawer::DecodedImage pixels{
        1U, 1U, {220U, 40U, 30U, 128U}};
    auto asset = std::make_shared<sawer::ImageAsset>();
    asset->pixel_width = pixels.width;
    asset->pixel_height = pixels.height;
    asset->preview = sawer::encode_png_rgba(pixels);
    sawer::Document document;
    REQUIRE(document.insert(sawer::Object::make_image(
        sawer::ObjectId::from_u64(200U), 0,
        {asset, {0.0, 0.0}, {100.0, 80.0}})));

    const sawer::BoardPreview preview = sawer::rasterize_board_preview(document);
    bool found_transparent_red = false;
    for (std::size_t pixel = 0U; pixel < preview.rgba.size() / 4U; ++pixel) {
        const std::size_t offset = pixel * 4U;
        found_transparent_red = found_transparent_red
            || (preview.rgba[offset] > 180U
                && preview.rgba[offset + 1U] < 80U
                && preview.rgba[offset + 3U] > 80U
                && preview.rgba[offset + 3U] < 180U);
    }
    REQUIRE(found_transparent_red);
}

TEST_CASE("distant thin preview strokes remain one-pixel hairlines")
{
    sawer::Document document;
    sawer::Style style;
    style.stroke = {40U, 90U, 210U, 255U};
    style.stroke_width = 1.0;
    REQUIRE(document.insert(sawer::Object::make_line(
        sawer::ObjectId::from_u64(1U),
        document.next_z_order(),
        {{0.0, 0.0}, {10'000.0, 0.0}},
        style)));

    const sawer::BoardPreview preview =
        sawer::rasterize_board_preview(document);
    const std::size_t column = sawer::BoardPreview::pixel_width / 2U;
    std::uint32_t column_alpha = 0U;
    for (std::size_t row = 0U;
         row < sawer::BoardPreview::pixel_height;
         ++row) {
        column_alpha += preview.rgba[
            (row * sawer::BoardPreview::pixel_width + column) * 4U + 3U];
    }

    // Anti-aliasing may distribute the hairline between adjacent rows, so
    // compare total coverage rather than requiring exactly one lit pixel.
    REQUIRE(column_alpha >= 128U);
    REQUIRE(column_alpha <= 280U);
}
