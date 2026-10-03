#include "storage/BoardFile.hpp"
#include "core/CommandLine.hpp"
#include "core/Filesystem.hpp"
#include "storage/SawerRecord.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

struct BoardInfo final {
    std::uint64_t frames{};
    std::uint64_t operations{};
    std::uint64_t assets{};
    std::uint64_t last_sequence{};
};

[[nodiscard]] std::string hex_bytes(const std::vector<std::uint8_t>& bytes)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] Json printable_json(const Json& source)
{
    if (source.is_binary()) {
        return hex_bytes(source.get_binary());
    }
    if (source.is_array()) {
        Json result = Json::array();
        for (const Json& value : source) result.push_back(printable_json(value));
        return result;
    }
    if (source.is_object()) {
        Json result = Json::object();
        for (auto it = source.begin(); it != source.end(); ++it) {
            result[it.key()] = printable_json(it.value());
        }
        return result;
    }
    return source;
}

template <typename FrameVisitor>
BoardInfo inspect(const std::filesystem::path& path, FrameVisitor&& visitor)
{
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"cannot open board"};
    sawer::validate_sawer_file_prologue(input);
    BoardInfo info;
    bool has_header = false;
    for (;;) {
        sawer::SawerRecord record;
        const auto result = sawer::read_sawer_record(
            input, record, 96U * 1024U * 1024U, 512U * 1024U * 1024U);
        if (result == sawer::SawerRecordReadResult::end_of_file) break;
        if (result == sawer::SawerRecordReadResult::truncated) {
            throw std::runtime_error{"truncated final frame"};
        }
        if (!has_header) {
            if (record.kind != sawer::SawerRecordKind::file_header
                || record.sequence != 0U) {
                throw std::runtime_error{"first frame is not the file header"};
            }
            has_header = true;
        } else if (record.kind == sawer::SawerRecordKind::operation) {
            if (record.sequence != info.last_sequence + 1U) {
                throw std::runtime_error{"non-contiguous operation sequence"};
            }
            info.last_sequence = record.sequence;
            ++info.operations;
        } else if (record.kind == sawer::SawerRecordKind::asset) {
            if (record.sequence != 0U) {
                throw std::runtime_error{"asset frame has a non-zero sequence"};
            }
            ++info.assets;
        } else {
            throw std::runtime_error{"unexpected file-header frame"};
        }
        ++info.frames;
        visitor(record);
    }
    if (!has_header) throw std::runtime_error{"missing file header"};
    return info;
}

void extract(
    const std::filesystem::path& board_path,
    const std::filesystem::path& output_directory)
{
    std::error_code error;
    if (!std::filesystem::create_directories(output_directory, error) && error) {
        throw std::runtime_error{"cannot create output directory: " + error.message()};
    }
    Json manifest{{"format", "sawer-format extract"}, {"assets", Json::array()}};
    static_cast<void>(inspect(board_path, [&](const sawer::SawerRecord& record) {
        if (record.kind != sawer::SawerRecordKind::asset) return;
        const Json& id = record.metadata.at("asset_id");
        if (!id.is_binary() || id.get_binary().size() != 32U) {
            throw std::runtime_error{"asset has invalid identifier"};
        }
        const std::string name = hex_bytes(id.get_binary()) + ".png";
        const std::filesystem::path output_path = output_directory / name;
        if (!std::filesystem::exists(output_path)) {
            std::ofstream output{output_path, std::ios::binary | std::ios::trunc};
            if (!output) throw std::runtime_error{"cannot write asset"};
            output.write(reinterpret_cast<const char*>(record.payload.data()),
                static_cast<std::streamsize>(record.payload.size()));
            if (!output) throw std::runtime_error{"cannot write asset"};
        }
        manifest["assets"].push_back({
            {"file", name},
            {"metadata", printable_json(record.metadata)},
        });
    }));
    std::ofstream manifest_file{output_directory / "manifest.json", std::ios::trunc};
    if (!manifest_file) throw std::runtime_error{"cannot write manifest"};
    manifest_file << manifest.dump(2) << '\n';
    if (!manifest_file) throw std::runtime_error{"cannot write manifest"};
}

void compact(const std::filesystem::path& path)
{
    sawer::Document document;
    bool recovered = false;
    auto session = sawer::BoardFileSession::open(path, document, recovered);
    session.compact(document);
    std::cout << "compacted" << (recovered ? " after trailing-frame recovery" : "") << '\n';
}

void validate_semantics(const std::filesystem::path& path)
{
    // BoardFileSession is the authoritative semantic validator (object
    // bounds, operation references, canonical assets, hashes and previews).
    // Validate a private copy so recovery of a torn final frame can never
    // modify the board the user asked us to inspect.
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path()
        / ("sawer-validate-" + sawer::ObjectId::random().to_string()
            + ".sawer");
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temporary};
    std::error_code error;
    std::filesystem::copy_file(
        path, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (error) throw std::runtime_error{"cannot create validation copy: " + error.message()};
    sawer::Document document;
    bool recovered = false;
    static_cast<void>(sawer::BoardFileSession::open(
        temporary, document, recovered));
}

void print_usage()
{
    std::cerr << "usage: sawer-format <info|validate|dump> board.sawer\n"
              << "       sawer-format extract board.sawer output-directory\n"
              << "       sawer-format compact board.sawer\n";
}

} // namespace

static int run_format(const std::vector<std::string>& argv)
{
    const auto argc = argv.size();
    if (argc < 3 || argc > 4) { print_usage(); return 2; }
    const std::string_view command{argv[1]};
    const auto board_path = sawer::path_from_utf8(argv[2]);
    try {
        if (command == "extract") {
            if (argc != 4) { print_usage(); return 2; }
            extract(board_path, sawer::path_from_utf8(argv[3]));
            return 0;
        }
        if (command == "compact") {
            if (argc != 3) { print_usage(); return 2; }
            compact(board_path);
            return 0;
        }
        if (argc != 3 || (command != "info" && command != "validate" && command != "dump")) {
            print_usage(); return 2;
        }
        if (command == "validate") {
            validate_semantics(board_path);
            return 0;
        }
        const BoardInfo info = inspect(board_path, [&](const sawer::SawerRecord& record) {
            if (command == "dump") std::cout << printable_json(record.metadata).dump() << '\n';
        });
        if (command == "info") {
            std::cout << "frames: " << info.frames << '\n'
                      << "operations: " << info.operations << '\n'
                      << "assets: " << info.assets << '\n'
                      << "last sequence: " << info.last_sequence << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sawer-format: " << error.what() << '\n';
        return 1;
    }
}

int main(const int argc, char* argv[])
{
    try {
        return run_format(sawer::command_line_arguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "sawer-format: " << error.what() << '\n';
        return 1;
    }
}
