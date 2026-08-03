#include "app/Application.hpp"
#include "core/BuildInfo.hpp"
#include "core/Log.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

int main(const int argc, char* argv[])
{
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << sawer::BuildInfo::name << ' '
                  << sawer::BuildInfo::version << '\n';
        return 0;
    }

    try {
        if (argc == 3
            && std::string_view{argv[1]} == "--export-diagnostics") {
            const sawer::Application application{true};
            const std::filesystem::path destination{argv[2]};
            const bool write_directly = sawer::log::file_path().empty();
            if (write_directly && !sawer::log::set_file(destination)) {
                throw std::runtime_error{
                    "Could not create diagnostics at "
                    + destination.string()};
            }
            sawer::log::write(
                sawer::log::Level::info,
                application.gpu_diagnostics());
            if (!write_directly
                && !sawer::log::export_file(destination)) {
                throw std::runtime_error{
                    "Could not export diagnostics to "
                    + destination.string()};
            }
            std::cout << "Diagnostics exported to "
                      << destination.string() << '\n';
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

        if (argc == 2 && std::string_view{argv[1]} == "--home-test") {
            sawer::Application application{true};
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

            const std::filesystem::path board_path{argv[1]};
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
