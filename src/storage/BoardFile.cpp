#include "storage/BoardFile.hpp"

#include "storage/SawerRecord.hpp"

#include "document/Object.hpp"
#include "image/ImageCodec.hpp"
#include "image/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sawer {
namespace {

using Json = nlohmann::json;
using AssetTable = std::map<AssetId, std::shared_ptr<const ImageAsset>>;

constexpr int format_version = 1;
constexpr std::uint64_t maximum_excess_operations = 4'096U;
constexpr std::uint64_t byte_pressure_excess_operations = 512U;
constexpr std::uintmax_t compaction_byte_pressure =
    64U * 1024U * 1024U;
constexpr std::size_t maximum_record_bytes = 256U * 1024U * 1024U;
constexpr std::size_t maximum_preview_record_bytes = 8U * 1024U * 1024U;
constexpr std::size_t line_read_chunk_bytes = 64U * 1024U;

enum class LineState {
    end_of_file,
    terminated,
    unterminated,
};

[[noreturn]] void storage_error(const std::string& message)
{
    throw std::runtime_error{"Sawer file error: " + message};
}

LineState read_bounded_line(
    std::istream& input,
    std::string& line,
    const std::size_t maximum_bytes)
{
    line.clear();
    std::array<char, line_read_chunk_bytes> chunk{};
    bool extracted_any = false;
    for (;;) {
        input.getline(
            chunk.data(),
            static_cast<std::streamsize>(chunk.size()),
            '\n');
        const std::size_t consumed =
            static_cast<std::size_t>(input.gcount());
        const std::size_t extracted =
            !input.eof() && !input.fail() && consumed > 0U
            ? consumed - 1U
            : consumed;
        extracted_any = extracted_any || extracted != 0U;
        if (extracted > maximum_bytes
            || line.size() > maximum_bytes - extracted) {
            storage_error("JSONL record exceeds its safety limit");
        }
        line.append(chunk.data(), extracted);

        if (input.bad()) {
            storage_error("I/O failure while reading");
        }
        if (input.eof()) {
            return extracted_any
                ? LineState::unterminated
                : LineState::end_of_file;
        }
        if (input.fail()) {
            input.clear(input.rdstate() & ~std::ios::failbit);
            continue;
        }
        return LineState::terminated;
    }
}

std::uint64_t next_sequence(const std::uint64_t current)
{
    if (current == std::numeric_limits<std::uint64_t>::max()) {
        storage_error("operation sequence is exhausted");
    }
    return current + 1U;
}

[[nodiscard]] std::set<AssetId> referenced_assets(const Document& document)
{
    std::set<AssetId> result;
    for (const Object* const object : document.all_objects()) {
        if (const auto* const image = std::get_if<Image>(&object->geometry)) {
            result.insert(image->asset->id);
        }
    }
    return result;
}

ObjectId parse_id(const Json& value)
{
    if (value.is_binary()) {
        const auto& bytes = value.get_binary();
        if (bytes.size() != ObjectId::Storage{}.size()) {
            storage_error("invalid binary object identifier");
        }
        ObjectId::Storage id_bytes{};
        std::copy(bytes.begin(), bytes.end(), id_bytes.begin());
        return ObjectId{id_bytes};
    }
    const auto parsed = ObjectId::parse(value.get<std::string>());
    if (!parsed.has_value()) {
        storage_error("invalid object identifier");
    }
    return *parsed;
}

Json serialize_binary_id(const ObjectId id)
{
    return Json::binary(std::vector<std::uint8_t>{
        id.bytes().begin(), id.bytes().end()});
}

Json serialize_asset_id(const AssetId& id)
{
    return Json::binary(std::vector<std::uint8_t>{id.begin(), id.end()});
}

AssetId parse_asset_id(const Json& value)
{
    if (!value.is_binary() || value.get_binary().size() != AssetId{}.size()) {
        storage_error("invalid asset identifier");
    }
    AssetId id{};
    std::copy(value.get_binary().begin(), value.get_binary().end(), id.begin());
    return id;
}

Json serialize_asset_metadata(const ImageAsset& asset)
{
    return {
        {"op", "asset_put"},
        {"asset_id", serialize_asset_id(asset.id)},
        {"mime", "image/png"},
        {"width", asset.pixel_width},
        {"height", asset.pixel_height},
        {"preview", Json::binary(asset.preview)},
    };
}

void validate_canonical_asset(const ImageAsset& asset)
{
    const DecodedImage decoded = decode_image_rgba(asset.png);
    if (decoded.width != asset.pixel_width || decoded.height != asset.pixel_height
        || encode_png_rgba(decoded) != asset.png) {
        storage_error("image asset is not a canonical PNG");
    }
    if (!asset.preview.empty()) {
        const DecodedImage preview = decode_image_rgba(asset.preview);
        if (preview.width > 256U || preview.height > 256U) {
            storage_error("image preview exceeds its size limit");
        }
    }
}

void write_asset_record(std::ostream& output, const ImageAsset& asset)
{
    if (asset.id != sha256(asset.png)) {
        storage_error("image asset ID does not match canonical PNG payload");
    }
    validate_canonical_asset(asset);
    write_sawer_record(output, {
        .kind = SawerRecordKind::asset,
        .sequence = 0U,
        .metadata = serialize_asset_metadata(asset),
        .payload = asset.png,
    });
}

void replay_asset(const SawerRecord& record, AssetTable& assets)
{
    if (record.metadata.at("op").get<std::string>() != "asset_put"
        || record.metadata.at("mime").get<std::string>() != "image/png") {
        storage_error("unsupported asset record");
    }
    auto asset = std::make_shared<ImageAsset>();
    asset->id = parse_asset_id(record.metadata.at("asset_id"));
    asset->pixel_width = record.metadata.at("width").get<std::uint32_t>();
    asset->pixel_height = record.metadata.at("height").get<std::uint32_t>();
    if (asset->pixel_width == 0U || asset->pixel_height == 0U
        || record.payload.empty()) {
        storage_error("invalid image asset");
    }
    asset->png = record.payload;
    if (asset->id != sha256(asset->png)) {
        storage_error("image asset ID does not match PNG payload");
    }
    validate_canonical_asset(*asset);
    if (record.metadata.contains("preview")) {
        const auto& preview = record.metadata.at("preview");
        if (!preview.is_binary()) storage_error("invalid image preview");
        asset->preview = preview.get_binary();
    }
    const auto existing = assets.find(asset->id);
    if (existing != assets.end()) {
        if (existing->second->png != asset->png) storage_error("conflicting image asset");
        return;
    }
    assets.emplace(asset->id, std::move(asset));
}

Json serialize_style(const Style& style)
{
    const Json fill = style.fill.has_value()
        ? Json{
            style.fill->red,
            style.fill->green,
            style.fill->blue,
            style.fill->alpha}
        : Json(nullptr);
    return {
        {"stroke", {
            style.stroke.red,
            style.stroke.green,
            style.stroke.blue,
            style.stroke.alpha}},
        {"fill", fill},
        {"width", style.stroke_width},
    };
}

Json serialize_object(const Object& object)
{
    Json geometry;
    if (const auto* const line = std::get_if<Line>(&object.geometry)) {
        geometry = {
            {"type", "line"},
            {"start", {line->start.x, line->start.y}},
            {"end", {line->end.x, line->end.y}},
        };
    } else if (const auto* const stroke =
                   std::get_if<Stroke>(&object.geometry)) {
        Json points = Json::array();
        for (const auto point : stroke->points) {
            points.push_back({point.x, point.y});
        }
        geometry = {
            {"type", "stroke"},
            {"points", std::move(points)},
        };
    } else if (const auto* const rectangle =
                   std::get_if<RectangleShape>(&object.geometry)) {
        geometry = {
            {"type", "rectangle"},
            {"first", {rectangle->first.x, rectangle->first.y}},
            {"second", {rectangle->second.x, rectangle->second.y}},
            {"roundness", rectangle->roundness},
        };
    } else if (const auto* const ellipse =
                   std::get_if<Ellipse>(&object.geometry)) {
        geometry = {
            {"type", "ellipse"},
            {"first", {ellipse->first.x, ellipse->first.y}},
            {"second", {ellipse->second.x, ellipse->second.y}},
        };
    } else if (const auto* const image = std::get_if<Image>(&object.geometry)) {
        geometry = {
            {"type", "image"},
            {"asset_id", serialize_asset_id(image->asset->id)},
            {"first", {image->first.x, image->first.y}},
            {"second", {image->second.x, image->second.y}},
        };
    } else {
        storage_error("unsupported object type");
    }

    geometry["z"] = object.z_order;
    if (!std::holds_alternative<Image>(object.geometry)) {
        geometry["style"] = serialize_style(object.style);
    }
    return geometry;
}

double finite_number(const Json& value)
{
    const double number = value.get<double>();
    if (!std::isfinite(number)) {
        storage_error("non-finite number");
    }
    return number;
}

Vec2d parse_point(const Json& value)
{
    if (!value.is_array() || value.size() != 2U) {
        storage_error("point must contain two numbers");
    }
    const Vec2d point{finite_number(value[0]), finite_number(value[1])};
    if (std::abs(point.x) > board_half_extent
        || std::abs(point.y) > board_half_extent) {
        storage_error("point is outside board bounds");
    }
    return point;
}

Vec2d parse_delta(const Json& value)
{
    if (!value.is_array() || value.size() != 2U) {
        storage_error("delta must contain two numbers");
    }
    const Vec2d delta{finite_number(value[0]), finite_number(value[1])};
    if (std::abs(delta.x) > board_half_extent * 2.0
        || std::abs(delta.y) > board_half_extent * 2.0) {
        storage_error("move delta is outside board bounds");
    }
    return delta;
}

Color parse_color(const Json& value)
{
    if (!value.is_array() || value.size() != 4U) {
        storage_error("color must contain four channels");
    }

    std::array<std::uint8_t, 4> channels{};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const int channel = value[index].get<int>();
        if (channel < 0 || channel > 255) {
            storage_error("color channel is outside 0..255");
        }
        channels[index] = static_cast<std::uint8_t>(channel);
    }
    return {channels[0], channels[1], channels[2], channels[3]};
}

Style parse_style(const Json& value)
{
    Style style{
        .stroke = parse_color(value.at("stroke")),
        .fill = value.contains("fill") && !value.at("fill").is_null()
            ? std::optional<Color>{parse_color(value.at("fill"))}
            : std::nullopt,
        .stroke_width = finite_number(value.at("width")),
    };
    if (style.stroke_width <= 0.0 || style.stroke_width > 10'000.0) {
        storage_error("invalid stroke width");
    }
    return style;
}

Object parse_object(
    const ObjectId id,
    const Json& value,
    const AssetTable* const assets = nullptr)
{
    const std::string type = value.at("type").get<std::string>();
    const auto z_order = value.at("z").get<std::int64_t>();
    if (z_order == std::numeric_limits<std::int64_t>::max()) {
        storage_error("z-order is exhausted");
    }
    if (type == "image") {
        if (assets == nullptr) storage_error("image asset table is unavailable");
        const AssetId asset_id = parse_asset_id(value.at("asset_id"));
        const auto asset = assets->find(asset_id);
        if (asset == assets->end()) storage_error("image references a missing asset");
        return Object::make_image(id, z_order, {
            asset->second,
            parse_point(value.at("first")),
            parse_point(value.at("second")),
        });
    }
    Style style = parse_style(value.at("style"));
    if (type == "line") {
        return Object::make_line(
            id,
            z_order,
            {
                parse_point(value.at("start")),
                parse_point(value.at("end")),
            },
            style);
    }
    if (type == "stroke") {
        const auto& point_values = value.at("points");
        if (!point_values.is_array() || point_values.empty()) {
            storage_error("stroke must contain points");
        }
        if (point_values.size() > maximum_stroke_point_count) {
            storage_error("stroke contains too many points");
        }
        Stroke stroke;
        stroke.points.reserve(point_values.size());
        for (const auto& point : point_values) {
            stroke.points.push_back(parse_point(point));
        }
        return Object::make_stroke(
            id, z_order, std::move(stroke), style);
    }
    if (type == "rectangle") {
        const double roundness = value.contains("roundness")
            ? finite_number(value.at("roundness"))
            : 0.0;
        if (roundness < 0.0 || roundness > 0.5) {
            storage_error("invalid rectangle roundness");
        }
        return Object::make_rectangle(
            id,
            z_order,
            {
                parse_point(value.at("first")),
                parse_point(value.at("second")),
                roundness,
            },
            style);
    }
    if (type == "ellipse") {
        return Object::make_ellipse(
            id,
            z_order,
            {
                parse_point(value.at("first")),
                parse_point(value.at("second")),
            },
            style);
    }
    storage_error("unsupported object type: " + type);
}

Json make_header(const ObjectId board_id)
{
    return {
        {"format", "sawer"},
        {"version", format_version},
        {"board_id", serialize_binary_id(board_id)},
        {"bounds", {
            -board_half_extent,
            -board_half_extent,
            board_half_extent,
            board_half_extent}},
    };
}

Json make_put(
    const std::uint64_t sequence,
    const Object& object)
{
    return {
        {"seq", sequence},
        {"op", "put"},
        {"id", object.id.to_string()},
        {"object", serialize_object(object)},
    };
}

Json make_delete(
    const std::uint64_t sequence,
    const ObjectId id)
{
    return {
        {"seq", sequence},
        {"op", "delete"},
        {"id", id.to_string()},
    };
}

Json make_move(
    const std::uint64_t sequence,
    const ObjectId id,
    const Vec2d delta)
{
    return {
        {"seq", sequence},
        {"op", "move"},
        {"id", id.to_string()},
        {"delta", {delta.x, delta.y}},
    };
}

Json make_style(
    const std::uint64_t sequence,
    const ObjectId id,
    const Style& style)
{
    return {
        {"seq", sequence},
        {"op", "style"},
        {"id", id.to_string()},
        {"style", serialize_style(style)},
    };
}

void validate_header(const Json& header, ObjectId& board_id)
{
    if (header.at("format").get<std::string>() != "sawer") {
        storage_error("not a Sawer board");
    }
    if (header.at("version").get<int>() != format_version) {
        storage_error("unsupported format version");
    }
    board_id = parse_id(header.at("board_id"));

    const auto& bounds = header.at("bounds");
    if (!bounds.is_array() || bounds.size() != 4U) {
        storage_error("invalid board bounds");
    }
    const std::array expected{
        -board_half_extent,
        -board_half_extent,
        board_half_extent,
        board_half_extent,
    };
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (finite_number(bounds[index]) != expected[index]) {
            storage_error("unsupported board bounds");
        }
    }
}

void apply_operation(
    Document& loaded,
    const Json& record,
    const AssetTable* const assets = nullptr)
{
    const ObjectId id = parse_id(record.at("id"));
    const std::string operation = record.at("op").get<std::string>();
    if (operation == "put") {
        Object object = parse_object(id, record.at("object"), assets);
        if (loaded.find(id) != nullptr) {
            if (!loaded.replace(std::move(object))) {
                storage_error("cannot replace replayed object");
            }
        } else if (!loaded.insert(std::move(object))) {
            storage_error("cannot insert replayed object");
        }
    } else if (operation == "delete") {
        if (!loaded.remove(id).has_value()) {
            storage_error("delete operation references a missing object");
        }
    } else if (operation == "move") {
        if (!loaded.translate(id, parse_delta(record.at("delta")))) {
            storage_error("move operation references a missing object");
        }
    } else if (operation == "style") {
        if (!loaded.exchange_style(id, parse_style(record.at("style"))).has_value()) {
            storage_error("style operation references a missing object");
        }
    } else {
        storage_error("unsupported operation: " + operation);
    }
}

void sync_file(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        storage_error("cannot open file for synchronization");
    }
    const bool success = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!success) {
        storage_error("cannot synchronize file");
    }
#else
    const int file = ::open(path.c_str(), O_RDONLY);
    if (file < 0) {
        storage_error("cannot open file for synchronization");
    }
    const bool success = ::fsync(file) == 0;
    ::close(file);
    if (!success) {
        storage_error("cannot synchronize file");
    }
#endif
}

void atomic_replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)
{
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        storage_error("atomic file replacement failed");
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        storage_error("atomic file replacement failed");
    }
#endif
}

std::filesystem::path temporary_path_for(
    const std::filesystem::path& destination)
{
    return destination.parent_path()
        / (destination.filename().string() + ".tmp-"
           + ObjectId::random().to_string());
}

} // namespace

Document read_board_preview(
    const std::filesystem::path& path,
    const std::size_t max_objects)
{
    Document loaded;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return loaded;
    }

    if (input.peek() == 'S') {
        try {
            validate_sawer_file_prologue(input);
            ObjectId board_id;
            AssetTable assets;
            bool has_header = false;
            for (;;) {
                SawerRecord record;
                const auto result = read_sawer_record(
                    input,
                    record,
                    maximum_preview_record_bytes,
                    64U * 1024U * 1024U);
                if (result != SawerRecordReadResult::record) {
                    break;
                }
                if (!has_header) {
                    if (record.kind != SawerRecordKind::file_header
                        || record.sequence != 0U) {
                        return loaded;
                    }
                    validate_header(record.metadata, board_id);
                    has_header = true;
                } else if (record.kind == SawerRecordKind::asset) {
                    if (record.sequence != 0U) {
                        return loaded;
                    }
                    replay_asset(record, assets);
                } else if (record.kind == SawerRecordKind::operation) {
                    try {
                        apply_operation(loaded, record.metadata, &assets);
                    } catch (const std::exception&) {
                        continue;
                    }
                }
                if (loaded.size() >= max_objects) {
                    break;
                }
            }
        } catch (const std::exception&) {
            return Document{};
        }
        return loaded;
    }

    ObjectId board_id;
    bool has_header = false;
    std::string line;
    for (;;) {
        LineState state;
        try {
            state = read_bounded_line(
                input, line, maximum_preview_record_bytes);
        } catch (const std::exception&) {
            return loaded;
        }
        if (state == LineState::end_of_file) {
            break;
        }
        try {
            const Json record = Json::parse(line);
            if (!has_header) {
                validate_header(record, board_id);
                has_header = true;
                continue;
            }
            const ObjectId id = parse_id(record.at("id"));
            const std::string operation = record.at("op").get<std::string>();
            if (operation == "put") {
                Object object = parse_object(id, record.at("object"));
                if (loaded.find(id) != nullptr) {
                    static_cast<void>(loaded.replace(std::move(object)));
                } else {
                    static_cast<void>(loaded.insert(std::move(object)));
                }
            } else if (operation == "delete") {
                static_cast<void>(loaded.remove(id));
            } else if (operation == "move") {
                static_cast<void>(
                    loaded.translate(id, parse_delta(record.at("delta"))));
            } else if (operation == "style") {
                static_cast<void>(loaded.exchange_style(
                    id, parse_style(record.at("style"))));
            }
        } catch (const std::exception&) {
            // Previews tolerate malformed or partial records; skip them.
            continue;
        }
        if (loaded.size() >= max_objects) {
            break;
        }
    }
    return loaded;
}

BoardFileSession BoardFileSession::create(
    const std::filesystem::path& path,
    const Document& document)
{
    const ObjectId board_id = ObjectId::random();
    const auto sequence = write_snapshot(path, board_id, document);
    return BoardFileSession{
        path, board_id, sequence, capture_stamp(path), referenced_assets(document)};
}

BoardFileSession BoardFileSession::open(
    const std::filesystem::path& path,
    Document& document,
    bool& recovered_final_line)
{
    const FileStamp initial_stamp = capture_stamp(path);
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        storage_error("cannot open " + path.string());
    }

    if (input.peek() == 'S') {
        Document loaded;
        AssetTable assets;
        ObjectId board_id;
        std::uint64_t sequence = 0U;
        bool has_header = false;
        recovered_final_line = false;
        validate_sawer_file_prologue(input);

        for (;;) {
            SawerRecord record;
            SawerRecordReadResult result;
            try {
                result = read_sawer_record(
                    input, record, maximum_record_bytes, maximum_record_bytes);
            } catch (const SawerRecordCrcError& error) {
                // A complete final frame can be torn after its CRC was
                // written. It is safe to discard only when no later data
                // follows; any earlier mismatch corrupts the board.
                if (has_header && input.peek() == std::char_traits<char>::eof()) {
                    recovered_final_line = true;
                    break;
                }
                storage_error(error.what());
            } catch (const std::exception& error) {
                storage_error(error.what());
            }
            if (result == SawerRecordReadResult::end_of_file) {
                break;
            }
            if (result == SawerRecordReadResult::truncated) {
                recovered_final_line = true;
                break;
            }
            try {
                if (!has_header) {
                    if (record.kind != SawerRecordKind::file_header
                        || record.sequence != 0U || !record.payload.empty()) {
                        storage_error("first frame is not the file header");
                    }
                    validate_header(record.metadata, board_id);
                    has_header = true;
                    continue;
                }
                if (record.kind == SawerRecordKind::asset) {
                    if (record.sequence != 0U) {
                        storage_error("asset record has a non-zero sequence");
                    }
                    replay_asset(record, assets);
                    continue;
                }
                if (record.kind != SawerRecordKind::operation
                    || !record.payload.empty()) {
                    storage_error("unexpected non-operation frame");
                }
                if (record.sequence != next_sequence(sequence)) {
                    storage_error("operation sequence is not contiguous");
                }
                apply_operation(loaded, record.metadata, &assets);
                sequence = record.sequence;
            } catch (const std::exception& error) {
                storage_error(error.what());
            }
        }
        if (!has_header) {
            storage_error("missing header");
        }
        input.close();
        FileStamp replayed_stamp = capture_stamp(path);
        if (replayed_stamp != initial_stamp) {
            storage_error("board changed while it was being opened");
        }
        if (recovered_final_line) {
            sequence = write_snapshot(path, board_id, loaded);
            replayed_stamp = capture_stamp(path);
        }
        document = std::move(loaded);
        return BoardFileSession{
            path, board_id, sequence, replayed_stamp,
            referenced_assets(document)};
    }

    Document loaded;
    ObjectId board_id;
    std::uint64_t sequence = 0U;
    std::size_t line_number = 0U;
    bool has_header = false;
    recovered_final_line = false;
    std::string line;

    for (;;) {
        LineState state;
        try {
            state = read_bounded_line(input, line, maximum_record_bytes);
        } catch (const std::exception& error) {
            storage_error(
                "line " + std::to_string(line_number + 1U)
                + ": " + error.what());
        }
        if (state == LineState::end_of_file) {
            break;
        }
        ++line_number;
        try {
            const Json record = Json::parse(line);
            if (!has_header) {
                validate_header(record, board_id);
                has_header = true;
                continue;
            }

            const auto record_sequence = record.at("seq").get<std::uint64_t>();
            if (record_sequence != next_sequence(sequence)) {
                storage_error("operation sequence is not contiguous");
            }

            const ObjectId id = parse_id(record.at("id"));
            const std::string operation = record.at("op").get<std::string>();
            if (operation == "put") {
                Object object = parse_object(id, record.at("object"));
                if (loaded.find(id) != nullptr) {
                    if (!loaded.replace(std::move(object))) {
                        storage_error("cannot replace replayed object");
                    }
                } else if (!loaded.insert(std::move(object))) {
                    storage_error("cannot insert replayed object");
                }
            } else if (operation == "delete") {
                if (!loaded.remove(id).has_value()) {
                    storage_error("delete operation references a missing object");
                }
            } else if (operation == "move") {
                if (!loaded.translate(
                        id, parse_delta(record.at("delta")))) {
                    storage_error("move operation references a missing object");
                }
            } else if (operation == "style") {
                if (!loaded.exchange_style(
                        id, parse_style(record.at("style"))).has_value()) {
                    storage_error("style operation references a missing object");
                }
            } else {
                storage_error("unsupported operation: " + operation);
            }
            sequence = record_sequence;
        } catch (const std::exception& error) {
            if (has_header && state == LineState::unterminated) {
                recovered_final_line = true;
                break;
            }
            storage_error(
                "line " + std::to_string(line_number) + ": " + error.what());
        }
    }

    if (!has_header) {
        storage_error("missing header");
    }
    input.close();
    FileStamp replayed_stamp = capture_stamp(path);
    if (replayed_stamp != initial_stamp) {
        storage_error("board changed while it was being opened");
    }
    if (recovered_final_line) {
        sequence = write_snapshot(path, board_id, loaded);
        replayed_stamp = capture_stamp(path);
    }

    document = std::move(loaded);
    return BoardFileSession{
        path, board_id, sequence, replayed_stamp,
        referenced_assets(document), true};
}

BoardFileSession::BoardFileSession(
    std::filesystem::path path,
    const ObjectId board_id,
    const std::uint64_t sequence,
    const FileStamp stamp,
    std::set<AssetId> persisted_assets,
    const bool needs_conversion)
    : path_{std::move(path)}
    , board_id_{board_id}
    , sequence_{sequence}
    , expected_stamp_{stamp}
    , persisted_assets_{std::move(persisted_assets)}
    , needs_conversion_{needs_conversion}
{
}

void BoardFileSession::queue_put(const Object& object)
{
    const std::uint64_t sequence = next_sequence(sequence_);
    pending_operations_.push_back({
        .sequence = sequence,
        .type = PendingOperation::Type::put,
        .object = object,
        .style = std::nullopt,
        .delta = {},
        .id = object.id,
    });
    sequence_ = sequence;
}

void BoardFileSession::queue_delete(const ObjectId id)
{
    const std::uint64_t sequence = next_sequence(sequence_);
    pending_operations_.push_back({
        .sequence = sequence,
        .type = PendingOperation::Type::delete_object,
        .object = std::nullopt,
        .style = std::nullopt,
        .delta = {},
        .id = id,
    });
    sequence_ = sequence;
}

void BoardFileSession::queue_move(const ObjectId id, const Vec2d delta)
{
    if (delta == Vec2d{}) {
        return;
    }
    const std::uint64_t sequence = next_sequence(sequence_);
    pending_operations_.push_back({
        .sequence = sequence,
        .type = PendingOperation::Type::move,
        .object = std::nullopt,
        .style = std::nullopt,
        .delta = delta,
        .id = id,
    });
    sequence_ = sequence;
}

void BoardFileSession::queue_style(
    const ObjectId id,
    const Style& style)
{
    const std::uint64_t sequence = next_sequence(sequence_);
    pending_operations_.push_back({
        .sequence = sequence,
        .type = PendingOperation::Type::style,
        .object = std::nullopt,
        .style = style,
        .delta = {},
        .id = id,
    });
    sequence_ = sequence;
}

void BoardFileSession::flush(const Document& document)
{
    try {
        finish_background_flush(true);
    } catch (...) {
        if (!append_state_uncertain_) {
            throw;
        }
    }
    if (append_state_uncertain_) {
        create_conflict_copy(document);
        return;
    }
    if (needs_conversion_) {
        if (externally_modified()) {
            create_conflict_copy(document);
            return;
        }
        sequence_ = write_snapshot(path_, board_id_, document);
        pending_operations_.clear();
        expected_stamp_ = capture_stamp(path_);
        needs_conversion_ = false;
        reset_persisted_assets(document);
        last_flush_ = std::chrono::steady_clock::now();
        return;
    }
    const auto needs_compaction = [&]() {
        const auto object_count = static_cast<std::uint64_t>(document.size());
        if (sequence_ <= object_count) return false;
        const std::uint64_t excess = sequence_ - object_count;
        return excess > maximum_excess_operations
            || (expected_stamp_.size > compaction_byte_pressure
                && excess > byte_pressure_excess_operations);
    };
    if (pending_operations_.empty()) {
        if (needs_compaction()) {
            compact(document);
        }
        return;
    }
    if (externally_modified()) {
        create_conflict_copy(document);
        return;
    }

    try {
        expected_stamp_ =
            append_operations(path_, pending_operations_, expected_stamp_, persisted_assets_);
        remember_assets(pending_operations_);
    } catch (...) {
        append_state_uncertain_ = true;
        last_flush_ = std::chrono::steady_clock::now();
        throw;
    }
    pending_operations_.clear();
    last_flush_ = std::chrono::steady_clock::now();
    if (needs_compaction()) {
        compact(document);
    }
}

void BoardFileSession::flush_if_due(
    const Document& document,
    const std::chrono::milliseconds interval)
{
    finish_background_flush(false);
    if (background_flush_.valid()
        || (pending_operations_.empty() && !append_state_uncertain_)
        || std::chrono::steady_clock::now() - last_flush_ < interval) {
        return;
    }

    try {
        if (append_state_uncertain_ || externally_modified()) {
            create_conflict_copy(document);
        } else if (needs_conversion_) {
            compact(document);
        } else if (!pending_operations_.empty()) {
            start_background_flush();
        }
    } catch (...) {
        last_flush_ = std::chrono::steady_clock::now();
        throw;
    }
}

void BoardFileSession::compact(const Document& document)
{
    try {
        finish_background_flush(true);
    } catch (...) {
        if (!append_state_uncertain_) {
            throw;
        }
    }
    if (append_state_uncertain_) {
        create_conflict_copy(document);
        return;
    }
    if (externally_modified()) {
        create_conflict_copy(document);
        return;
    }

    sequence_ = write_snapshot(path_, board_id_, document);
    pending_operations_.clear();
    expected_stamp_ = capture_stamp(path_);
    needs_conversion_ = false;
    reset_persisted_assets(document);
    last_flush_ = std::chrono::steady_clock::now();
}

void BoardFileSession::rename(const std::filesystem::path& new_path)
{
    finish_background_flush(true);
    if (append_state_uncertain_) {
        storage_error("cannot rename after an uncertain append");
    }
    if (!pending_operations_.empty()) {
        try {
            expected_stamp_ =
                append_operations(path_, pending_operations_, expected_stamp_, persisted_assets_);
            remember_assets(pending_operations_);
        } catch (...) {
            append_state_uncertain_ = true;
            last_flush_ = std::chrono::steady_clock::now();
            throw;
        }
        pending_operations_.clear();
    }

    std::error_code error;
    std::filesystem::rename(path_, new_path, error);
    if (error) {
        storage_error("could not rename board to " + new_path.string());
    }
    path_ = new_path;
    expected_stamp_ = capture_stamp(path_);
    last_flush_ = std::chrono::steady_clock::now();
}

const std::filesystem::path& BoardFileSession::path() const noexcept
{
    return path_;
}

ObjectId BoardFileSession::board_id() const noexcept
{
    return board_id_;
}

std::uint64_t BoardFileSession::sequence() const noexcept
{
    return sequence_;
}

bool BoardFileSession::conflict_created() const noexcept
{
    return conflict_created_;
}

bool BoardFileSession::has_pending_operations() const noexcept
{
    return !pending_operations_.empty() || background_flush_.valid()
        || in_flight_operations_ || append_state_uncertain_;
}

void BoardFileSession::finish_background_flush(const bool wait)
{
    if (!background_flush_.valid()) {
        return;
    }
    if (!wait
        && background_flush_.wait_for(std::chrono::milliseconds{0})
            != std::future_status::ready) {
        return;
    }

    try {
        expected_stamp_ = background_flush_.get();
        remember_assets(*in_flight_operations_);
        in_flight_operations_.reset();
        last_flush_ = std::chrono::steady_clock::now();
    } catch (...) {
        // The current document remains authoritative. Keep this batch alive
        // until flush() writes a complete conflict snapshot.
        append_state_uncertain_ = true;
        last_flush_ = std::chrono::steady_clock::now();
        throw;
    }
}

void BoardFileSession::start_background_flush()
{
    auto operations =
        std::make_shared<std::vector<PendingOperation>>();
    operations->swap(pending_operations_);
    const auto path = path_;
    const FileStamp expected_stamp = expected_stamp_;
    try {
        auto background = std::async(
            std::launch::async,
            [path, operations, expected_stamp, persisted_assets = persisted_assets_]() {
                return append_operations(
                    path, *operations, expected_stamp, std::move(persisted_assets));
            });
        in_flight_operations_ = std::move(operations);
        background_flush_ = std::move(background);
    } catch (...) {
        pending_operations_.swap(*operations);
        throw;
    }
}

bool BoardFileSession::externally_modified() const
{
    std::error_code error;
    if (!std::filesystem::exists(path_, error) || error) {
        return true;
    }
    return capture_stamp(path_) != expected_stamp_;
}

void BoardFileSession::create_conflict_copy(const Document& document)
{
    const ObjectId conflict_id = ObjectId::random();
    const std::string suffix = conflict_id.to_string().substr(0U, 8U);
    const auto conflict_path = path_.parent_path()
        / (path_.stem().string() + " (conflict-" + suffix + ")"
           + path_.extension().string());

    const std::uint64_t conflict_sequence =
        write_snapshot(conflict_path, conflict_id, document);
    path_ = conflict_path;
    board_id_ = conflict_id;
    sequence_ = conflict_sequence;
    pending_operations_.clear();
    in_flight_operations_.reset();
    expected_stamp_ = capture_stamp(path_);
    conflict_created_ = true;
    append_state_uncertain_ = false;
    reset_persisted_assets(document);
    last_flush_ = std::chrono::steady_clock::now();
}

BoardFileSession::FileStamp BoardFileSession::capture_stamp(
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        storage_error("cannot inspect file size");
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) {
        storage_error("cannot inspect modification time");
    }
    return {size, modified};
}

BoardFileSession::FileStamp BoardFileSession::append_operations(
    const std::filesystem::path& path,
    const std::vector<PendingOperation>& operations,
    const FileStamp expected_stamp,
    std::set<AssetId> persisted_assets)
{
    if (capture_stamp(path) != expected_stamp) {
        storage_error("board changed before append");
    }
    std::ofstream output{path, std::ios::binary | std::ios::app};
    if (!output) {
        storage_error("cannot append to " + path.string());
    }
    for (const auto& operation : operations) {
        Json metadata;
        if (operation.type == PendingOperation::Type::put) {
            if (const auto* const image =
                    std::get_if<Image>(&operation.object->geometry)) {
                if (persisted_assets.insert(image->asset->id).second) {
                    write_asset_record(output, *image->asset);
                }
            }
            metadata = make_put(operation.sequence, *operation.object);
        } else if (
            operation.type == PendingOperation::Type::delete_object) {
            metadata = make_delete(operation.sequence, operation.id);
        } else if (operation.type == PendingOperation::Type::move) {
            metadata = make_move(
                operation.sequence, operation.id, operation.delta);
        } else {
            metadata = make_style(
                operation.sequence, operation.id, *operation.style);
        }
        metadata.erase("seq");
        metadata["id"] = serialize_binary_id(operation.id);
        write_sawer_record(output, {
            .kind = SawerRecordKind::operation,
            .sequence = operation.sequence,
            .metadata = std::move(metadata),
            .payload = {},
        });
    }
    output.flush();
    if (!output) {
        storage_error("cannot flush appended operations");
    }
    output.close();
    sync_file(path);
    return capture_stamp(path);
}

void BoardFileSession::remember_assets(
    const std::vector<PendingOperation>& operations)
{
    for (const PendingOperation& operation : operations) {
        if (operation.type != PendingOperation::Type::put) continue;
        if (const auto* const image =
                std::get_if<Image>(&operation.object->geometry)) {
            persisted_assets_.insert(image->asset->id);
        }
    }
}

void BoardFileSession::reset_persisted_assets(const Document& document)
{
    persisted_assets_ = referenced_assets(document);
}

std::uint64_t BoardFileSession::write_snapshot(
    const std::filesystem::path& path,
    const ObjectId board_id,
    const Document& document)
{
    const auto temporary = temporary_path_for(path);
    std::uint64_t sequence = 0U;

    try {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            storage_error("cannot create temporary board file");
        }
        write_sawer_file_prologue(output);
        write_sawer_record(output, {
            .kind = SawerRecordKind::file_header,
            .sequence = 0U,
            .metadata = make_header(board_id),
            .payload = {},
        });
        AssetTable assets;
        for (const Object* const object : document.all_objects()) {
            if (const auto* const image = std::get_if<Image>(&object->geometry)) {
                assets.emplace(image->asset->id, image->asset);
            }
        }
        for (const auto& [asset_id, asset] : assets) {
            static_cast<void>(asset_id);
            write_asset_record(output, *asset);
        }
        for (const Object* const object : document.all_objects()) {
            sequence = next_sequence(sequence);
            Json metadata = make_put(sequence, *object);
            metadata.erase("seq");
            metadata["id"] = serialize_binary_id(object->id);
            write_sawer_record(output, {
                .kind = SawerRecordKind::operation,
                .sequence = sequence,
                .metadata = std::move(metadata),
                .payload = {},
            });
        }
        output.flush();
        if (!output) {
            storage_error("cannot write temporary board file");
        }
        output.close();
        sync_file(temporary);
        atomic_replace(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }

    return sequence;
}

} // namespace sawer
