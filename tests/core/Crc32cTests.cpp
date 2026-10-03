#include "core/Crc32c.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <random>
#include <vector>

namespace {

std::uint32_t reference_crc32c(const std::span<const std::uint8_t> bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0x82F63B78U : 0U);
        }
    }
    return ~crc;
}

} // namespace

TEST_CASE("CRC32C matches the standard check vector and empty extension")
{
    constexpr std::array<std::uint8_t, 9U> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    REQUIRE(sawer::crc32c_extend(0U, {}) == 0U);
    REQUIRE(sawer::crc32c_extend(0U, input) == 0xE3069283U);
    REQUIRE(sawer::crc32c_extend_portable(0U, input) == 0xE3069283U);
    REQUIRE(sawer::crc32c_extend(0x12345678U, {}) == 0x12345678U);
    REQUIRE(sawer::crc32c_extend_portable(0x12345678U, {}) == 0x12345678U);
}

TEST_CASE("CRC32C matches the original algorithm for unaligned and chunked input")
{
    std::mt19937 random{0xC32CU};
    std::vector<std::uint8_t> storage(8192U + 8U);
    for (auto& byte : storage) byte = static_cast<std::uint8_t>(random());
    for (std::size_t offset = 0U; offset < 8U; ++offset) {
        for (const std::size_t size : {0U, 1U, 7U, 8U, 9U, 31U, 32U,
                                      33U, 255U, 1024U, 8192U}) {
            const auto input = std::span<const std::uint8_t>{storage}.subspan(offset, size);
            const auto expected = reference_crc32c(input);
            REQUIRE(sawer::crc32c_extend(0U, input) == expected);
            REQUIRE(sawer::crc32c_extend_portable(0U, input) == expected);
            const std::size_t split = size / 3U;
            const auto first = sawer::crc32c_extend(0U, input.first(split));
            REQUIRE(sawer::crc32c_extend(first, input.subspan(split)) == expected);
            const auto portable_first =
                sawer::crc32c_extend_portable(0U, input.first(split));
            REQUIRE(sawer::crc32c_extend_portable(portable_first, input.subspan(split)) == expected);
            REQUIRE(sawer::crc32c_extend(first, input.subspan(split))
                == sawer::crc32c_extend_portable(first, input.subspan(split)));
        }
    }
}
