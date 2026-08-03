#pragma once

#include "document/Document.hpp"
#include "document/ObjectId.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sawer {

// Best-effort, read-only load of a board's objects for previews/thumbnails.
// Never writes to the file, never throws on malformed content, and stops once
// max_objects have been replayed so huge boards stay cheap to preview.
[[nodiscard]] Document read_board_preview(
    const std::filesystem::path& path,
    std::size_t max_objects = 4000U);

class BoardFileSession final {
public:
    static BoardFileSession create(
        const std::filesystem::path& path,
        const Document& document);
    static BoardFileSession open(
        const std::filesystem::path& path,
        Document& document,
        bool& recovered_final_line);

    BoardFileSession(BoardFileSession&&) noexcept = default;
    BoardFileSession& operator=(BoardFileSession&&) noexcept = default;
    BoardFileSession(const BoardFileSession&) = delete;
    BoardFileSession& operator=(const BoardFileSession&) = delete;

    void queue_put(const Object& object);
    void queue_delete(ObjectId id);
    void queue_move(ObjectId id, Vec2d delta);
    void queue_style(ObjectId id, const Style& style);
    void flush(const Document& document);
    void flush_if_due(
        const Document& document,
        std::chrono::milliseconds interval = std::chrono::seconds{1});
    void compact(const Document& document);
    // Renames the board file on disk and retargets the session to it. Any
    // pending operations are flushed to the old path first; callers should
    // normally flush() beforehand.
    void rename(const std::filesystem::path& new_path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] ObjectId board_id() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] bool conflict_created() const noexcept;
    [[nodiscard]] bool has_pending_operations() const noexcept;

private:
    struct FileStamp final {
        std::uintmax_t size{};
        std::filesystem::file_time_type modified{};

        friend bool operator==(const FileStamp&, const FileStamp&) = default;
    };

    struct PendingOperation final {
        enum class Type {
            put,
            delete_object,
            move,
            style,
        };

        std::uint64_t sequence{};
        Type type{Type::delete_object};
        std::optional<Object> object;
        std::optional<Style> style;
        Vec2d delta;
        ObjectId id;
    };

    BoardFileSession(
        std::filesystem::path path,
        ObjectId board_id,
        std::uint64_t sequence,
        FileStamp stamp,
        std::set<AssetId> persisted_assets = {},
        bool needs_conversion = false);

    [[nodiscard]] bool externally_modified() const;
    void finish_background_flush(bool wait);
    void start_background_flush();
    void create_conflict_copy(const Document& document);
    void remember_assets(const std::vector<PendingOperation>& operations);
    void reset_persisted_assets(const Document& document);

    [[nodiscard]] static FileStamp capture_stamp(
        const std::filesystem::path& path);
    [[nodiscard]] static std::uint64_t write_snapshot(
        const std::filesystem::path& path,
        ObjectId board_id,
        const Document& document);
    [[nodiscard]] static FileStamp append_operations(
        const std::filesystem::path& path,
        const std::vector<PendingOperation>& operations,
        FileStamp expected_stamp,
        std::set<AssetId> persisted_assets);

    std::filesystem::path path_;
    ObjectId board_id_;
    std::uint64_t sequence_{};
    FileStamp expected_stamp_;
    std::vector<PendingOperation> pending_operations_;
    std::shared_ptr<std::vector<PendingOperation>> in_flight_operations_;
    std::set<AssetId> persisted_assets_;
    std::future<FileStamp> background_flush_;
    std::chrono::steady_clock::time_point last_flush_{
        std::chrono::steady_clock::now()};
    bool conflict_created_{};
    bool append_state_uncertain_{};
    bool needs_conversion_{};
};

} // namespace sawer
