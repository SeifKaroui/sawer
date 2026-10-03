#include "app/Application.hpp"
#include "core/BuildInfo.hpp"
#include "core/CommandLine.hpp"
#include "core/Filesystem.hpp"
#include "core/Log.hpp"
#include "core/ThirdPartyNotices.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int run_application(const std::vector<std::string>& argv)
{
    const auto argc = argv.size();
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << sawer::BuildInfo::name << ' '
                  << sawer::BuildInfo::version << '\n';
        return 0;
    }

    if (argc == 2
        && std::string_view{argv[1]} == "--third-party-notices") {
#ifdef _WIN32
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            std::cerr << "Could not configure notice output" << '\n';
            return 1;
        }
#endif
        std::cout << sawer::third_party_notices();
        return 0;
    }

    try {
        if (argc == 3
            && std::string_view{argv[1]} == "--export-diagnostics") {
            const sawer::Application application{true};
            const auto destination = sawer::path_from_utf8(argv[2]);
            const bool write_directly = sawer::log::file_path().empty();
            if (write_directly && !sawer::log::set_file(destination)) {
                throw std::runtime_error{
                    "Could not create diagnostics at "
                    + sawer::path_to_utf8(destination)};
            }
            sawer::log::write(
                sawer::log::Level::info,
                application.gpu_diagnostics());
            if (!write_directly
                && !sawer::log::export_file(destination)) {
                throw std::runtime_error{
                    "Could not export diagnostics to "
                    + sawer::path_to_utf8(destination)};
            }
            std::cout << "Diagnostics exported to "
                      << sawer::path_to_utf8(destination) << '\n';
            return 0;
        }

        if (argc == 2 && std::string_view{argv[1]} == "--smoke-test") {
            const sawer::Application application{true};
            return 0;
        }

        if (argc == 2 && std::string_view{argv[1]} == "--gpu-info") {
            const sawer::Application application{true};
            std::cout << application.gpu_diagnostics() << '\n';
            return 0;
        }

        if (argc == 2 && std::string_view{argv[1]} == "--render-test") {
            sawer::Application application;
            return application.run_line_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--board-loading-test") {
            sawer::Application application;
            return application.run_board_loading_test();
        }

        if (argc == 2
            && std::string_view{argv[1]} == "--renderer-recovery-test") {
            sawer::Application application;
            return application.run_renderer_recovery_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--resize-test") {
            sawer::Application application;
            return application.run_resize_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--input-test") {
            sawer::Application application{true};
            return application.run_input_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--ui-test") {
            sawer::Application application;
            return application.run_ui_input_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--selection-test") {
            sawer::Application application;
            return application.run_selection_input_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--rendering-performance-test") {
            sawer::Application application;
            return application.run_rendering_performance_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--large-board-test") {
            sawer::Application application;
            return application.run_large_board_test();
        }

        if (argc == 2
            && std::string_view{argv[1]}
                == "--drawing-performance-test") {
            sawer::Application application;
            return application.run_drawing_performance_test();
        }

        if (argc == 2
            && std::string_view{argv[1]}
                == "--buffer-growth-test") {
            sawer::Application application;
            return application.run_buffer_growth_test();
        }

        if ((argc == 2 || argc == 3)
            && std::string_view{argv[1]} == "--stroke-visibility-test") {
            sawer::Application application;
            return application.run_stroke_visibility_test(
                argc == 3 ? sawer::path_from_utf8(argv[2]) : std::filesystem::path{});
        }

        if (argc == 2 && std::string_view{argv[1]} == "--navigation-transition-test") {
            // Vulkan does not provide a swapchain image for a hidden window.
            sawer::Application application;
            return application.run_navigation_transition_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--home-test") {
            sawer::Application application;
            return application.run_home_test();
        }

        if (argc == 2 && std::string_view{argv[1]} == "--zoom-state-test") {
            sawer::Application application{true};
            return application.run_zoom_state_test();
        }

        sawer::Application application;
        if (argc == 2) {
            const std::string_view argument{argv[1]};
            if (argument.starts_with("--")) {
                throw std::invalid_argument{
                    "Unknown command-line option: " + std::string{argument}};
            }

            const auto board_path = sawer::path_from_utf8(argv[1]);
            if (std::filesystem::exists(board_path)) {
                application.open_board(board_path);
            } else {
                application.save_as(board_path);
            }
        } else if (argc > 2) {
            throw std::invalid_argument{"Expected at most one board path"};
        }
        return application.run();
    } catch (const std::exception& error) {
        sawer::log::write(sawer::log::Level::error, error.what());
        return 1;
    }
}

int main(const int argc, char* argv[])
{
    try {
        return run_application(sawer::command_line_arguments(argc, argv));
    } catch (const std::exception& error) {
        sawer::log::write(sawer::log::Level::error, error.what());
        return 1;
    }
}
