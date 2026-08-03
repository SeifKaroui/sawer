#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sawer {

class ObjectId final {
public:
    using Storage = std::array<std::uint8_t, 16>;

    ObjectId() = default;
    explicit constexpr ObjectId(Storage bytes) noexcept
        : bytes_{bytes}
    {
    }

    [[nodiscard]] static ObjectId random();
    [[nodiscard]] static std::optional<ObjectId> parse(
        std::string_view text) noexcept;
    [[nodiscard]] static constexpr ObjectId from_u64(
        const std::uint64_t value) noexcept
    {
        Storage bytes{};
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            bytes[bytes.size() - 1U - index] =
                static_cast<std::uint8_t>(value >> (index * 8U));
        }
        return ObjectId{bytes};
    }

    [[nodiscard]] const Storage& bytes() const noexcept;
    [[nodiscard]] std::string to_string() const;

    friend constexpr auto operator<=>(const ObjectId&, const ObjectId&) = default;

private:
    Storage bytes_{};
};

struct ObjectIdHash final {
    [[nodiscard]] std::size_t operator()(const ObjectId& id) const noexcept;
};

} // namespace sawer
