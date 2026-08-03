#pragma once

#include <string_view>

namespace sawer {

struct BuildInfo final {
    static constexpr std::string_view name{"Sawer"};
    static constexpr std::string_view version{"0.9.0"};
    static constexpr std::string_view organization{"Sawer"};
    static constexpr std::string_view identifier{"io.sawer.app"};
};

[[nodiscard]] std::string_view build_configuration() noexcept;

} // namespace sawer
