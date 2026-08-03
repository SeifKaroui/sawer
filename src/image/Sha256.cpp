#include "image/Sha256.hpp"

#include <array>
#include <bit>
#include <cstddef>

namespace sawer {
namespace {

constexpr std::array<std::uint32_t, 64U> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr std::uint32_t load_be(const std::uint8_t* const bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U)
        | (static_cast<std::uint32_t>(bytes[1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[2]) << 8U)
        | static_cast<std::uint32_t>(bytes[3]);
}

constexpr void store_be(
    std::uint8_t* const bytes,
    const std::uint32_t value) noexcept
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void compress(
    std::array<std::uint32_t, 8U>& state,
    const std::uint8_t* const block) noexcept
{
    std::array<std::uint32_t, 64U> schedule{};
    for (std::size_t index = 0U; index < 16U; ++index) {
        schedule[index] = load_be(block + index * 4U);
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
        const std::uint32_t sigma0 = std::rotr(schedule[index - 15U], 7)
            ^ std::rotr(schedule[index - 15U], 18)
            ^ (schedule[index - 15U] >> 3U);
        const std::uint32_t sigma1 = std::rotr(schedule[index - 2U], 17)
            ^ std::rotr(schedule[index - 2U], 19)
            ^ (schedule[index - 2U] >> 10U);
        schedule[index] = schedule[index - 16U] + sigma0
            + schedule[index - 7U] + sigma1;
    }

    std::uint32_t a = state[0]; std::uint32_t b = state[1];
    std::uint32_t c = state[2]; std::uint32_t d = state[3];
    std::uint32_t e = state[4]; std::uint32_t f = state[5];
    std::uint32_t g = state[6]; std::uint32_t h = state[7];
    for (std::size_t index = 0U; index < schedule.size(); ++index) {
        const std::uint32_t sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11)
            ^ std::rotr(e, 25);
        const std::uint32_t choice = (e & f) ^ ((~e) & g);
        const std::uint32_t temporary1 = h + sigma1 + choice
            + round_constants[index] + schedule[index];
        const std::uint32_t sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13)
            ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sigma0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace

AssetId sha256(const std::span<const std::uint8_t> bytes) noexcept
{
    std::array<std::uint32_t, 8U> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::size_t offset = 0U;
    while (offset + 64U <= bytes.size()) {
        compress(state, bytes.data() + offset);
        offset += 64U;
    }
    std::array<std::uint8_t, 128U> tail{};
    const std::size_t remaining = bytes.size() - offset;
    for (std::size_t index = 0U; index < remaining; ++index) {
        tail[index] = bytes[offset + index];
    }
    tail[remaining] = 0x80U;
    const std::uint64_t bit_count = static_cast<std::uint64_t>(bytes.size()) * 8U;
    const std::size_t padded_size = remaining + 1U + 8U <= 64U ? 64U : 128U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        tail[padded_size - 1U - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
    }
    compress(state, tail.data());
    if (padded_size == 128U) compress(state, tail.data() + 64U);

    AssetId result{};
    for (std::size_t index = 0U; index < state.size(); ++index) {
        store_be(result.data() + index * 4U, state[index]);
    }
    return result;
}

} // namespace sawer
