#include "storage/BoardFile.hpp"
#include "storage/RecentFiles.hpp"
#include "storage/SawerRecord.hpp"

#include "canvas/Selection.hpp"
#include "document/Document.hpp"
#include "document/Object.hpp"
#include "document/ObjectId.hpp"
#include "image/ImageCodec.hpp"
#include "image/Sha256.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryBoard final {
public:
    TemporaryBoard()
        : path_{std::filesystem::temp_directory_path()
            / ("sawer-test-" + sawer::ObjectId::random().to_string()
               + ".sawer")}
    {
    }

    ~TemporaryBoard()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        if (!secondary_.empty()) {
            std::filesystem::remove(secondary_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    void track(std::filesystem::path path)
    {
        secondary_ = std::move(path);
    }

private:
    std::filesystem::path path_;
    std::filesystem::path secondary_;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path()
            / ("sawer-directory-test-"
               + sawer::ObjectId::random().to_string())}
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

sawer::Object test_line(const std::uint64_t id)
{
    return sawer::Object::make_line(
        sawer::ObjectId::from_u64(id),
        3,
        {{-12.5, 8.25}, {99.0, -42.0}},
        {
            .stroke = {12U, 34U, 56U, 200U},
            .fill = std::nullopt,
            .stroke_width = 7.5,
        });
}

TEST_CASE("object identifier text round trips")
{
    const auto id = sawer::ObjectId::random();
    const auto parsed = sawer::ObjectId::parse(id.to_string());

    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == id);
    REQUIRE_FALSE(sawer::ObjectId::parse("not-a-uuid").has_value());
}

TEST_CASE("portable board snapshot round trips")
{
    TemporaryBoard temporary;
    sawer::Document original;
    REQUIRE(original.insert(test_line(1U)));

    auto session = sawer::BoardFileSession::create(temporary.path(), original);
    REQUIRE(session.sequence() == 1U);

    sawer::Document loaded;
    bool recovered = false;
    auto reopened =
        sawer::BoardFileSession::open(temporary.path(), loaded, recovered);

    REQUIRE_FALSE(recovered);
    REQUIRE(loaded.size() == 1U);
    const auto* const object =
        loaded.find(sawer::ObjectId::from_u64(1U));
    REQUIRE(object != nullptr);
    REQUIRE(object->style.stroke_width == 7.5);
    REQUIRE(std::get<sawer::Line>(object->geometry).end.y == -42.0);
    REQUIRE(reopened.sequence() == 1U);
}

TEST_CASE("new boards use the framed CBOR v1 prologue")
{
    TemporaryBoard temporary;
    sawer::Document document;
    REQUIRE(document.insert(test_line(71U)));
    static_cast<void>(sawer::BoardFileSession::create(temporary.path(), document));

    std::ifstream input{temporary.path(), std::ios::binary};
    std::array<std::uint8_t, sawer::sawer_file_prologue_size> prologue{};
    input.read(reinterpret_cast<char*>(prologue.data()),
        static_cast<std::streamsize>(prologue.size()));
    REQUIRE(prologue == std::array<std::uint8_t, 8U>{
        0x53U, 0x41U, 0x57U, 0x45U, 0x52U, 0x00U, 0x01U, 0x00U});

    sawer::SawerRecord header;
    REQUIRE(sawer::read_sawer_record(input, header, 1024U, 0U)
            == sawer::SawerRecordReadResult::record);
    REQUIRE(header.kind == sawer::SawerRecordKind::file_header);
    REQUIRE(header.sequence == 0U);
    REQUIRE(header.metadata.at("board_id").is_binary());
}

TEST_CASE("image assets are persisted before their placements")
{
    TemporaryBoard temporary;
    auto asset = std::make_shared<sawer::ImageAsset>();
    const sawer::DecodedImage pixels{
        2U, 1U, {10U, 20U, 30U, 255U, 40U, 50U, 60U, 128U}};
    asset->pixel_width = pixels.width;
    asset->pixel_height = pixels.height;
    asset->png = sawer::encode_png_rgba(pixels);
    asset->id = sawer::sha256(asset->png);
    asset->preview = sawer::encode_png_rgba(pixels);
    sawer::Document document;
    const auto id = sawer::ObjectId::from_u64(76U);
    REQUIRE(document.insert(sawer::Object::make_image(
        id, 0, {asset, {10.0, 20.0}, {12.0, 21.0}})));
    static_cast<void>(sawer::BoardFileSession::create(temporary.path(), document));

    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered));
    const auto* const image = std::get_if<sawer::Image>(
        &loaded.find(id)->geometry);
    REQUIRE(image != nullptr);
    REQUIRE(image->asset->id == asset->id);
    REQUIRE(image->asset->png == asset->png);
    REQUIRE(image->asset->preview == asset->preview);
}

TEST_CASE("append operations replay puts and deletes")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto session = sawer::BoardFileSession::create(temporary.path(), document);

    auto object = test_line(2U);
    REQUIRE(document.insert(object));
    session.queue_put(object);
    session.flush(document);

    REQUIRE(document.remove(object.id).has_value());
    session.queue_delete(object.id);
    session.flush(document);

    sawer::Document loaded;
    bool recovered = false;
    auto reopened =
        sawer::BoardFileSession::open(temporary.path(), loaded, recovered);
    REQUIRE(loaded.size() == 0U);
    REQUIRE(reopened.sequence() == 2U);
}

TEST_CASE("moves and styles append compact deltas for large strokes")
{
    TemporaryBoard temporary;
    sawer::Stroke stroke;
    stroke.points.reserve(10'000U);
    for (std::size_t index = 0U; index < 10'000U; ++index) {
        stroke.points.push_back({
            static_cast<double>(index),
            static_cast<double>(index % 23U),
        });
    }
    const auto id = sawer::ObjectId::from_u64(22U);
    sawer::Document document;
    REQUIRE(document.insert(sawer::Object::make_stroke(
        id, 0, std::move(stroke))));
    auto session =
        sawer::BoardFileSession::create(temporary.path(), document);
    const auto snapshot_size = std::filesystem::file_size(temporary.path());

    const sawer::Vec2d delta{125.5, -70.25};
    REQUIRE(document.translate(id, delta));
    session.queue_move(id, delta);
    sawer::Style changed = document.find(id)->style;
    changed.stroke = {18U, 91U, 220U, 255U};
    changed.stroke_width = 9.5;
    REQUIRE(document.exchange_style(id, changed).has_value());
    session.queue_style(id, changed);
    session.flush(document);

    const auto appended_size = std::filesystem::file_size(temporary.path());
    REQUIRE(appended_size - snapshot_size < 1'024U);

    sawer::Document loaded;
    bool recovered = false;
    auto reopened = sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered);
    REQUIRE_FALSE(recovered);
    REQUIRE(reopened.sequence() == 3U);
    const auto* const restored = loaded.find(id);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->style == changed);
    REQUIRE(std::get<sawer::Stroke>(restored->geometry).points.front()
            == sawer::Vec2d{125.5, -70.25});
    REQUIRE(std::get<sawer::Stroke>(restored->geometry).points.back()
            == sawer::Vec2d{10'124.5, -53.25});
}

TEST_CASE("explicit flush compacts an excessively long operation log")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto object = test_line(23U);
    const auto id = object.id;
    REQUIRE(document.insert(std::move(object)));
    auto session =
        sawer::BoardFileSession::create(temporary.path(), document);

    for (std::size_t index = 0U; index < 4'097U; ++index) {
        const sawer::Vec2d delta{1.0, 0.0};
        REQUIRE(document.translate(id, delta));
        session.queue_move(id, delta);
    }
    session.flush(document);
    REQUIRE(session.sequence() == 1U);

    sawer::Document loaded;
    bool recovered = false;
    auto reopened = sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered);
    REQUIRE_FALSE(recovered);
    REQUIRE(reopened.sequence() == 1U);
    REQUIRE(std::get<sawer::Line>(loaded.find(id)->geometry).start.x
            == -12.5 + 4'097.0);
}

TEST_CASE("all mouse object variants and fills round trip")
{
    TemporaryBoard temporary;
    sawer::Document document;
    const sawer::Style filled{
        .stroke = {200U, 210U, 220U, 255U},
        .fill = sawer::Color{20U, 30U, 40U, 128U},
        .stroke_width = 5.0,
    };

    REQUIRE(document.insert(sawer::Object::make_stroke(
        sawer::ObjectId::from_u64(30U),
        0,
        {{{0.0, 0.0}, {10.0, 15.0}, {25.0, 5.0}}},
        filled)));
    REQUIRE(document.insert(sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(31U),
        1,
        {{-20.0, -10.0}, {20.0, 10.0}},
        filled)));
    REQUIRE(document.insert(sawer::Object::make_ellipse(
        sawer::ObjectId::from_u64(32U),
        2,
        {{-30.0, -15.0}, {30.0, 15.0}},
        filled)));

    static_cast<void>(
        sawer::BoardFileSession::create(temporary.path(), document));

    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(
        sawer::BoardFileSession::open(
            temporary.path(), loaded, recovered));

    REQUIRE(loaded.size() == 3U);
    REQUIRE(std::holds_alternative<sawer::Stroke>(
        loaded.find(sawer::ObjectId::from_u64(30U))->geometry));
    REQUIRE(std::holds_alternative<sawer::RectangleShape>(
        loaded.find(sawer::ObjectId::from_u64(31U))->geometry));
    const auto* const ellipse =
        loaded.find(sawer::ObjectId::from_u64(32U));
    REQUIRE(std::holds_alternative<sawer::Ellipse>(ellipse->geometry));
    REQUIRE(ellipse->style.fill.has_value());
    REQUIRE(ellipse->style.fill->alpha == 128U);
}

TEST_CASE("an interrupted final record preserves earlier data")
{
    TemporaryBoard temporary;
    sawer::Document document;
    REQUIRE(document.insert(test_line(3U)));
    static_cast<void>(
        sawer::BoardFileSession::create(temporary.path(), document));

    {
        std::ofstream output{
            temporary.path(), std::ios::binary | std::ios::app};
        output.write("SW", 2);
    }

    sawer::Document loaded;
    bool recovered = false;
    auto reopened =
        sawer::BoardFileSession::open(temporary.path(), loaded, recovered);
    REQUIRE(recovered);
    REQUIRE(loaded.size() == 1U);
    REQUIRE(reopened.sequence() == 1U);
}

TEST_CASE("a bad CRC on the final frame recovers through the prior frame")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto session = sawer::BoardFileSession::create(temporary.path(), document);
    auto object = test_line(72U);
    REQUIRE(document.insert(object));
    session.queue_put(object);
    session.flush(document);

    const auto size = std::filesystem::file_size(temporary.path());
    {
        std::fstream file{temporary.path(), std::ios::binary | std::ios::in | std::ios::out};
        REQUIRE(file);
        file.seekg(static_cast<std::streamoff>(size - 1U));
        char byte{};
        file.read(&byte, 1);
        file.clear();
        file.seekp(static_cast<std::streamoff>(size - 1U));
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        file.write(&byte, 1);
    }

    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered));
    REQUIRE(recovered);
    REQUIRE(loaded.size() == 0U);
}

TEST_CASE("a bad CRC before a later frame rejects the board")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto first = test_line(73U);
    REQUIRE(document.insert(first));
    auto session = sawer::BoardFileSession::create(temporary.path(), document);
    auto second = test_line(74U);
    REQUIRE(document.insert(second));
    session.queue_put(second);
    session.flush(document);

    std::streampos first_operation;
    {
        std::ifstream input{temporary.path(), std::ios::binary};
        sawer::validate_sawer_file_prologue(input);
        sawer::SawerRecord header;
        REQUIRE(sawer::read_sawer_record(input, header, 1024U, 0U)
                == sawer::SawerRecordReadResult::record);
        first_operation = input.tellg();
    }
    {
        std::fstream file{temporary.path(), std::ios::binary | std::ios::in | std::ios::out};
        file.seekg(first_operation + std::streamoff{28});
        char byte{};
        file.read(&byte, 1);
        file.clear();
        file.seekp(first_operation + std::streamoff{28});
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        file.write(&byte, 1);
    }

    sawer::Document loaded;
    bool recovered = false;
    REQUIRE_THROWS(sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered));
}

TEST_CASE("arbitrary final-byte truncation never fabricates board state")
{
    TemporaryDirectory temporary;
    const auto source_path = temporary.path() / "source.sawer";
    sawer::Document original;
    const auto id = sawer::ObjectId::from_u64(75U);
    REQUIRE(original.insert(test_line(75U)));
    static_cast<void>(sawer::BoardFileSession::create(source_path, original));
    std::ifstream source{source_path, std::ios::binary};
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{source},
        std::istreambuf_iterator<char>{}};
    REQUIRE_FALSE(bytes.empty());

    for (std::size_t size = 0U; size < bytes.size(); ++size) {
        const auto path = temporary.path()
            / ("truncated-" + std::to_string(size) + ".sawer");
        {
            std::ofstream output{path, std::ios::binary};
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(size));
        }
        sawer::Document loaded;
        bool recovered = false;
        try {
            static_cast<void>(sawer::BoardFileSession::open(
                path, loaded, recovered));
            REQUIRE(loaded.size() <= 1U);
            if (loaded.size() == 1U) {
                REQUIRE(loaded.find(id) != nullptr);
                REQUIRE(loaded.find(id)->geometry
                    == original.find(id)->geometry);
            }
        } catch (const std::exception&) {
            REQUIRE(loaded.size() == 0U);
        }
    }
}

TEST_CASE("malformed non-final records are rejected")
{
    TemporaryBoard temporary;
    {
        std::ofstream output{temporary.path(), std::ios::binary};
        output << "{\"format\":\"sawer\",\"version\":1,\"board_id\":\""
               << sawer::ObjectId::from_u64(92U).to_string()
               << "\",\"bounds\":[-1000000,-1000000,1000000,1000000]}\n"
               << "not-json\n{}\n";
    }

    sawer::Document document;
    bool recovered = false;
    REQUIRE_THROWS(
        sawer::BoardFileSession::open(
            temporary.path(), document, recovered));
}

TEST_CASE("coordinates outside the finite board are rejected")
{
    TemporaryBoard temporary;
    const auto id = sawer::ObjectId::from_u64(90U).to_string();
    {
        std::ofstream output{temporary.path(), std::ios::binary};
        output
            << "{\"format\":\"sawer\",\"version\":1,\"board_id\":\""
            << sawer::ObjectId::from_u64(93U).to_string()
            << "\",\"bounds\":[-1000000,-1000000,1000000,1000000]}\n"
            << "{\"seq\":1,\"op\":\"put\",\"id\":\"" << id
            << "\",\"object\":{\"type\":\"line\",\"start\":[1e300,0],"
               "\"end\":[0,0],\"z\":0,\"style\":{\"stroke\":[0,0,0,255],"
               "\"fill\":null,\"width\":1}}}\n"
            << "{}\n";
    }

    sawer::Document document;
    bool recovered = false;
    REQUIRE_THROWS(
        sawer::BoardFileSession::open(
            temporary.path(), document, recovered));
}

TEST_CASE("unsupported board bounds are rejected")
{
    TemporaryBoard temporary;
    {
        std::ofstream output{temporary.path(), std::ios::binary};
        output
            << "{\"format\":\"sawer\",\"version\":1,\"board_id\":\""
            << sawer::ObjectId::from_u64(91U).to_string()
            << "\",\"bounds\":[-10,-10,10,10]}\n";
    }

    sawer::Document document;
    bool recovered = false;
    REQUIRE_THROWS(
        sawer::BoardFileSession::open(
            temporary.path(), document, recovered));
}

TEST_CASE("compacted and append-only boards are equivalent")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto session = sawer::BoardFileSession::create(temporary.path(), document);

    auto first = test_line(10U);
    auto second = test_line(11U);
    REQUIRE(document.insert(first));
    session.queue_put(first);
    REQUIRE(document.insert(second));
    session.queue_put(second);
    session.flush(document);

    REQUIRE(document.remove(first.id).has_value());
    session.queue_delete(first.id);
    session.flush(document);
    session.compact(document);

    sawer::Document loaded;
    bool recovered = false;
    auto reopened =
        sawer::BoardFileSession::open(temporary.path(), loaded, recovered);
    REQUIRE_FALSE(recovered);
    REQUIRE(loaded.size() == 1U);
    REQUIRE(loaded.find(second.id) != nullptr);
    REQUIRE(reopened.sequence() == 1U);
}

TEST_CASE("external modification creates a conflict copy")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto session = sawer::BoardFileSession::create(temporary.path(), document);

    auto object = test_line(20U);
    REQUIRE(document.insert(object));
    session.queue_put(object);

    {
        std::ofstream external{
            temporary.path(), std::ios::binary | std::ios::app};
        external << ' ';
    }

    session.flush(document);
    temporary.track(session.path());

    REQUIRE(session.conflict_created());
    REQUIRE(session.path() != temporary.path());
    REQUIRE(std::filesystem::exists(temporary.path()));
    REQUIRE(std::filesystem::exists(session.path()));

    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(
        sawer::BoardFileSession::open(session.path(), loaded, recovered));
    REQUIRE(loaded.size() == 1U);
    REQUIRE(loaded.find(object.id) != nullptr);
}

TEST_CASE("failed conflict creation preserves the active session")
{
    TemporaryDirectory temporary;
    const auto original_path = temporary.path() / "board.sawer";
    sawer::Document document;
    auto session =
        sawer::BoardFileSession::create(original_path, document);

    auto object = test_line(21U);
    REQUIRE(document.insert(object));
    session.queue_put(object);

    std::error_code error;
    std::filesystem::remove_all(temporary.path(), error);
    REQUIRE_FALSE(error);

    REQUIRE_THROWS(session.flush(document));
    REQUIRE(session.path() == original_path);
    REQUIRE(session.has_pending_operations());

    std::filesystem::create_directories(temporary.path());
    session.flush(document);
    REQUIRE(session.conflict_created());
    REQUIRE(session.path() != original_path);

    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(
        sawer::BoardFileSession::open(session.path(), loaded, recovered));
    REQUIRE(loaded.find(object.id) != nullptr);
}

TEST_CASE("transformed objects reload with exact geometry")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto rectangle = sawer::Object::make_rectangle(
        sawer::ObjectId::from_u64(40U),
        0,
        {{-20.25, 12.5}, {80.75, 63.125}},
        {
            .stroke = {80U, 120U, 220U, 255U},
            .fill = sawer::Color{20U, 30U, 60U, 255U},
            .stroke_width = 6.5,
        });
    sawer::translate_object(rectangle, {1234.125, -876.5});
    sawer::resize_object(
        rectangle,
        sawer::SelectionHandle::bottom_right,
        {1410.875, -700.25});
    const auto expected = rectangle.geometry;
    REQUIRE(document.insert(std::move(rectangle)));
    static_cast<void>(
        sawer::BoardFileSession::create(temporary.path(), document));

    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered));
    REQUIRE_FALSE(recovered);
    REQUIRE(loaded.find(sawer::ObjectId::from_u64(40U))->geometry == expected);
}

TEST_CASE("timed autosave writes operations in the background")
{
    TemporaryBoard temporary;
    sawer::Document document;
    auto session = sawer::BoardFileSession::create(temporary.path(), document);
    auto object = test_line(41U);
    REQUIRE(document.insert(object));
    session.queue_put(object);

    const auto start = std::chrono::steady_clock::now();
    session.flush_if_due(document, std::chrono::milliseconds{0});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed < std::chrono::milliseconds{100});
    REQUIRE(session.has_pending_operations());

    session.flush(document);
    REQUIRE_FALSE(session.has_pending_operations());
    sawer::Document loaded;
    bool recovered = false;
    static_cast<void>(sawer::BoardFileSession::open(
        temporary.path(), loaded, recovered));
    REQUIRE(loaded.find(object.id) != nullptr);
}

TEST_CASE("recent files persist in most-recent order")
{
    TemporaryBoard temporary;
    const auto settings = temporary.path();
    const auto first = settings.parent_path() / "first.sawer";
    const auto second = settings.parent_path() / "second.sawer";

    {
        sawer::RecentFiles recent{settings, 2U};
        recent.touch(first);
        recent.touch(second);
        recent.touch(first);
        REQUIRE(recent.entries().size() == 2U);
        REQUIRE(recent.entries()[0].filename() == "first.sawer");
    }

    const sawer::RecentFiles reloaded{settings, 2U};
    REQUIRE(reloaded.entries().size() == 2U);
    REQUIRE(reloaded.entries()[0].filename() == "first.sawer");
    REQUIRE(reloaded.entries()[1].filename() == "second.sawer");
}

TEST_CASE("oversized recent-file preferences are ignored")
{
    TemporaryBoard temporary;
    {
        std::ofstream output{
            temporary.path(), std::ios::binary | std::ios::trunc};
        output.seekp(1024 * 1024);
        output.put('x');
    }

    const sawer::RecentFiles recent{temporary.path(), 20U};
    REQUIRE(recent.entries().empty());
}

TEST_CASE("renaming a recent file replaces its stale path")
{
    TemporaryBoard temporary;
    const auto settings = temporary.path();
    const auto first = settings.parent_path() / "first.sawer";
    const auto second = settings.parent_path() / "second.sawer";
    const auto renamed = settings.parent_path() / "renamed.sawer";

    {
        sawer::RecentFiles recent{settings, 3U};
        recent.touch(first);
        recent.touch(second);
        recent.replace(second, renamed);

        REQUIRE(recent.entries().size() == 2U);
        REQUIRE(recent.entries()[0].filename() == "renamed.sawer");
        REQUIRE(recent.entries()[1].filename() == "first.sawer");
    }

    const sawer::RecentFiles reloaded{settings, 3U};
    REQUIRE(reloaded.entries().size() == 2U);
    REQUIRE(reloaded.entries()[0].filename() == "renamed.sawer");
    REQUIRE(reloaded.entries()[1].filename() == "first.sawer");
}

TEST_CASE("recent files retain per-board zoom through renames")
{
    TemporaryBoard temporary;
    const auto settings = temporary.path();
    const auto first = settings.parent_path() / "first.sawer";
    const auto second = settings.parent_path() / "second.sawer";
    const auto renamed = settings.parent_path() / "renamed.sawer";

    {
        sawer::RecentFiles recent{settings, 3U};
        recent.touch(first);
        recent.set_zoom(first, 1.75);
        recent.touch(second);
        recent.set_zoom(second, 0.55);
        recent.replace(second, renamed);

        REQUIRE(recent.zoom(first) == 1.75);
        REQUIRE_FALSE(recent.zoom(second).has_value());
        REQUIRE(recent.zoom(renamed) == 0.55);
    }

    const sawer::RecentFiles reloaded{settings, 3U};
    REQUIRE(reloaded.zoom(first) == 1.75);
    REQUIRE_FALSE(reloaded.zoom(second).has_value());
    REQUIRE(reloaded.zoom(renamed) == 0.55);
}

TEST_CASE("legacy recent-file preferences remain readable")
{
    TemporaryBoard temporary;
    const auto settings = temporary.path();
    const auto first = settings.parent_path() / "first.sawer";
    {
        std::ofstream output{
            settings, std::ios::binary | std::ios::trunc};
        output << nlohmann::json::array({first.string()}).dump() << '\n';
    }

    const sawer::RecentFiles recent{settings, 3U};
    REQUIRE(recent.entries().size() == 1U);
    REQUIRE(recent.entries().front().filename() == "first.sawer");
    REQUIRE_FALSE(recent.zoom(first).has_value());
}

} // namespace
