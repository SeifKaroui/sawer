#include "core/BuildInfo.hpp"

namespace sawer {

std::string_view build_configuration() noexcept
{
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

} // namespace sawer

