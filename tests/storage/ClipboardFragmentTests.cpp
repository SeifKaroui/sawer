#include "storage/ClipboardFragment.hpp"

#include "image/ImageCodec.hpp"
#include "image/Sha256.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("mixed Sawer clipboard fragments round trip with deduplicated assets")
{
    sawer::Document source;
    auto asset = std::make_shared<sawer::ImageAsset>();
    const sawer::DecodedImage pixels{2U, 1U,
        {10U, 20U, 30U, 255U, 50U, 60U, 70U, 100U}};
    asset->pixel_width = pixels.width;
    asset->pixel_height = pixels.height;
    asset->png = sawer::encode_png_rgba(pixels);
    asset->preview = asset->png;
    asset->id = sawer::sha256(asset->png);
    const auto line_id = sawer::ObjectId::from_u64(1U);
    const auto first_image_id = sawer::ObjectId::from_u64(2U);
    const auto second_image_id = sawer::ObjectId::from_u64(3U);
    REQUIRE(source.insert(sawer::Object::make_line(
        line_id, 3, {{1.0, 2.0}, {30.0, 40.0}})));
    REQUIRE(source.insert(sawer::Object::make_image(
        first_image_id, 4, {asset, {50.0, 60.0}, {70.0, 80.0}})));
    REQUIRE(source.insert(sawer::Object::make_image(
        second_image_id, 5, {asset, {80.0, 90.0}, {100.0, 110.0}})));

    const std::array ids{second_image_id, line_id, first_image_id};
    const auto bytes = sawer::serialize_clipboard_fragment(source, ids);
    auto decoded = sawer::deserialize_clipboard_fragment(bytes);

    REQUIRE(decoded.objects.size() == 3U);
    REQUIRE(std::holds_alternative<sawer::Line>(decoded.objects[0].geometry));
    const auto& first = std::get<sawer::Image>(decoded.objects[1].geometry);
    const auto& second = std::get<sawer::Image>(decoded.objects[2].geometry);
    REQUIRE(first.asset == second.asset);
    REQUIRE(first.asset->png == asset->png);
    REQUIRE(first.asset->id == asset->id);
}

TEST_CASE("Sawer clipboard fragments reject truncation and semantic corruption")
{
    sawer::Document source;
    const auto id = sawer::ObjectId::from_u64(10U);
    REQUIRE(source.insert(sawer::Object::make_rectangle(
        id, 0, {{-10.0, -5.0}, {20.0, 30.0}, 0.25})));
    const std::array ids{id};
    auto bytes = sawer::serialize_clipboard_fragment(source, ids);
    bytes.pop_back();
    REQUIRE_THROWS(sawer::deserialize_clipboard_fragment(bytes));
    bytes = sawer::serialize_clipboard_fragment(source, ids);
    bytes[0] ^= 0x01U;
    REQUIRE_THROWS(sawer::deserialize_clipboard_fragment(bytes));
}
