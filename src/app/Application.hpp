#pragma once

#include "canvas/Camera.hpp"
#include "app/Clipboard.hpp"
#include "canvas/Selection.hpp"
#include "document/Command.hpp"
#include "document/Document.hpp"
#include "document/Object.hpp"
#include "image/ImageCodec.hpp"
#include "geometry/StrokeProcessing.hpp"
#include "input/PointerSample.hpp"
#include "input/PointerResampler.hpp"
#include "input/AdaptiveStrokeFilter.hpp"
#include "input/DrawingSettings.hpp"
#include "input/Tool.hpp"
#include "renderer/GpuRenderer.hpp"
#include "storage/BoardFile.hpp"
#include "storage/ClipboardFragment.hpp"
#include "storage/RecentFiles.hpp"
#include "ui/HomeView.hpp"
#include "ui/NavigationTransition.hpp"
#include "ui/Toolbar.hpp"
#include "ui/UnsavedDialog.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SDL_Window;
struct SDL_Cursor;
union SDL_Event;

namespace sawer {

class Application final {
public:
    explicit Application(bool hidden = false);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    [[nodiscard]] int run();
    [[nodiscard]] int run_frame_test(std::uint32_t frame_count);
    [[nodiscard]] int run_resize_test();
    [[nodiscard]] int run_line_test();
    [[nodiscard]] int run_board_loading_test();
    [[nodiscard]] int run_renderer_recovery_test();
    [[nodiscard]] int run_input_test();
    [[nodiscard]] int run_ui_input_test();
    [[nodiscard]] int run_selection_input_test();
    [[nodiscard]] int run_large_board_test();
    [[nodiscard]] int run_drawing_performance_test();
    [[nodiscard]] int run_rendering_performance_test();
    [[nodiscard]] int run_buffer_growth_test();
    [[nodiscard]] int run_stroke_visibility_test(
        const std::filesystem::path& board_path = {});
    [[nodiscard]] int run_home_test();
    [[nodiscard]] int run_navigation_transition_test();
    [[nodiscard]] int run_zoom_state_test();
    [[nodiscard]] std::string_view gpu_diagnostics() const noexcept;
    void new_board();
    void open_board(const std::filesystem::path& path);
    void save_as(const std::filesystem::path& path);

private:
    struct SdlDeleter {
        void operator()(SDL_Window* window) const noexcept;
        void operator()(SDL_Cursor* cursor) const noexcept;
    };

    enum class PendingUnsavedActionKind {
        none,
        new_board,
        home,
        open_path,
        navigate,
        quit,
    };

    struct PendingUnsavedAction final {
        PendingUnsavedActionKind kind{PendingUnsavedActionKind::none};
        std::filesystem::path path;
        std::size_t navigation_index{};
    };

    void handle_event(const SDL_Event& event, bool& running);
    [[nodiscard]] bool update_viewport();
    // Event watch that keeps the window painting while the user drags a
    // window edge; Windows traps the message loop during interactive resize,
    // so the main loop cannot present until the mouse is released.
    static bool live_resize_watch(void* userdata, SDL_Event* event);
    void render_during_live_resize(std::uint32_t window_id) noexcept;
    void begin_gesture(PointerSample sample);
    void update_gesture(PointerSample sample, bool complete = false);
    void finish_gesture(PointerSample sample);
    void cancel_gesture() noexcept;
    void begin_selection_gesture(PointerSample sample);
    void update_selection_gesture(PointerSample sample);
    void finish_selection_gesture(PointerSample sample);
    void cancel_selection_gesture() noexcept;
    void delete_selection();
    void duplicate_selection();
    void nudge_selection(Vec2d delta);
    [[nodiscard]] bool copy_selection();
    void paste_clipboard();
    [[nodiscard]] bool poll_clipboard_paste();
    void restyle_selection(UiAction action);
    void restyle_selection(Color color);
    void restyle_selection_width(double value, bool relative);
    void set_context_stroke_width(double width);
    void adjust_context_stroke_width(double delta);
    [[nodiscard]] PointerSample mouse_sample(
        float x,
        float y,
        std::uint64_t timestamp) const;
    void record_object_state(ObjectId id);
    void record_object_states(const std::vector<ObjectId>& ids);
    void record_move_states(
        const std::vector<ObjectId>& ids,
        Vec2d delta);
    void record_style_states(const std::vector<ObjectId>& ids);
    void flush_board();
    [[nodiscard]] bool flush_board_safely() noexcept;
    void autosave_if_due();
    void autosave_safely() noexcept;
    void request_quit(bool& running) noexcept;
    void update_window_title();
    [[nodiscard]] StyleColorTarget effective_style_color_target() const noexcept;
    void sync_toolbar();
    [[nodiscard]] std::optional<DrawingCursor>
    drawing_cursor_preview() const noexcept;
    void update_cursor() noexcept;
    void tick_ui() noexcept;
    void activate_ui_action(UiAction action);
    void apply_custom_color(UiAction field, Vec2d point);
    void undo_or_redo(bool redo);
    void zoom_by_steps_at(Vec2d screen_anchor, double steps);
    void zoom_by_steps_at_viewport_center(double steps);
    void zoom_at_viewport_center(double factor);
    void set_zoom(double zoom);
    void remember_current_board_zoom() noexcept;
    void restore_board_zoom(const std::filesystem::path& path);
    void fit_content();
    void fit_selection();
    void show_board_dialog(bool save);
    void handle_board_dialog_result(void* result) noexcept;
    void begin_background_open(const std::filesystem::path& path);
    void poll_background_open() noexcept;
    [[nodiscard]] bool poll_background_previews() noexcept;

    void start_session();
    [[nodiscard]] bool has_unsaved_untitled() const noexcept;
    [[nodiscard]] bool confirm_unsaved_untitled(
        PendingUnsavedAction action);
    void handle_unsaved_dialog_event(const SDL_Event& event);
    void handle_unsaved_dialog_choice(UnsavedDialogChoice choice);
    void discard_unsaved_untitled();
    void resume_pending_unsaved_action();
    void enter_home();
    void refresh_home();
    void open_board_from_home(std::size_t index);
    void handle_home_event(const SDL_Event& event, bool& running);
    void reset_navigation_history();
    void record_navigation_target();
    void synchronize_navigation_target();
    void navigate_history(int direction);
    [[nodiscard]] bool restore_navigation_target(std::size_t index);
    [[nodiscard]] static BoardPreview build_board_preview(
        const std::filesystem::path& path);

    void start_rename_current();
    void start_rename_home(std::size_t index);
    void commit_rename();
    void cancel_rename();
    [[nodiscard]] bool handle_rename_event(const SDL_Event& event);
    void start_width_edit();
    void commit_width_edit();
    void cancel_width_edit();
    [[nodiscard]] bool handle_width_edit_event(const SDL_Event& event);
    [[nodiscard]] bool handle_color_edit_event(const SDL_Event& event);
    void cancel_custom_color();

    enum class ViewMode {
        board,
        home,
    };

    struct NavigationTarget final {
        ViewMode view{ViewMode::home};
        std::filesystem::path board_path;

        friend bool operator==(
            const NavigationTarget&,
            const NavigationTarget&) = default;
    };

    enum class RenameTarget {
        none,
        board,
        home,
    };

    enum class SelectionInteraction {
        none,
        marquee,
        move,
        resize,
    };

    struct BackgroundOpenResult final {
        std::filesystem::path path;
        Document document;
        ImageDecodeCache decoded_images;
        std::optional<BoardFileSession> session;
        bool recovered_final_line{};
    };

    std::unique_ptr<SDL_Window, SdlDeleter> window_;
    std::unique_ptr<GpuRenderer> renderer_;
    std::unique_ptr<SDL_Cursor, SdlDeleter> hand_cursor_;
    std::unique_ptr<SDL_Cursor, SdlDeleter> move_cursor_;
    std::unique_ptr<SDL_Cursor, SdlDeleter> nwse_resize_cursor_;
    std::unique_ptr<SDL_Cursor, SdlDeleter> nesw_resize_cursor_;
    Camera camera_{1280.0, 720.0};
    Document document_;
    CommandHistory history_;
    Selection selection_;
    std::optional<ObjectDraft> active_draft_;
    std::uint64_t next_draft_generation_{};
    SelectionInteraction selection_interaction_{SelectionInteraction::none};
    SelectionHandle selection_handle_{SelectionHandle::none};
    Vec2d selection_start_;
    std::vector<ObjectId> selection_edit_ids_;
    std::optional<Aabb> selection_original_bounds_;
    std::optional<SelectionPreview> selection_preview_;
    bool selection_additive_{};
    Tool current_tool_{Tool::pencil};
    DrawingSettings drawing_settings_;
    AdaptiveStrokeFilter adaptive_stroke_filter_;
    PointerResampler pointer_resampler_;
    IncrementalStrokeCurve incremental_stroke_curve_;
    Style current_style_;
    double rectangle_roundness_{};
    StyleColorTarget style_color_target_{StyleColorTarget::stroke};
    BackgroundStyle background_style_{BackgroundStyle::dot};
    Color background_color_{240U, 242U, 247U, 255U};
    std::optional<Color> grid_color_;
    std::optional<BoardFileSession> board_file_;
    Clipboard clipboard_;
    std::future<CanonicalImage> clipboard_paste_;
    std::future<ClipboardFragment> clipboard_fragment_paste_;
    std::uint64_t clipboard_paste_generation_{};
    Vec2d clipboard_paste_anchor_;
    AssetId clipboard_paste_digest_{};
    AssetId last_clipboard_paste_digest_{};
    std::size_t clipboard_fragment_paste_count_{};
    std::string untitled_name_{"Untitled"};
    std::unique_ptr<RecentFiles> recent_files_;
    HomeView home_view_;
    ViewMode view_mode_{ViewMode::board};
    std::vector<NavigationTarget> navigation_history_;
    std::size_t navigation_history_index_{};
    bool replaying_navigation_{};
    bool renaming_{};
    RenameTarget rename_target_{RenameTarget::none};
    std::string rename_text_;
    std::size_t rename_cursor_{};
    std::size_t rename_anchor_{};
    bool rename_pointer_selecting_{};
    std::filesystem::path rename_path_;
    bool width_editing_{};
    std::optional<Color> color_editor_original_grid_;
    bool width_edit_replace_all_{};
    std::string width_edit_text_;

    struct PreviewCacheEntry final {
        std::filesystem::file_time_type modified;
        std::uintmax_t file_size{};
        std::shared_ptr<const BoardPreview> preview;
    };
    struct PreviewTask final {
        std::filesystem::file_time_type modified;
        std::uintmax_t file_size{};
        std::future<BoardPreview> result;
    };
    std::filesystem::path preview_cache_directory_;
    std::unordered_map<std::string, PreviewCacheEntry> preview_cache_;
    std::unordered_map<std::string, PreviewTask> preview_tasks_;
    Toolbar toolbar_;
    UnsavedDialog unsaved_dialog_;
    std::string status_error_;
    std::string status_notice_;
    bool middle_button_down_{};
    bool left_button_down_{};
    bool hand_dragging_{};
    bool ui_pointer_down_{};
    bool title_pointer_down_{};
    bool space_down_{};
    bool shift_down_{};
    bool left_shift_down_{};
    bool right_shift_down_{};
    std::optional<UiAction> color_drag_action_;
    std::optional<Vec2d> pointer_screen_;
    double display_scale_{1.0};
    bool viewport_initialized_{};
    int viewport_pixel_width_{};
    int viewport_pixel_height_{};
    std::uint32_t dialog_event_type_{};
    bool dialog_active_{};
    std::shared_ptr<std::atomic_bool> dialog_delivery_failed_{
        std::make_shared<std::atomic_bool>(false)};
    bool quit_requested_{};
    std::optional<PendingUnsavedAction> pending_unsaved_action_;
    std::optional<UnsavedDialogChoice> next_unsaved_choice_for_test_;
    std::uint64_t lifecycle_generation_{};
    std::uint64_t background_open_origin_generation_{};
    std::optional<std::filesystem::path> background_open_home_path_;
    std::future<BackgroundOpenResult> background_open_;
    std::uint64_t last_ui_tick_{};
    // View whose entrance animation is currently playing or settled; a
    // mismatch with view_mode_ restarts the entrance for the new view.
    std::optional<ViewMode> animated_view_mode_;
    NavigationTransition navigation_transition_;
    bool live_resize_rendering_{};
    std::uint64_t last_live_resize_render_{};
};

} // namespace sawer
