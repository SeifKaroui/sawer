#include "core/CommandLine.hpp"
#include "core/Filesystem.hpp"

#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace sawer {

std::vector<std::string> command_line_arguments(
    const int argc, char* const argv[])
{
    std::vector<std::string> arguments;
#if defined(_WIN32)
    static_cast<void>(argc);
    static_cast<void>(argv);
    int count{};
    wchar_t** const wide_arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide_arguments == nullptr) {
        throw std::runtime_error{"Could not read the Unicode command line"};
    }
    struct Cleanup final {
        wchar_t** arguments;
        ~Cleanup() { LocalFree(arguments); }
    } cleanup{wide_arguments};
    arguments.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        arguments.push_back(path_to_utf8(std::filesystem::path{wide_arguments[index]}));
    }
#else
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
#endif
    return arguments;
}

} // namespace sawer
