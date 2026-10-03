#include "image/ImageCodec.hpp"
#include "image/ImageDecodeCache.hpp"
#include "image/Sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <vector>

namespace {
void put_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t i = 0; i < 4U; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}
void put_u16(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value); bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}
} // namespace

TEST_CASE("uncompressed BMP clipboard pixels decode to RGBA")
{
    std::vector<std::uint8_t> bmp(58U, 0U);
    bmp[0] = 'B'; bmp[1] = 'M'; put_u32(bmp, 2U, 58U); put_u32(bmp, 10U, 54U);
    put_u32(bmp, 14U, 40U); put_u32(bmp, 18U, 1U); put_u32(bmp, 22U, 1U);
    put_u16(bmp, 26U, 1U); put_u16(bmp, 28U, 24U); put_u32(bmp, 34U, 4U);
    bmp[54] = 30U; bmp[55] = 20U; bmp[56] = 10U;
    const auto decoded = sawer::decode_bmp_rgba(bmp);
    REQUIRE(decoded.width == 1U); REQUIRE(decoded.height == 1U);
    REQUIRE(decoded.rgba == std::vector<std::uint8_t>{10U, 20U, 30U, 255U});
    const auto png = sawer::encode_png_rgba(decoded);
    const auto round_trip = sawer::decode_image_rgba(png);
    REQUIRE(round_trip.rgba == decoded.rgba);
}

TEST_CASE("SHA-256 matches the standard empty input vector")
{
    const auto digest = sawer::sha256({});
    const std::array<std::uint8_t, 32U> expected{
        0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U,
        0x9aU, 0xfbU, 0xf4U, 0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U,
        0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU, 0x93U, 0x4cU,
        0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U,
    };
    REQUIRE(digest == expected);
}

TEST_CASE("SHA-256 matches the standard abc vector")
{
    constexpr std::array<std::uint8_t, 3U> input{'a', 'b', 'c'};
    const auto digest = sawer::sha256(input);
    const std::array<std::uint8_t, 32U> expected{
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU,
    };
    REQUIRE(digest == expected);
}

TEST_CASE("canonical image assets preserve alpha and deduplicate by PNG hash")
{
    std::vector<std::uint8_t> bmp(58U, 0U);
    bmp[0] = 'B'; bmp[1] = 'M'; put_u32(bmp, 2U, 58U); put_u32(bmp, 10U, 54U);
    put_u32(bmp, 14U, 40U); put_u32(bmp, 18U, 1U); put_u32(bmp, 22U, 1U);
    put_u16(bmp, 26U, 1U); put_u16(bmp, 28U, 32U); put_u32(bmp, 34U, 4U);
    bmp[54] = 12U; bmp[55] = 34U; bmp[56] = 56U; bmp[57] = 78U;
    const auto asset = sawer::canonicalize_image(bmp);
    REQUIRE(asset.asset->id == sawer::sha256(asset.asset->png));
    REQUIRE(asset.asset->pixel_width == 1U);
    REQUIRE(asset.decoded->rgba == std::vector<std::uint8_t>{56U, 34U, 12U, 78U});
    REQUIRE_FALSE(asset.asset->preview.empty());
}

TEST_CASE("image decoding rejects malformed and oversized encoded input")
{
    const std::array<std::uint8_t, 8U> malformed{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    REQUIRE_THROWS(sawer::decode_image_rgba(malformed));

    const sawer::DecodedImage one_pixel{1U, 1U, {1U, 2U, 3U, 4U}};
    auto oversized_header = sawer::encode_png_rgba(one_pixel);
    // stb_image deliberately does not require the IHDR CRC for discovery;
    // changing the width exercises Sawer's dimension gate without allocating.
    oversized_header[16] = 0U;
    oversized_header[17] = 0U;
    oversized_header[18] = 0x40U;
    oversized_header[19] = 0x01U;
    REQUIRE_THROWS(sawer::decode_image_rgba(oversized_header));
}

TEST_CASE("decoded image cache evicts least recently used pixels within budget")
{
    sawer::ImageDecodeCache cache{12U};
    sawer::AssetId first{};
    sawer::AssetId second{};
    first[0] = 1U;
    second[0] = 2U;
    auto a = std::make_shared<sawer::DecodedImage>(
        sawer::DecodedImage{1U, 2U, std::vector<std::uint8_t>(8U, 1U)});
    auto b = std::make_shared<sawer::DecodedImage>(
        sawer::DecodedImage{1U, 2U, std::vector<std::uint8_t>(8U, 2U)});
    cache.insert(first, a);
    REQUIRE(cache.find(first) == a);
    cache.insert(second, b);
    REQUIRE(cache.bytes() == 8U);
    REQUIRE_FALSE(cache.find(first));
    REQUIRE(cache.find(second) == b);
}

TEST_CASE("decoded image cache transfers ownership and releases replaced pixels")
{
    sawer::AssetId id{};
    auto image = std::make_shared<sawer::DecodedImage>(
        sawer::DecodedImage{2U, 1U, std::vector<std::uint8_t>(8U, 5U)});
    sawer::ImageDecodeCache prepared{12U};
    prepared.insert(id, image);
    sawer::ImageDecodeCache transfer{std::move(prepared)};
    REQUIRE(prepared.bytes() == 0U);
    REQUIRE_FALSE(prepared.find(id));
    REQUIRE(transfer.find(id) == image);
    REQUIRE(transfer.bytes() == 8U);

    sawer::ImageDecodeCache renderer{4U};
    sawer::AssetId old_id{};
    old_id[0] = 1U;
    auto old = std::make_shared<sawer::DecodedImage>(
        sawer::DecodedImage{1U, 1U, std::vector<std::uint8_t>(4U, 1U)});
    std::weak_ptr<const sawer::DecodedImage> replaced = old;
    renderer.insert(old_id, std::move(old));
    renderer = std::move(transfer);
    REQUIRE(replaced.expired());
    REQUIRE(transfer.bytes() == 0U);
    REQUIRE_FALSE(transfer.find(id));
    REQUIRE(renderer.find(id) == image);
    REQUIRE(renderer.budget() == 12U);
    prepared.insert(id, image);
    REQUIRE(prepared.bytes() == 8U);
    renderer.clear();
    REQUIRE(renderer.bytes() == 0U);
}

TEST_CASE("decoded image cache declines oversized images and survives repeated transfers")
{
    sawer::ImageDecodeCache renderer{4U};
    sawer::AssetId id{};
    const auto oversized = std::make_shared<sawer::DecodedImage>(
        sawer::DecodedImage{2U, 1U, std::vector<std::uint8_t>(8U, 0U)});
    renderer.insert(id, oversized);
    REQUIRE(renderer.bytes() == 0U);
    REQUIRE_FALSE(renderer.find(id));
    for (unsigned index = 0U; index < 100U; ++index) {
        sawer::ImageDecodeCache incoming{4U};
        incoming.insert(id, std::make_shared<sawer::DecodedImage>(
            sawer::DecodedImage{1U, 1U, std::vector<std::uint8_t>(4U, 1U)}));
        renderer = std::move(incoming);
        REQUIRE(renderer.bytes() == 4U);
        REQUIRE(incoming.bytes() == 0U);
    }
}
