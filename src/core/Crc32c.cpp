#include "core/Crc32c.hpp"

#include <array>

namespace sawer {
namespace {

constexpr auto make_crc_table()
{
    std::array<std::uint32_t, 256U> table{};
    for (std::size_t index = 0U; index < table.size(); ++index) {
        auto crc = static_cast<std::uint32_t>(index);
        for (std::size_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0x82F63B78U : 0U);
        }
        table[index] = crc;
    }
    return table;
}

constexpr auto crc_table = make_crc_table();

} // namespace

std::uint32_t crc32c_extend_portable(
    const std::uint32_t checksum,
    const std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t crc = ~checksum;
    for (const std::uint8_t byte : bytes) {
        crc = (crc >> 8U) ^ crc_table[(crc ^ byte) & 0xFFU];
    }
    return ~crc;
}

std::uint32_t crc32c_extend(
    const std::uint32_t checksum,
    const std::span<const std::uint8_t> bytes) noexcept
{
    return crc32c_extend_portable(checksum, bytes);
}

} // namespace sawer
