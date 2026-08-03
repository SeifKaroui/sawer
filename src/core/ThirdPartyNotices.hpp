#pragma once

#include <string_view>

namespace sawer {

// The exact THIRD_PARTY_NOTICES.md contents embedded at build time. Keeping
// this API platform-neutral lets every self-contained Sawer binary expose the
// same offline notices through both the UI and command line.
[[nodiscard]] std::string_view third_party_notices() noexcept;

} // namespace sawer
