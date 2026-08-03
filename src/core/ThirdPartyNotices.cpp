#include "core/ThirdPartyNotices.hpp"

#include "notices/EmbeddedNotices.hpp"

namespace sawer {

std::string_view third_party_notices() noexcept
{
    return {
        reinterpret_cast<const char*>(assets::third_party_notices),
        assets::third_party_notices_size,
    };
}

} // namespace sawer
