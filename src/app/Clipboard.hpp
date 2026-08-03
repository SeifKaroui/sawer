#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sawer {
class Clipboard final {
public:
    using Payload = std::vector<std::pair<std::string, std::vector<std::uint8_t>>>;

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(std::string_view mime_type) const;
    [[nodiscard]] bool has(std::string_view mime_type) const;
    // SDL requests payload lazily. Ownership transfers to SDL's cleanup
    // callback only after this returns true, so a failed offer leaves no
    // dangling provider and callers can safely decide whether to cut.
    [[nodiscard]] bool offer(Payload payload) const;
};
} // namespace sawer
