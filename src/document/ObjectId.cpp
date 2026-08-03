#include "document/ObjectId.hpp"

#include <algorithm>
#include <random>

namespace sawer {
namespace {

int hex_value(const char character) noexcept
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

ObjectId ObjectId::random()
{
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    Storage bytes{};

    for (std::size_t offset = 0; offset < bytes.size(); offset += 8U) {
        const std::uint64_t value = generator();
        for (std::size_t byte = 0; byte < 8U; ++byte) {
            bytes[offset + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8U));
        }
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return ObjectId{bytes};
}

std::optional<ObjectId> ObjectId::parse(const std::string_view text) noexcept
{
    if (text.size() != 36U
        || text[8] != '-'
        || text[13] != '-'
        || text[18] != '-'
        || text[23] != '-') {
        return std::nullopt;
    }

    Storage bytes{};
    std::size_t source = 0U;
    for (auto& byte : bytes) {
        while (source < text.size() && text[source] == '-') {
            ++source;
        }
        if (source + 1U >= text.size()) {
            return std::nullopt;
        }
        const int high = hex_value(text[source]);
        const int low = hex_value(text[source + 1U]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        byte = static_cast<std::uint8_t>((high << 4) | low);
        source += 2U;
    }

    return ObjectId{bytes};
}

const ObjectId::Storage& ObjectId::bytes() const noexcept
{
    return bytes_;
}

std::string ObjectId::to_string() const
{
    constexpr char digits[] = "0123456789abcdef";
    constexpr std::array<std::size_t, 4> separators{4U, 6U, 8U, 10U};

    std::string result;
    result.reserve(36U);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (std::ranges::find(separators, index) != separators.end()) {
            result.push_back('-');
        }
        result.push_back(digits[bytes_[index] >> 4U]);
        result.push_back(digits[bytes_[index] & 0x0FU]);
    }
    return result;
}

std::size_t ObjectIdHash::operator()(const ObjectId& id) const noexcept
{
    constexpr std::size_t offset =
        sizeof(std::size_t) == 8U
        ? static_cast<std::size_t>(14'695'981'039'346'656'037ULL)
        : static_cast<std::size_t>(2'166'136'261U);
    constexpr std::size_t prime =
        sizeof(std::size_t) == 8U
        ? static_cast<std::size_t>(1'099'511'628'211ULL)
        : static_cast<std::size_t>(16'777'619U);

    std::size_t hash = offset;
    for (const auto byte : id.bytes()) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

} // namespace sawer
