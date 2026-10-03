#pragma once

#include <string>
#include <vector>

namespace sawer {

[[nodiscard]] std::vector<std::string> command_line_arguments(
    int argc, char* const argv[]);

} // namespace sawer
