#pragma once

#include <cstdint>
#include <span>

namespace sawer {

// Extends a finalized CRC32C checksum; start with zero for a new message.
// The portable path is also exposed for compatibility checks and benchmarks.
[[nodiscard]] std::uint32_t crc32c_extend_portable(
    std::uint32_t checksum, std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(
    std::uint32_t checksum, std::span<const std::uint8_t> bytes) noexcept;

} // namespace sawer
