#include "storage/ClipboardFragment.hpp"

#include "image/ImageCodec.hpp"
#include "image/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace sawer {
namespace {

using Json = nlohmann::json;
constexpr std::array<std::uint8_t, 8U> prologue{
    'S', 'A', 'W', 'E', 'R', 'C', 'L', 'P'};
constexpr std::uint64_t fragment_version = 1U;
constexpr std::size_t maximum_fragment_bytes = 384U * 1024U * 1024U;
constexpr std::size_t maximum_fragment_objects = 100'000U;
constexpr std::size_t maximum_fragment_assets = 4'096U;
constexpr std::size_t maximum_fragment_stroke_points = 1'000'000U;
constexpr double board_half_extent = 1'000'000.0;

[[noreturn]] void fragment_error(const std::string& message)
{
    throw std::runtime_error{"Sawer clipboard fragment: " + message};
}

Json binary_asset_id(const AssetId& id)
{
    return Json::binary(std::vector<std::uint8_t>{id.begin(), id.end()});
}

AssetId parse_asset_id(const Json& value)
{
    if (!value.is_binary() || value.get_binary().size() != AssetId{}.size()) {
        fragment_error("invalid asset identifier");
    }
    AssetId result{};
    std::copy(value.get_binary().begin(), value.get_binary().end(), result.begin());
    return result;
}

Json serialize_style(const Style& style)
{
    const auto color = [](const Color value) {
        return Json{
            static_cast<unsigned int>(value.red),
            static_cast<unsigned int>(value.green),
            static_cast<unsigned int>(value.blue),
            static_cast<unsigned int>(value.alpha),
        };
    };
    return {
        {"stroke", color(style.stroke)},
        {"fill", style.fill ? color(*style.fill) : Json(nullptr)},
        {"width", style.stroke_width},
    };
}

Json serialize_object(const Object& object)
{
    Json value{{"z", object.z_order}};
    if (const auto* line = std::get_if<Line>(&object.geometry)) {
        value.update({{"type", "line"}, {"start", {line->start.x, line->start.y}},
            {"end", {line->end.x, line->end.y}}, {"style", serialize_style(object.style)}});
    } else if (const auto* stroke = std::get_if<Stroke>(&object.geometry)) {
        Json points = Json::array();
        for (const Vec2d point : stroke->points) points.push_back({point.x, point.y});
        value.update({{"type", "stroke"}, {"points", std::move(points)},
            {"style", serialize_style(object.style)}});
    } else if (const auto* rectangle = std::get_if<RectangleShape>(&object.geometry)) {
        value.update({{"type", "rectangle"},
            {"first", {rectangle->first.x, rectangle->first.y}},
            {"second", {rectangle->second.x, rectangle->second.y}},
            {"roundness", rectangle->roundness}, {"style", serialize_style(object.style)}});
    } else if (const auto* ellipse = std::get_if<Ellipse>(&object.geometry)) {
        value.update({{"type", "ellipse"}, {"first", {ellipse->first.x, ellipse->first.y}},
            {"second", {ellipse->second.x, ellipse->second.y}},
            {"style", serialize_style(object.style)}});
    } else if (const auto* image = std::get_if<Image>(&object.geometry)) {
        if (!image->asset) fragment_error("image has no asset");
        value.update({{"type", "image"}, {"asset", binary_asset_id(image->asset->id)},
            {"first", {image->first.x, image->first.y}},
            {"second", {image->second.x, image->second.y}}});
    } else {
        fragment_error("unsupported object type");
    }
    return value;
}

double finite_number(const Json& value)
{
    if (!value.is_number()) fragment_error("expected a number");
    const double result = value.get<double>();
    if (!std::isfinite(result)) fragment_error("non-finite number");
    return result;
}

Vec2d parse_point(const Json& value)
{
    if (!value.is_array() || value.size() != 2U) fragment_error("invalid point");
    const Vec2d point{finite_number(value[0]), finite_number(value[1])};
    if (std::abs(point.x) > board_half_extent || std::abs(point.y) > board_half_extent) {
        fragment_error("point is outside board bounds");
    }
    return point;
}

Color parse_color(const Json& value)
{
    if (!value.is_array() || value.size() != 4U) {
        fragment_error("invalid color collection");
    }
    std::array<std::uint8_t, 4U> channels{};
    for (std::size_t index = 0U; index < channels.size(); ++index) {
        if (!value[index].is_number_integer()
            && !value[index].is_number_unsigned()) {
            fragment_error("invalid color channel type");
        }
        const int channel = value[index].get<int>();
        if (channel < 0 || channel > 255) fragment_error("color channel is outside 0..255");
        channels[index] = static_cast<std::uint8_t>(channel);
    }
    return {channels[0], channels[1], channels[2], channels[3]};
}

Style parse_style(const Json& value)
{
    if (!value.is_object()) fragment_error("invalid style");
    Style result;
    result.stroke = parse_color(value.at("stroke"));
    if (!value.at("fill").is_null()) result.fill = parse_color(value.at("fill"));
    result.stroke_width = finite_number(value.at("width"));
    if (result.stroke_width <= 0.0 || result.stroke_width > 10'000.0) {
        fragment_error("invalid stroke width");
    }
    return result;
}

Object parse_object(
    const Json& value,
    const std::map<AssetId, std::shared_ptr<const ImageAsset>>& assets,
    std::size_t& stroke_points)
{
    if (!value.is_object()) fragment_error("invalid object");
    const std::string type = value.at("type").get<std::string>();
    const auto z = value.at("z").get<std::int64_t>();
    if (z == std::numeric_limits<std::int64_t>::max()) fragment_error("invalid z-order");
    const ObjectId id = ObjectId::random();
    if (type == "image") {
        const auto found = assets.find(parse_asset_id(value.at("asset")));
        if (found == assets.end()) fragment_error("image references a missing asset");
        return Object::make_image(id, z, {found->second,
            parse_point(value.at("first")), parse_point(value.at("second"))});
    }
    const Style style = parse_style(value.at("style"));
    if (type == "line") {
        return Object::make_line(id, z,
            {parse_point(value.at("start")), parse_point(value.at("end"))}, style);
    }
    if (type == "stroke") {
        const Json& values = value.at("points");
        if (!values.is_array() || values.empty()
            || values.size() > maximum_fragment_stroke_points - stroke_points) {
            fragment_error("invalid or excessive stroke points");
        }
        Stroke stroke;
        stroke.points.reserve(values.size());
        for (const Json& point : values) stroke.points.push_back(parse_point(point));
        stroke_points += stroke.points.size();
        return Object::make_stroke(id, z, std::move(stroke), style);
    }
    if (type == "rectangle") {
        const double roundness = finite_number(value.at("roundness"));
        if (roundness < 0.0 || roundness > 0.5) fragment_error("invalid roundness");
        return Object::make_rectangle(id, z,
            {parse_point(value.at("first")), parse_point(value.at("second")), roundness}, style);
    }
    if (type == "ellipse") {
        return Object::make_ellipse(id, z,
            {parse_point(value.at("first")), parse_point(value.at("second"))}, style);
    }
    fragment_error("unsupported object type");
}

} // namespace

std::vector<std::uint8_t> serialize_clipboard_fragment(
    const Document& document,
    const std::span<const ObjectId> ids)
{
    if (ids.empty() || ids.size() > maximum_fragment_objects) {
        fragment_error("invalid object count");
    }
    std::vector<const Object*> objects;
    objects.reserve(ids.size());
    std::map<AssetId, std::shared_ptr<const ImageAsset>> assets;
    for (const ObjectId id : ids) {
        const Object* object = document.find(id);
        if (!object) fragment_error("selection references a missing object");
        objects.push_back(object);
        if (const auto* image = std::get_if<Image>(&object->geometry)) {
            if (!image->asset) fragment_error("image has no asset");
            assets.emplace(image->asset->id, image->asset);
        }
    }
    std::ranges::sort(objects, [](const Object* a, const Object* b) {
        return a->z_order < b->z_order || (a->z_order == b->z_order && a->id < b->id);
    });
    Json root{{"format", "sawer-clipboard"}, {"version", fragment_version},
        {"assets", Json::array()}, {"objects", Json::array()}};
    for (const auto& [id, asset] : assets) {
        if (!asset || asset->id != id || asset->id != sha256(asset->png)) {
            fragment_error("invalid image asset");
        }
        root["assets"].push_back({{"id", binary_asset_id(id)},
            {"width", asset->pixel_width}, {"height", asset->pixel_height},
            {"png", Json::binary(asset->png)}, {"preview", Json::binary(asset->preview)}});
    }
    for (const Object* object : objects) root["objects"].push_back(serialize_object(*object));
    std::vector<std::uint8_t> cbor = Json::to_cbor(root);
    if (cbor.size() > maximum_fragment_bytes - prologue.size()) {
        fragment_error("fragment exceeds byte limit");
    }
    std::vector<std::uint8_t> result;
    result.reserve(prologue.size() + cbor.size());
    result.insert(result.end(), prologue.begin(), prologue.end());
    result.insert(result.end(), cbor.begin(), cbor.end());
    return result;
}

ClipboardFragment deserialize_clipboard_fragment(
    const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() <= prologue.size() || bytes.size() > maximum_fragment_bytes
        || !std::equal(prologue.begin(), prologue.end(), bytes.begin())) {
        fragment_error("invalid prologue or size");
    }
    Json root;
    try {
        root = Json::from_cbor(bytes.subspan(prologue.size()), true, true);
    } catch (const std::exception& error) {
        fragment_error(std::string{"invalid CBOR: "} + error.what());
    }
    if (!root.is_object() || root.at("format") != "sawer-clipboard"
        || root.at("version") != fragment_version) {
        fragment_error("unsupported format or version");
    }
    const Json& asset_values = root.at("assets");
    const Json& object_values = root.at("objects");
    if (!asset_values.is_array() || asset_values.size() > maximum_fragment_assets
        || !object_values.is_array() || object_values.empty()
        || object_values.size() > maximum_fragment_objects) {
        fragment_error("invalid collection sizes");
    }
    std::map<AssetId, std::shared_ptr<const ImageAsset>> assets;
    for (const Json& value : asset_values) {
        if (!value.is_object() || !value.at("png").is_binary()
            || !value.at("preview").is_binary()) fragment_error("invalid asset");
        auto asset = std::make_shared<ImageAsset>();
        asset->id = parse_asset_id(value.at("id"));
        asset->pixel_width = value.at("width").get<std::uint32_t>();
        asset->pixel_height = value.at("height").get<std::uint32_t>();
        asset->png = value.at("png").get_binary();
        asset->preview = value.at("preview").get_binary();
        if (asset->pixel_width == 0U || asset->pixel_height == 0U
            || asset->id != sha256(asset->png)) fragment_error("invalid asset digest");
        const DecodedImage decoded = decode_image_rgba(asset->png);
        if (decoded.width != asset->pixel_width || decoded.height != asset->pixel_height
            || encode_png_rgba(decoded) != asset->png) fragment_error("non-canonical image asset");
        if (!asset->preview.empty()) {
            const DecodedImage preview = decode_image_rgba(asset->preview);
            if (preview.width > 256U || preview.height > 256U) fragment_error("oversized preview");
        }
        const auto [found, inserted] = assets.emplace(asset->id, asset);
        if (!inserted && found->second->png != asset->png) fragment_error("conflicting asset");
    }
    ClipboardFragment result;
    result.objects.reserve(object_values.size());
    std::size_t stroke_points{};
    for (const Json& value : object_values) {
        Object object = parse_object(value, assets, stroke_points);
        if (!object.within_board_bounds()) fragment_error("object is outside board bounds");
        result.objects.push_back(std::move(object));
    }
    return result;
}

} // namespace sawer
