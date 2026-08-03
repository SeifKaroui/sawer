#include "app/Application.hpp"

#include "core/BuildInfo.hpp"
#include "core/Log.hpp"
#include "geometry/StrokeProcessing.hpp"
#include "image/Sha256.hpp"
#include "renderer/GpuRenderer.hpp"
#include "storage/PreviewCache.hpp"
#include "storage/BoardPath.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace sawer {
namespace {

constexpr double minimum_stroke_width = 0.5;
constexpr double maximum_stroke_width = 64.0;
constexpr double precise_stroke_width_step = 0.5;
constexpr double wheel_zoom_base = 1.18;
constexpr double zoom_snap_level = 1.0;
constexpr double zoom_snap_tolerance = 0.015;
constexpr std::uint64_t home_live_resize_interval_ns = 16'666'667ULL;
constexpr std::uint64_t board_live_resize_interval_ns = 8'000'000ULL;
constexpr std::size_t maximum_background_preview_tasks = 4U;

double adaptive_stroke_width_delta(
    const double width,
    const bool increasing,
    const bool precise) noexcept
{
    double step = precise_stroke_width_step;
    if (!precise) {
        if (increasing) {
            step = width < 5.0
                ? 0.5
                : (width < 20.0 ? 1.0 : 2.0);
        } else {
            step = width <= 5.0
                ? 0.5
                : (width <= 20.0 ? 1.0 : 2.0);
        }
    }
    return increasing ? step : -step;
}

double stepped_zoom_target(
    const double current_zoom,
    const double requested_steps) noexcept
{
    if (!std::isfinite(current_zoom) || current_zoom <= 0.0
        || !std::isfinite(requested_steps) || requested_steps == 0.0) {
        return current_zoom;
    }

    // Process batched wheel notches individually. If one notch reaches or
    // crosses 100%, it lands exactly there; later notches in the same event
    // continue from 100% instead of being discarded.
    const double steps = std::clamp(requested_steps, -32.0, 32.0);
    const double direction = steps < 0.0 ? -1.0 : 1.0;
    const double magnitude = std::abs(steps);
    const int whole_steps = static_cast<int>(std::floor(magnitude));
    const double fractional_step =
        magnitude - static_cast<double>(whole_steps);
    double target = current_zoom;
    const auto advance = [&](const double amount) {
        const double candidate =
            target * std::pow(wheel_zoom_base, direction * amount);
        const bool crosses_default =
            (target < zoom_snap_level && candidate > zoom_snap_level)
            || (target > zoom_snap_level && candidate < zoom_snap_level);
        target = crosses_default
                || std::abs(candidate - zoom_snap_level)
                    <= zoom_snap_tolerance
            ? zoom_snap_level
            : candidate;
    };
    for (int step = 0; step < whole_steps; ++step) {
        advance(1.0);
    }
    if (fractional_step > 1.0e-9) {
        advance(fractional_step);
    }
    return target;
}

double clamp_board_translation_axis(
    const double requested,
    const double minimum,
    const double maximum) noexcept
{
    const double lower = -board_half_extent - minimum;
    const double upper = board_half_extent - maximum;
    return lower <= upper ? std::clamp(requested, lower, upper) : 0.0;
}

class SdlLifetime final {
public:
    SdlLifetime()
    {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error{
                std::string{"SDL initialization failed: "} + SDL_GetError()};
        }
    }

    ~SdlLifetime()
    {
        SDL_Quit();
    }

    SdlLifetime(const SdlLifetime&) = delete;
    SdlLifetime& operator=(const SdlLifetime&) = delete;
};

SdlLifetime& sdl_lifetime()
{
    static SdlLifetime lifetime;
    return lifetime;
}

[[noreturn]] void throw_sdl(const std::string_view operation)
{
    throw std::runtime_error{
        std::string{operation} + " failed: " + SDL_GetError()};
}

enum class DialogKind {
    open,
    save,
};

struct DialogRequest final {
    std::uint32_t event_type{};
    DialogKind kind{};
    std::uint64_t lifecycle_generation{};
    std::shared_ptr<std::atomic_bool> delivery_failed;
};

struct DialogResult final {
    DialogKind kind{};
    std::uint64_t lifecycle_generation{};
    std::optional<std::filesystem::path> path;
    std::string error;
};

void SDLCALL board_dialog_callback(
    void* const userdata,
    const char* const* const filelist,
    int)
{
    const std::unique_ptr<DialogRequest> request{
        static_cast<DialogRequest*>(userdata)};
    auto result = std::make_unique<DialogResult>();
    result->kind = request->kind;
    result->lifecycle_generation = request->lifecycle_generation;
    if (filelist == nullptr) {
        result->error = SDL_GetError();
    } else if (filelist[0] != nullptr) {
        result->path = std::filesystem::path{filelist[0]};
    }

    SDL_Event event{};
    event.type = request->event_type;
    event.user.data1 = result.release();
    if (!SDL_PushEvent(&event)) {
        delete static_cast<DialogResult*>(event.user.data1);
        request->delivery_failed->store(true, std::memory_order_release);
    }
}

} // namespace

Application::Application(const bool hidden)
{
    if (!SDL_SetAppMetadata(
            BuildInfo::name.data(),
            BuildInfo::version.data(),
            BuildInfo::identifier.data())) {
        throw std::runtime_error{
            std::string{"Application metadata setup failed: "} + SDL_GetError()};
    }

    static_cast<void>(sdl_lifetime());

    dialog_event_type_ = SDL_RegisterEvents(1);
    if (dialog_event_type_ == static_cast<std::uint32_t>(-1)) {
        throw_sdl("File-dialog event registration");
    }

    if (char* const preference_path = SDL_GetPrefPath(
            BuildInfo::organization.data(), BuildInfo::name.data())) {
        const std::filesystem::path preferences{preference_path};
        recent_files_ = std::make_unique<RecentFiles>(
            preferences / "recent-files.json");
        preview_cache_directory_ = preferences / "previews";
        if (!log::set_file(preferences / "Sawer.log")) {
            log::write(
                log::Level::warning,
                "Could not open the local diagnostic log");
        }
        SDL_free(preference_path);
    }

    constexpr int initial_width = 1280;
    constexpr int initial_height = 720;
    SDL_WindowFlags flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    window_.reset(SDL_CreateWindow(
        BuildInfo::name.data(), initial_width, initial_height, flags));

    if (!window_) {
        throw std::runtime_error{
            std::string{"Window creation failed: "} + SDL_GetError()};
    }
    if (!SDL_SetWindowMinimumSize(window_.get(), 480, 420)) {
        throw_sdl("Window minimum-size configuration");
    }

    hand_cursor_.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER));
    move_cursor_.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE));
    nwse_resize_cursor_.reset(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE));
    nesw_resize_cursor_.reset(
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE));

    static_cast<void>(update_viewport());
    renderer_ = std::make_unique<GpuRenderer>(*window_);
    update_cursor();

    // Windows runs a modal message loop while a window edge is dragged, so
    // SDL_PollEvent starves until the mouse is released. SDL delivers size
    // events synchronously through event watches, letting the watch present
    // frames during the drag for a live resize. Wayland and X11 continue
    // pumping the normal event loop; installing the watch there would change
    // presentation mode and recreate the Vulkan swapchain from inside its
    // resize notification.
#if defined(_WIN32)
    if (!SDL_AddEventWatch(&Application::live_resize_watch, this)) {
        log::write(
            log::Level::warning,
            std::string{"Live-resize event watch registration failed: "}
                + SDL_GetError());
    }
#endif

    log::write(
        log::Level::info,
        std::string{"Started Sawer "} + std::string{BuildInfo::version} +
            " (" + std::string{build_configuration()} + ')');
    update_window_title();
}

Application::~Application()
{
#if defined(_WIN32)
    SDL_RemoveEventWatch(&Application::live_resize_watch, this);
#endif
    remember_current_board_zoom();
    try {
        flush_board();
        remember_current_board_zoom();
    } catch (const std::exception& error) {
        log::write(log::Level::error, error.what());
    }
}

bool Application::live_resize_watch(void* const userdata, SDL_Event* const event)
{
    if (event->type != SDL_EVENT_WINDOW_RESIZED
        && event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        return true;
    }
    auto* const application = static_cast<Application*>(userdata);
    application->render_during_live_resize(event->window.windowID);
    return true;
}

void Application::render_during_live_resize(
    const std::uint32_t window_id) noexcept
{
    // Watches can run on any thread that pushes events; only the main thread
    // may touch the renderer. Re-entrancy is blocked because presenting a
    // frame can itself surface window events.
    if (renderer_ == nullptr || window_ == nullptr || !SDL_IsMainThread()
        || renderer_->rendering()) {
        return;
    }
    if (window_id != SDL_GetWindowID(window_.get())
        || live_resize_rendering_) {
        return;
    }
    // Home rebuilds its responsive card layout and visible thumbnails for
    // every distinct size. Pace that work evenly at display-friendly 60 Hz;
    // the lighter board path retains its faster live-resize cadence.
    const std::uint64_t now = SDL_GetTicksNS();
    const std::uint64_t minimum_interval =
        view_mode_ == ViewMode::home
        ? home_live_resize_interval_ns
        : board_live_resize_interval_ns;
    if (last_live_resize_render_ != 0U
        && now - last_live_resize_render_ < minimum_interval) {
        return;
    }
    live_resize_rendering_ = true;
    try {
        // SDL commonly emits both logical-size and pixel-size events for the
        // same drag tick. The first event records both sizes, so the paired
        // event can be discarded without rebuilding or uploading Home again.
        if (update_viewport()) {
            last_live_resize_render_ = now;
            renderer_->begin_live_resize();
            tick_ui();
            const auto drawing_cursor = drawing_cursor_preview();
            static_cast<void>(renderer_->render(
                camera_,
                document_,
                active_draft_ ? &*active_draft_ : nullptr,
                toolbar_,
                selection_,
                view_mode_ == ViewMode::home ? &home_view_ : nullptr,
                selection_preview_ ? &*selection_preview_ : nullptr,
                unsaved_dialog_.visible() ? &unsaved_dialog_ : nullptr,
                drawing_cursor ? &*drawing_cursor : nullptr));
        }
    } catch (const std::exception& error) {
        log::write(log::Level::error, error.what());
    }
    live_resize_rendering_ = false;
}

int Application::run()
{
    start_session();
    update_cursor();
    bool running = true;
    bool redraw_requested = true;
    bool renderer_recovery_pending = false;
    while (running) {
        const bool animations_active =
            unsaved_dialog_.animating()
            || (view_mode_ == ViewMode::home
                ? home_view_.animating()
                : toolbar_.animating());
        if (!redraw_requested && !animations_active) {
            SDL_Event waited_event{};
            const Sint32 timeout =
                background_open_.valid() || !preview_tasks_.empty()
                ? 16
                : 100;
            if (SDL_WaitEventTimeout(&waited_event, timeout)) {
                handle_event(waited_event, running);
                redraw_requested = true;
            }
        }

        SDL_Event event{};
        while (running && SDL_PollEvent(&event)) {
            handle_event(event, running);
            redraw_requested = true;
        }
        if (!running) {
            break;
        }
        const bool background_was_pending = background_open_.valid();
        poll_background_open();
        if (background_was_pending && !background_open_.valid()) {
            redraw_requested = true;
        }
        if (poll_background_previews()) {
            redraw_requested = true;
        }
        if (poll_clipboard_paste()) {
            redraw_requested = true;
        }
        if (dialog_delivery_failed_->exchange(
                false, std::memory_order_acq_rel)) {
            dialog_active_ = false;
            pending_unsaved_action_.reset();
            status_error_ =
                "The file dialog result could not be delivered.";
            log::write(log::Level::error, status_error_);
            if (view_mode_ == ViewMode::home) {
                refresh_home();
            } else {
                sync_toolbar();
            }
            redraw_requested = true;
        }
        if (quit_requested_) {
            request_quit(running);
            if (!running) {
                break;
            }
        }
        tick_ui();

        // Reaching this point means the modal drag loop has returned control,
        // so restore vsync presentation. MSAA remains active throughout.
        renderer_->end_live_resize();
        const bool animate_frame =
            unsaved_dialog_.animating()
            || (view_mode_ == ViewMode::home
                ? home_view_.animating()
                : toolbar_.animating());
        if (running && (redraw_requested || animate_frame)) {
            try {
                const auto drawing_cursor = drawing_cursor_preview();
                if (!renderer_->render(
                        camera_,
                        document_,
                        active_draft_ ? &*active_draft_ : nullptr,
                        toolbar_,
                        selection_,
                        view_mode_ == ViewMode::home ? &home_view_ : nullptr,
                        selection_preview_ ? &*selection_preview_ : nullptr,
                        unsaved_dialog_.visible()
                            ? &unsaved_dialog_
                            : nullptr,
                        drawing_cursor ? &*drawing_cursor : nullptr)) {
                    SDL_Delay(16U);
                }
                renderer_recovery_pending = false;
            } catch (const std::exception& error) {
                if (renderer_recovery_pending) {
                    throw;
                }
                log::write(
                    log::Level::warning,
                    "Rendering failed; recreating the graphics device: "
                        + std::string{error.what()});
                renderer_.reset();
                try {
                    renderer_ = std::make_unique<GpuRenderer>(*window_);
                } catch (const std::exception& recovery_error) {
                    throw std::runtime_error{
                        "Graphics recovery failed after: "
                        + std::string{error.what()} + "; "
                        + recovery_error.what()};
                }
                renderer_recovery_pending = true;
                redraw_requested = true;
                SDL_Delay(16U);
                continue;
            }
            redraw_requested = false;
        }
        autosave_safely();
    }

    return 0;
}

int Application::run_frame_test(const std::uint32_t frame_count)
{
    std::uint32_t presented_frames = 0U;
    const Uint64 deadline = SDL_GetTicks() + 5'000U;

    while (presented_frames < frame_count && SDL_GetTicks() < deadline) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            bool running = true;
            handle_event(event, running);
            if (!running) {
                return 1;
            }
        }
        tick_ui();

        if (renderer_->render(
                camera_,
                document_,
                active_draft_ ? &*active_draft_ : nullptr,
                toolbar_,
                selection_)) {
            ++presented_frames;
        } else {
            SDL_Delay(16U);
        }
    }

    if (presented_frames != frame_count) {
        log::write(
            log::Level::error,
            "GPU frame test timed out before presentation");
        return 1;
    }

    return 0;
}

int Application::run_resize_test()
{
    constexpr int widths[] = {960, 1440, 640, 1280};
    constexpr int heights[] = {640, 900, 480, 720};

    for (std::size_t index = 0; index < std::size(widths); ++index) {
        if (!SDL_SetWindowSize(window_.get(), widths[index], heights[index])) {
            throw_sdl("Resize test window-size change");
        }
        if (!SDL_SyncWindow(window_.get())) {
            throw_sdl("Resize test window synchronization");
        }

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            bool running = true;
            handle_event(event, running);
            if (!running) {
                return 1;
            }
        }

        // Exercise the live path at every size. It uses immediate
        // presentation but must retain the MSAA render and resolve targets.
        renderer_->begin_live_resize();
        const int result = run_frame_test(2U);
        renderer_->end_live_resize();
        if (result != 0) {
            return result;
        }
    }

    return 0;
}

int Application::run_line_test()
{
    const Style outline{
        .stroke = {238U, 92U, 86U, 255U},
        .fill = std::nullopt,
        .stroke_width = 10.0,
    };
    const Style filled{
        .stroke = {112U, 190U, 255U, 255U},
        .fill = Color{45U, 82U, 120U, 255U},
        .stroke_width = 8.0,
    };
    std::vector<Object> objects;
    objects.push_back(Object::make_line(
        ObjectId::random(),
        document_.next_z_order(),
        {{-300.0, -170.0}, {300.0, 170.0}},
        outline));
    objects.push_back(Object::make_stroke(
        ObjectId::random(),
        document_.next_z_order(),
        {{{-300.0, 130.0}, {-160.0, 30.0}, {0.0, 120.0}, {160.0, 20.0}}},
        outline));
    DecodedImage image_pixels;
    image_pixels.width = 2'050U;
    image_pixels.height = 2U;
    image_pixels.rgba.resize(
        static_cast<std::size_t>(image_pixels.width)
            * image_pixels.height * 4U);
    for (std::size_t pixel = 0U;
         pixel < image_pixels.rgba.size() / 4U; ++pixel) {
        image_pixels.rgba[pixel * 4U] = 32U;
        image_pixels.rgba[pixel * 4U + 1U] = 120U;
        image_pixels.rgba[pixel * 4U + 2U] = 240U;
        image_pixels.rgba[pixel * 4U + 3U] =
            pixel % 2U == 0U ? 96U : 220U;
    }
    auto image_asset = std::make_shared<ImageAsset>();
    image_asset->pixel_width = image_pixels.width;
    image_asset->pixel_height = image_pixels.height;
    image_asset->png = encode_png_rgba(image_pixels);
    image_asset->id = sha256(image_asset->png);
    objects.push_back(Object::make_image(
        ObjectId::random(),
        document_.next_z_order(),
        {image_asset, {-250.0, -30.0}, {250.0, 30.0}}));
    objects.push_back(Object::make_rectangle(
        ObjectId::random(),
        document_.next_z_order(),
        {{-260.0, -120.0}, {-60.0, 20.0}},
        filled));
    objects.push_back(Object::make_ellipse(
        ObjectId::random(),
        document_.next_z_order(),
        {{60.0, -120.0}, {280.0, 30.0}},
        filled));
    for (auto& object : objects) {
        history_.execute(
            std::make_unique<AddObjectCommand>(std::move(object)),
            document_);
    }
    if (const int scene_result = run_frame_test(1U);
        scene_result != 0) {
        return scene_result;
    }
    if (renderer_->stats().image_texture_cache_entries != 1U
        || renderer_->stats().image_texture_tiles < 2U
        || renderer_->stats().image_upload_bytes == 0U) {
        log::write(log::Level::error,
            "GPU image upload, alpha, or tiling path was not exercised");
        return 1;
    }
    if (run_frame_test(1U) != 0
        || renderer_->stats().image_upload_bytes != 0U
        || renderer_->stats().image_texture_cache_entries != 1U) {
        log::write(log::Level::error,
            "GPU image texture was not reused on the retained frame");
        return 1;
    }
    set_zoom(0.02);
    if (run_frame_test(1U) != 0
        || renderer_->stats().image_upload_bytes != 0U) {
        log::write(log::Level::error,
            "GPU image texture was not reused at extreme zoom");
        return 1;
    }
    set_zoom(1.0);

    // Exercise the modal picker's interpolated hue and saturation/value
    // geometry through the real GPU submission path.
    toolbar_.begin_custom_color(
        CustomColorTarget::stroke, {124U, 64U, 220U, 255U});
    sync_toolbar();
    return run_frame_test(3U);
}

int Application::run_renderer_recovery_test()
{
    const DecodedImage pixels{
        2U, 2U,
        {
            255U, 0U, 0U, 255U, 0U, 255U, 0U, 128U,
            0U, 0U, 255U, 64U, 255U, 255U, 255U, 255U,
        },
    };
    auto asset = std::make_shared<ImageAsset>();
    asset->pixel_width = pixels.width;
    asset->pixel_height = pixels.height;
    asset->png = encode_png_rgba(pixels);
    asset->id = sha256(asset->png);
    const ObjectId id = ObjectId::random();
    history_.execute(std::make_unique<AddObjectCommand>(Object::make_image(
        id, document_.next_z_order(),
        {asset, {-100.0, -100.0}, {100.0, 100.0}})), document_);
    if (run_frame_test(1U) != 0
        || renderer_->stats().image_upload_bytes == 0U) {
        log::write(log::Level::error,
            "Initial GPU image resource build failed");
        return 1;
    }

    renderer_.reset();
    renderer_ = std::make_unique<GpuRenderer>(*window_);
    if (run_frame_test(1U) != 0
        || renderer_->stats().image_upload_bytes == 0U
        || renderer_->stats().image_texture_cache_entries != 1U) {
        log::write(log::Level::error,
            "GPU image resources did not rebuild after renderer recovery");
        return 1;
    }
    return 0;
}

int Application::run_input_test()
{
    current_tool_ = Tool::line;
    bool running = true;

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = 320.0F;
    down.button.y = 240.0F;
    handle_event(down, running);

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 760.0F;
    motion.motion.y = 480.0F;
    handle_event(motion, running);

    if (!active_draft_.has_value() || document_.size() != 0U) {
        log::write(log::Level::error, "Line preview entered history too early");
        return 1;
    }

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.x = 760.0F;
    up.button.y = 480.0F;
    handle_event(up, running);

    if (active_draft_.has_value() || document_.size() != 1U
        || !history_.can_undo()) {
        log::write(log::Level::error, "Mouse line was not committed once");
        return 1;
    }
    if (history_.undo(document_).empty() || document_.size() != 0U) {
        log::write(log::Level::error, "Mouse line undo failed");
        return 1;
    }
    if (history_.redo(document_).empty() || document_.size() != 1U) {
        log::write(log::Level::error, "Mouse line redo failed");
        return 1;
    }

    const auto draw_tool = [this, &running](
                               const Tool tool,
                               const float offset) {
        current_tool_ = tool;
        SDL_Event tool_down{};
        tool_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        tool_down.button.button = SDL_BUTTON_LEFT;
        tool_down.button.x = 300.0F + offset;
        tool_down.button.y = 220.0F;
        handle_event(tool_down, running);

        SDL_Event tool_motion{};
        tool_motion.type = SDL_EVENT_MOUSE_MOTION;
        tool_motion.motion.x = 520.0F + offset;
        tool_motion.motion.y = 410.0F;
        handle_event(tool_motion, running);
        if (tool == Tool::pencil) {
            const auto* const preview = active_draft_.has_value()
                ? std::get_if<Stroke>(&active_draft_->geometry)
                : nullptr;
            if (preview == nullptr
                || preview->points.size()
                    <= active_raw_stroke_points_.size()) {
                throw std::runtime_error{
                    "Live pencil preview was not curve-interpolated"};
            }
        }

        SDL_Event tool_up{};
        tool_up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        tool_up.button.button = SDL_BUTTON_LEFT;
        tool_up.button.x = 520.0F + offset;
        tool_up.button.y = 410.0F;
        handle_event(tool_up, running);
    };

    draw_tool(Tool::pencil, 10.0F);
    draw_tool(Tool::rectangle, 20.0F);
    draw_tool(Tool::ellipse, 30.0F);
    if (document_.size() != 4U || history_.size() != 4U) {
        log::write(
            log::Level::error,
            "Mouse tools did not create exactly one command per gesture");
        return 1;
    }

    // Either Shift key must keep constraints active until both are released.
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_LSHIFT;
    key.key.mod = SDL_KMOD_SHIFT;
    handle_event(key, running);
    key.key.scancode = SDL_SCANCODE_RSHIFT;
    handle_event(key, running);
    key.type = SDL_EVENT_KEY_UP;
    key.key.scancode = SDL_SCANCODE_LSHIFT;
    handle_event(key, running);
    if (!shift_down_) {
        log::write(
            log::Level::error,
            "Releasing one Shift key dropped the other Shift key");
        return 1;
    }
    key.key.scancode = SDL_SCANCODE_RSHIFT;
    key.key.mod = SDL_KMOD_NONE;
    handle_event(key, running);

    // Entering temporary pan while drawing must cancel the draft and must not
    // let the release commit a partial object.
    current_tool_ = Tool::line;
    down.button.x = 360.0F;
    down.button.y = 260.0F;
    handle_event(down, running);
    motion.motion.x = 500.0F;
    motion.motion.y = 340.0F;
    handle_event(motion, running);
    const Vec2d camera_before_pan = camera_.position();
    key = {};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_SPACE;
    handle_event(key, running);
    if (active_draft_.has_value()) {
        log::write(
            log::Level::error,
            "Space-pan did not cancel the active drawing draft");
        return 1;
    }
    motion.motion.xrel = 30.0F;
    motion.motion.yrel = -15.0F;
    handle_event(motion, running);
    if (camera_.position() == camera_before_pan) {
        log::write(log::Level::error, "Space-left drag did not pan");
        return 1;
    }
    key.type = SDL_EVENT_KEY_UP;
    handle_event(key, running);
    up.button.x = motion.motion.x;
    up.button.y = motion.motion.y;
    handle_event(up, running);
    if (document_.size() != 4U) {
        log::write(
            log::Level::error,
            "Canceled Space-pan draft entered document history");
        return 1;
    }

    // Camera scale must remain stable while a pointer edit is active.
    handle_event(down, running);
    const double zoom_before_wheel = camera_.zoom();
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.mouse_x = down.button.x;
    wheel.wheel.mouse_y = down.button.y;
    wheel.wheel.y = 1.0F;
    handle_event(wheel, running);
    if (camera_.zoom() != zoom_before_wheel) {
        log::write(
            log::Level::error,
            "Mouse wheel changed zoom during an active gesture");
        return 1;
    }
    key = {};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_ESCAPE;
    handle_event(key, running);
    handle_event(up, running);

    // Discrete wheel zoom must not skip over the familiar 100% level, even
    // when SDL batches several notches into one event.
    set_zoom(0.9);
    wheel.wheel.mouse_x = 640.0F;
    wheel.wheel.mouse_y = 360.0F;
    wheel.wheel.y = 1.0F;
    const Vec2d wheel_anchor{
        static_cast<double>(wheel.wheel.mouse_x),
        static_cast<double>(wheel.wheel.mouse_y),
    };
    const Vec2d anchored_world = camera_.screen_to_world(wheel_anchor);
    handle_event(wheel, running);
    const Vec2d anchored_after = camera_.screen_to_world(wheel_anchor);
    if (std::abs(camera_.zoom() - 1.0) > 1.0e-9
        || std::abs(anchored_after.x - anchored_world.x) > 1.0e-6
        || std::abs(anchored_after.y - anchored_world.y) > 1.0e-6) {
        log::write(
            log::Level::error,
            "Mouse wheel skipped 100% or moved its screen anchor");
        return 1;
    }
    set_zoom(0.8);
    wheel.wheel.y = 3.0F;
    handle_event(wheel, running);
    if (std::abs(camera_.zoom() - wheel_zoom_base) > 1.0e-9) {
        log::write(
            log::Level::error,
            "Batched mouse-wheel steps did not continue from 100%");
        return 1;
    }
    set_zoom(1.0);

    // Alt-modified letters are reserved for platform/window shortcuts.
    current_tool_ = Tool::line;
    key = {};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_P;
    key.key.mod = SDL_KMOD_ALT;
    handle_event(key, running);
    if (current_tool_ != Tool::line) {
        log::write(log::Level::error, "Alt-letter activated a tool shortcut");
        return 1;
    }

    return 0;
}

int Application::run_ui_input_test()
{
    const UiControl* const line_button = toolbar_.find(UiAction::line);
    if (line_button == nullptr) {
        log::write(log::Level::error, "Toolbar omitted the line control");
        return 1;
    }

    const float button_x = static_cast<float>(
        line_button->bounds.x + line_button->bounds.width * 0.5);
    const float button_y = static_cast<float>(
        line_button->bounds.y + line_button->bounds.height * 0.5);
    bool running = true;
    SDL_Event ui_down{};
    ui_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ui_down.button.button = SDL_BUTTON_LEFT;
    ui_down.button.x = button_x;
    ui_down.button.y = button_y;
    handle_event(ui_down, running);
    if (current_tool_ == Tool::line) {
        log::write(
            log::Level::error,
            "Toolbar action activated before pointer release");
        return 1;
    }

    SDL_Event ui_up{};
    ui_up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    ui_up.button.button = SDL_BUTTON_LEFT;
    ui_up.button.x = button_x;
    ui_up.button.y = button_y;
    handle_event(ui_up, running);

    if (current_tool_ != Tool::line || document_.size() != 0U
        || active_draft_.has_value()) {
        log::write(log::Level::error, "Toolbar input leaked into the canvas");
        return 1;
    }

    const float canvas_y = static_cast<float>(toolbar_.height() + 100.0);
    SDL_Event canvas_down{};
    canvas_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    canvas_down.button.button = SDL_BUTTON_LEFT;
    canvas_down.button.x = 320.0F;
    canvas_down.button.y = canvas_y;
    handle_event(canvas_down, running);

    SDL_Event canvas_up{};
    canvas_up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    canvas_up.button.button = SDL_BUTTON_LEFT;
    canvas_up.button.x = 620.0F;
    canvas_up.button.y = canvas_y + 120.0F;
    handle_event(canvas_up, running);

    if (document_.size() != 1U || !history_.can_undo()) {
        log::write(log::Level::error, "Canvas input failed after toolbar use");
        return 1;
    }

    const UiControl* const hand_button = toolbar_.find(UiAction::hand);
    if (hand_button == nullptr) {
        log::write(log::Level::error, "Toolbar omitted the hand control");
        return 1;
    }
    const float hand_x = static_cast<float>(
        hand_button->bounds.x + hand_button->bounds.width * 0.5);
    const float hand_y = static_cast<float>(
        hand_button->bounds.y + hand_button->bounds.height * 0.5);
    ui_down.button.x = hand_x;
    ui_down.button.y = hand_y;
    handle_event(ui_down, running);
    ui_up.button.x = hand_x;
    ui_up.button.y = hand_y;
    handle_event(ui_up, running);

    const Vec2d camera_before = camera_.position();
    canvas_down.button.x = 420.0F;
    canvas_down.button.y = canvas_y + 80.0F;
    handle_event(canvas_down, running);
    SDL_Event pan_motion{};
    pan_motion.type = SDL_EVENT_MOUSE_MOTION;
    pan_motion.motion.x = 460.0F;
    pan_motion.motion.y = canvas_y + 60.0F;
    pan_motion.motion.xrel = 40.0F;
    pan_motion.motion.yrel = -20.0F;
    handle_event(pan_motion, running);
    canvas_up.button.x = pan_motion.motion.x;
    canvas_up.button.y = pan_motion.motion.y;
    handle_event(canvas_up, running);
    if (current_tool_ != Tool::hand
        || camera_.position() == camera_before
        || document_.size() != 1U) {
        log::write(log::Level::error, "Hand tool did not pan cleanly");
        return 1;
    }

    const auto click_control = [&](const UiAction action) {
        const UiControl* const control = toolbar_.find(action);
        if (control == nullptr) return false;
        const float control_x = static_cast<float>(
            control->bounds.x + control->bounds.width * 0.5);
        const float control_y = static_cast<float>(
            control->bounds.y + control->bounds.height * 0.5);
        ui_down.button.x = control_x;
        ui_down.button.y = control_y;
        handle_event(ui_down, running);
        ui_up.button.x = control_x;
        ui_up.button.y = control_y;
        handle_event(ui_up, running);
        return true;
    };
    if (!click_control(UiAction::pencil)
        || !click_control(UiAction::stabilization_light)
        || drawing_settings_.stabilization
            != StrokeStabilization::light
        || !click_control(UiAction::stabilization_default)
        || drawing_settings_.stabilization
            != StrokeStabilization::standard
        || !click_control(UiAction::stabilization_strong)
        || drawing_settings_.stabilization
            != StrokeStabilization::strong) {
        log::write(
            log::Level::error,
            "Stroke stabilization preset control failed");
        return 1;
    }
    if (!click_control(UiAction::stabilization_off)
        || drawing_settings_.stabilization != StrokeStabilization::off) {
        log::write(
            log::Level::error,
            "Stroke stabilization controls failed");
        return 1;
    }

    if (!click_control(UiAction::pencil)) {
        log::write(log::Level::error, "Pencil control disappeared");
        return 1;
    }
    canvas_down.button.x = 360.0F;
    canvas_down.button.y = canvas_y + 40.0F;
    handle_event(canvas_down, running);
    SDL_Event escape{};
    escape.type = SDL_EVENT_KEY_DOWN;
    escape.key.scancode = SDL_SCANCODE_ESCAPE;
    handle_event(escape, running);
    if (active_draft_.has_value() || current_tool_ != Tool::pencil) {
        log::write(
            log::Level::error,
            "First Escape did not only cancel the active gesture");
        return 1;
    }
    canvas_up.button.x = canvas_down.button.x;
    canvas_up.button.y = canvas_down.button.y;
    handle_event(canvas_up, running);
    handle_event(escape, running);
    if (current_tool_ != Tool::select) {
        log::write(
            log::Level::error,
            "Second Escape did not return to Select");
        return 1;
    }
    return run_frame_test(1U);
}

int Application::run_home_test()
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("sawer-home-test-" + ObjectId::random().to_string());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    // Isolate recent-file writes from the user's real settings.
    recent_files_ = std::make_unique<RecentFiles>(
        directory / "recent-files.json", 50U);
    preview_cache_directory_ = directory / "preferences" / "previews";
    preview_cache_.clear();

    // The empty recent-files screen must render cleanly.
    view_mode_ = ViewMode::home;
    refresh_home();
    static_cast<void>(renderer_->render(
        camera_, document_, nullptr, toolbar_, selection_, &home_view_));

    {
        Document drawn;
        static_cast<void>(drawn.insert(Object::make_line(
            ObjectId::random(),
            drawn.next_z_order(),
            {{-50.0, -30.0}, {60.0, 40.0}},
            {
                .stroke = {40U, 44U, 52U, 255U},
                .fill = std::nullopt,
                .stroke_width = 4.0,
            })));
        for (std::size_t index = 0U; index < 700U; ++index) {
            static_cast<void>(drawn.insert(Object::make_line(
                ObjectId::random(),
                drawn.next_z_order(),
                {
                    {0.0, static_cast<double>(index % 30U)},
                    {80.0, static_cast<double>(index % 30U)},
                })));
        }
        static_cast<void>(drawn.insert(Object::make_line(
            ObjectId::random(),
            drawn.next_z_order(),
            {{920.0, 0.0}, {1'000.0, 30.0}})));
        auto first = BoardFileSession::create(
            directory / "Board one.sawer", drawn);
        first.flush(drawn);

        Document empty;
        auto second = BoardFileSession::create(
            directory / "Board two.sawer", empty);
        second.flush(empty);
    }

    recent_files_->touch(directory / "Board one.sawer");
    recent_files_->touch(directory / "Board two.sawer");
    enter_home();
    const Uint64 preview_deadline = SDL_GetTicks() + 5'000U;
    while (!preview_tasks_.empty() && SDL_GetTicks() < preview_deadline) {
        static_cast<void>(poll_background_previews());
        SDL_Delay(1U);
    }
    int result = 0;

    // Home is application chrome, not drawing space: it starts with the
    // normal arrow and only switches to a pointer over interactive controls.
    if (SDL_GetCursor() != SDL_GetDefaultCursor()) {
        log::write(log::Level::error, "Home did not restore the arrow cursor");
        result = 1;
    }
    bool cursor_running = true;
    const UiRect header = home_view_.header_bounds();
    SDL_Event header_motion{};
    header_motion.type = SDL_EVENT_MOUSE_MOTION;
    header_motion.motion.x = static_cast<float>(header.x + header.width * 0.5);
    header_motion.motion.y = static_cast<float>(header.y + header.height * 0.5);
    handle_event(header_motion, cursor_running);
    if (SDL_GetCursor() != SDL_GetDefaultCursor()) {
        log::write(
            log::Level::error,
            "Non-interactive home header did not use the arrow cursor");
        result = 1;
    }
    const UiControl& cursor_button =
        home_view_.controls()[home_view_.boards().size()];
    SDL_Event button_motion{};
    button_motion.type = SDL_EVENT_MOUSE_MOTION;
    button_motion.motion.x = static_cast<float>(
        cursor_button.bounds.x + cursor_button.bounds.width * 0.5);
    button_motion.motion.y = static_cast<float>(
        cursor_button.bounds.y + cursor_button.bounds.height * 0.5);
    handle_event(button_motion, cursor_running);
    if (hand_cursor_ && SDL_GetCursor() != hand_cursor_.get()) {
        log::write(
            log::Level::error,
            "Interactive home control did not use the pointer cursor");
        result = 1;
    }

    // Exercise the home render path (including a board thumbnail) so GPU
    // validation covers it.
    static_cast<void>(renderer_->render(
        camera_, document_, nullptr, toolbar_, selection_, &home_view_));

    if (home_view_.boards().size() != 2U) {
        log::write(log::Level::error, "Home did not list both recent boards");
        result = 1;
    }
    const bool any_preview = std::ranges::any_of(
        home_view_.boards(),
        [](const HomeBoard& board) {
            return board.preview != nullptr
                && board.preview->has_content;
        });
    if (!any_preview) {
        log::write(log::Level::error, "Home board preview was not generated");
        result = 1;
    }
    const auto complete_preview = std::ranges::find_if(
        home_view_.boards(),
        [](const HomeBoard& board) {
            return board.preview != nullptr
                && board.preview->has_content;
        });
    bool late_object_visible = false;
    if (complete_preview != home_view_.boards().end()) {
        const auto& rgba = complete_preview->preview->rgba;
        const std::size_t right_edge =
            BoardPreview::pixel_width * 3U / 4U;
        for (std::size_t y = 0U;
             y < BoardPreview::pixel_height && !late_object_visible;
             ++y) {
            for (std::size_t x = right_edge;
                 x < BoardPreview::pixel_width;
                 ++x) {
                late_object_visible =
                    rgba[
                        (y * BoardPreview::pixel_width + x) * 4U + 3U]
                    > 0U;
                if (late_object_visible) {
                    break;
                }
            }
        }
    }
    if (!late_object_visible) {
        log::write(
            log::Level::error,
            "Home preview omitted objects after the old snapshot cap");
        result = 1;
    }

    // Responsive and DPI transitions rebuild the font atlas. Exercise several
    // generations in quick succession so stale GPU atlas reuse cannot return.
    for (const double scale : {0.85, 1.0, 1.25, 0.9, 1.5, 1.0}) {
        home_view_.relayout(
            camera_.viewport().x,
            camera_.viewport().y,
            scale,
            toolbar_.theme());
        if (!renderer_->render(
                camera_, document_, nullptr, toolbar_, selection_, &home_view_)
            || renderer_->stats().text_cache_entries == 0U) {
            log::write(
                log::Level::error,
                "Home font-atlas transition failed to render text");
            result = 1;
            break;
        }
    }
    refresh_home();

    // Exercise the alternate palette and the visible error state through the
    // real GPU path, not only the layout tests.
    home_view_.update(
        camera_.viewport().x,
        camera_.viewport().y,
        display_scale_,
        Theme::dark,
        home_view_.boards(),
        "The selected board could not be opened.");
    static_cast<void>(renderer_->render(
        camera_, document_, nullptr, toolbar_, selection_, &home_view_));
    refresh_home();

    // Rendering work must be proportional to visible rows, not every board in
    // recent files. This keeps live resizing steady for large home galleries.
    const auto synthetic_frame_vertices = [&](const std::size_t count) {
        std::vector<HomeBoard> synthetic;
        synthetic.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            synthetic.push_back(HomeBoard{
                "Synthetic " + std::to_string(index + 1U),
                "Today",
                {},
                {},
            });
        }
        home_view_.update(
            camera_.viewport().x,
            camera_.viewport().y,
            display_scale_,
            toolbar_.theme(),
            std::move(synthetic));
        static_cast<void>(renderer_->render(
            camera_, document_, nullptr, toolbar_, selection_, &home_view_));
        return renderer_->stats().emitted_vertices;
    };
    const std::size_t short_gallery_vertices = synthetic_frame_vertices(24U);
    const std::size_t large_gallery_vertices = synthetic_frame_vertices(240U);
    if (large_gallery_vertices > short_gallery_vertices + 64U) {
        log::write(
            log::Level::error,
            "Off-screen home cards increased submitted frame geometry");
        result = 1;
    }
    refresh_home();

    // Thumbnail density must not affect Home's geometry count: even a fully
    // covered raster is one textured quad and cannot exhaust the overlay.
    auto dense_preview = std::make_shared<BoardPreview>();
    dense_preview->has_content = true;
    dense_preview->rgba.resize(
        static_cast<std::size_t>(BoardPreview::pixel_width)
            * BoardPreview::pixel_height * 4U,
        255U);
    std::vector<HomeBoard> dense_boards;
    dense_boards.reserve(12U);
    for (std::size_t index = 0U; index < 12U; ++index) {
        dense_boards.push_back(HomeBoard{
            "Dense " + std::to_string(index + 1U),
            "Today",
            dense_preview,
            directory / ("Dense " + std::to_string(index + 1U) + ".sawer"),
        });
    }
    home_view_.update(
        camera_.viewport().x,
        camera_.viewport().y,
        display_scale_,
        toolbar_.theme(),
        std::move(dense_boards));
    for (std::size_t frame = 0U; frame < 10U; ++frame) {
        home_view_.tick(0.1);
    }
    static_cast<void>(renderer_->render(
        camera_, document_, nullptr, toolbar_, selection_, &home_view_));
    const std::size_t expected_thumbnail_upload =
        static_cast<std::size_t>(BoardPreview::pixel_width)
        * BoardPreview::pixel_height * 4U;
    if (renderer_->stats().thumbnail_upload_bytes
        != expected_thumbnail_upload) {
        log::write(
            log::Level::error,
            "Shared Home thumbnail was not uploaded exactly once");
        result = 1;
    }
    static_cast<void>(renderer_->render(
        camera_, document_, nullptr, toolbar_, selection_, &home_view_));
    if (renderer_->stats().thumbnail_upload_bytes != 0U) {
        log::write(
            log::Level::error,
            "Unchanged Home thumbnail repeated its GPU upload");
        result = 1;
    }
    refresh_home();

    // Rename a board from the home screen.
    start_rename_home(0U);
    rename_text_ = "Renamed From Home";
    commit_rename();
    if (!std::filesystem::exists(directory / "Renamed From Home.sawer")) {
        log::write(log::Level::error, "Home rename did not rename the file");
        result = 1;
    }

    const UiControl* new_button = nullptr;
    for (const auto& control : home_view_.controls()) {
        if (control.action == UiAction::home_new_board) {
            new_button = &control;
        }
    }
    if (new_button == nullptr) {
        log::write(log::Level::error, "Home is missing the New board button");
        result = 1;
    } else {
        SDL_Event down{};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.x = static_cast<float>(
            new_button->bounds.x + new_button->bounds.width * 0.5);
        down.button.y = static_cast<float>(
            new_button->bounds.y + new_button->bounds.height * 0.5);
        bool running = true;
        handle_event(down, running);
        SDL_Event up = down;
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        handle_event(up, running);
        if (view_mode_ != ViewMode::board || board_file_.has_value()) {
            log::write(
                log::Level::error,
                "New board from home did not open an untitled board");
            result = 1;
        }
    }

    // A brand-new unsaved board starts as "Untitled" and its title can be
    // edited before Save As chooses the eventual file location.
    if (view_mode_ == ViewMode::board && !board_file_.has_value()) {
        bool running = true;
        if (toolbar_.filename() != "Untitled") {
            log::write(
                log::Level::error, "New board was not named Untitled");
            result = 1;
        }
        start_rename_current();
        SDL_Event select_all{};
        select_all.type = SDL_EVENT_KEY_DOWN;
        select_all.key.scancode = SDL_SCANCODE_A;
        select_all.key.mod = SDL_KMOD_CTRL;
        handle_event(select_all, running);
        SDL_Event replacement{};
        replacement.type = SDL_EVENT_TEXT_INPUT;
        replacement.text.text = "My Board";
        handle_event(replacement, running);
        if (rename_text_ != "My Board"
            || rename_cursor_ != rename_text_.size()
            || rename_anchor_ != rename_cursor_) {
            log::write(
                log::Level::error,
                "Ctrl+A did not replace the selected board title");
            result = 1;
        }
        commit_rename();
        if (toolbar_.filename() != "My Board"
            || !has_unsaved_untitled()
            || board_file_.has_value()) {
            log::write(
                log::Level::error,
                "Unsaved board rename did not update its pending title");
            result = 1;
        }
        // Keep the rest of this fixture free to exercise navigation without
        // invoking a real save dialog.
        untitled_name_ = "Untitled";
        update_window_title();
    }

    // Opening a recent board from home must fully
    // load before the board view accepts input, so a quick first stroke is not
    // discarded by the pending load swapping in the document.
    const auto nav_dir = std::filesystem::temp_directory_path()
        / ("sawer-home-nav-" + ObjectId::random().to_string());
    std::filesystem::create_directories(nav_dir, error);
    {
        Document drawn;
        static_cast<void>(drawn.insert(Object::make_line(
            ObjectId::random(),
            drawn.next_z_order(),
            {{-40.0, -20.0}, {50.0, 30.0}},
            {
                .stroke = {40U, 44U, 52U, 255U},
                .fill = std::nullopt,
                .stroke_width = 4.0,
            })));
        auto board = BoardFileSession::create(nav_dir / "Content.sawer", drawn);
        board.flush(drawn);
    }
    recent_files_->touch(nav_dir / "Content.sawer");
    enter_home();
    if (home_view_.boards().empty()) {
        log::write(log::Level::error, "Recent list showed no boards");
        result = 1;
    } else {
        open_board_from_home(0U);
        // The synchronous open must leave the board fully loaded and ready to
        // draw immediately, with no pending background open.
        if (view_mode_ != ViewMode::board || !board_file_.has_value()
            || background_open_.valid() || document_.size() != 1U) {
            log::write(
                log::Level::error, "Board did not load correctly from home");
            result = 1;
        } else {
            const std::size_t before = document_.size();
            current_tool_ = Tool::line;
            bool running = true;
            SDL_Event down{};
            down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = 420.0F;
            down.button.y = 300.0F;
            handle_event(down, running);
            SDL_Event motion{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.x = 540.0F;
            motion.motion.y = 380.0F;
            handle_event(motion, running);
            SDL_Event up{};
            up.type = SDL_EVENT_MOUSE_BUTTON_UP;
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = 540.0F;
            up.button.y = 380.0F;
            handle_event(up, running);
            poll_background_open();
            if (document_.size() != before + 1U) {
                log::write(
                    log::Level::error,
                    "First stroke after navigating from home was lost");
                result = 1;
            }
        }
    }

    // Dedicated keyboard Back/Forward keys and their conventional Alt-arrow
    // equivalents traverse the same Home/board history.
    reset_navigation_history();
    enter_home();
    bool navigation_running = true;
    const auto press_navigation_key = [&](const SDL_Scancode scancode,
                                          const SDL_Keymod modifiers
                                              = SDL_KMOD_NONE) {
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = scancode;
        key.key.mod = modifiers;
        handle_event(key, navigation_running);
    };
    press_navigation_key(SDL_SCANCODE_AC_BACK);
    if (view_mode_ != ViewMode::board || !board_file_.has_value()
        || board_file_->path() != nav_dir / "Content.sawer") {
        log::write(
            log::Level::error,
            "Keyboard Back did not restore the previous board");
        result = 1;
    }
    press_navigation_key(SDL_SCANCODE_AC_FORWARD);
    if (view_mode_ != ViewMode::home) {
        log::write(
            log::Level::error,
            "Keyboard Forward did not restore Home");
        result = 1;
    }
    press_navigation_key(SDL_SCANCODE_LEFT, SDL_KMOD_ALT);
    press_navigation_key(SDL_SCANCODE_RIGHT, SDL_KMOD_ALT);
    if (view_mode_ != ViewMode::home) {
        log::write(
            log::Level::error,
            "Alt-arrow history navigation did not round trip");
        result = 1;
    }

    // New-board-from-home: the first stroke must persist and actually render.
    enter_home();
    const UiControl* nb_button = nullptr;
    for (const auto& control : home_view_.controls()) {
        if (control.action == UiAction::home_new_board) {
            nb_button = &control;
        }
    }
    if (nb_button != nullptr) {
        bool running = true;
        SDL_Event click_down{};
        click_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        click_down.button.button = SDL_BUTTON_LEFT;
        click_down.button.x = static_cast<float>(
            nb_button->bounds.x + nb_button->bounds.width * 0.5);
        click_down.button.y = static_cast<float>(
            nb_button->bounds.y + nb_button->bounds.height * 0.5);
        handle_event(click_down, running);
        SDL_Event click = click_down;
        click.type = SDL_EVENT_MOUSE_BUTTON_UP;
        handle_event(click, running);

        current_tool_ = Tool::line;
        SDL_Event down{};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.x = 420.0F;
        down.button.y = 300.0F;
        handle_event(down, running);
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.x = 540.0F;
        motion.motion.y = 380.0F;
        handle_event(motion, running);
        SDL_Event up{};
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.button = SDL_BUTTON_LEFT;
        up.button.x = 540.0F;
        up.button.y = 380.0F;
        handle_event(up, running);

        if (document_.size() != 1U) {
            log::write(
                log::Level::error,
                "New-board first stroke lost from the document");
            result = 1;
        }
        static_cast<void>(renderer_->render(
            camera_, document_, nullptr, toolbar_, selection_, nullptr));
        if (renderer_->stats().visible_objects < 1U) {
            log::write(
                log::Level::error,
                "New-board first stroke was not rendered");
            result = 1;
        }

        next_unsaved_choice_for_test_ = UnsavedDialogChoice::cancel;
        press_navigation_key(SDL_SCANCODE_AC_BACK);
        if (view_mode_ != ViewMode::board || document_.size() != 1U) {
            log::write(
                log::Level::error,
                "Canceling unsaved navigation discarded the Untitled board");
            result = 1;
        }
    }

    // Home-button navigation must not leave board-mode pointer state stuck.
    // Reaching home via the toolbar Home button splits the click (press in
    // board mode, release in home mode); if ui_pointer_down_ stays set, the
    // first release on the next board is swallowed instead of committing.
    save_as(directory / "Home navigation.sawer");
    new_board();
    view_mode_ = ViewMode::board;
    sync_toolbar();
    if (const UiControl* const home_button = toolbar_.find(UiAction::go_home)) {
        bool running = true;
        SDL_Event press{};
        press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        press.button.button = SDL_BUTTON_LEFT;
        press.button.x = static_cast<float>(
            home_button->bounds.x + home_button->bounds.width * 0.5);
        press.button.y = static_cast<float>(
            home_button->bounds.y + home_button->bounds.height * 0.5);
        handle_event(press, running);
        SDL_Event release{};
        release.type = SDL_EVENT_MOUSE_BUTTON_UP;
        release.button.button = SDL_BUTTON_LEFT;
        release.button.x = press.button.x;
        release.button.y = press.button.y;
        handle_event(release, running);

        if (view_mode_ != ViewMode::home) {
            log::write(log::Level::error, "Home button did not open home");
            result = 1;
        }
        if (ui_pointer_down_) {
            log::write(
                log::Level::error,
                "Pointer state stuck after the Home button");
            result = 1;
        }

        const UiControl* hb_new = nullptr;
        for (const auto& control : home_view_.controls()) {
            if (control.action == UiAction::home_new_board) {
                hb_new = &control;
            }
        }
        if (hb_new != nullptr) {
            SDL_Event new_down{};
            new_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            new_down.button.button = SDL_BUTTON_LEFT;
            new_down.button.x = static_cast<float>(
                hb_new->bounds.x + hb_new->bounds.width * 0.5);
            new_down.button.y = static_cast<float>(
                hb_new->bounds.y + hb_new->bounds.height * 0.5);
            handle_event(new_down, running);
            SDL_Event new_click = new_down;
            new_click.type = SDL_EVENT_MOUSE_BUTTON_UP;
            handle_event(new_click, running);

            const UiControl* const focused = toolbar_.focused_control();
            if (focused != nullptr
                && focused->action == UiAction::go_home) {
                log::write(
                    log::Level::error,
                    "Home button focus remained after returning to a board");
                result = 1;
            }

            current_tool_ = Tool::line;
            SDL_Event down{};
            down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = 420.0F;
            down.button.y = 300.0F;
            handle_event(down, running);
            SDL_Event motion{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.x = 540.0F;
            motion.motion.y = 380.0F;
            handle_event(motion, running);
            SDL_Event up{};
            up.type = SDL_EVENT_MOUSE_BUTTON_UP;
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = 540.0F;
            up.button.y = 380.0F;
            handle_event(up, running);

            if (document_.size() != 1U) {
                log::write(
                    log::Level::error,
                    "First stroke after Home-button navigation was lost");
                result = 1;
            }
        }
    }

    board_file_.reset();
    document_.clear();
    history_ = {};
    history_.mark_saved(document_);
    untitled_name_ = "Untitled";
    view_mode_ = ViewMode::home;
    refresh_home();

    // Drag-and-drop uses the same guarded background-open lifecycle as the
    // native picker.
    const std::string dropped_path =
        (nav_dir / "Content.sawer").string();
    bool drop_running = true;
    SDL_Event drop{};
    drop.type = SDL_EVENT_DROP_FILE;
    drop.drop.data = dropped_path.c_str();
    handle_event(drop, drop_running);
    const Uint64 drop_deadline = SDL_GetTicks() + 5'000U;
    while (background_open_.valid() && SDL_GetTicks() < drop_deadline) {
        poll_background_open();
        SDL_Delay(1U);
    }
    if (view_mode_ != ViewMode::board || !board_file_.has_value()
        || board_file_->path() != nav_dir / "Content.sawer"
        || document_.size() == 0U) {
        log::write(
            log::Level::error,
            "Dropped board did not complete the guarded open flow");
        result = 1;
    }

    board_file_.reset();
    document_.clear();
    history_ = {};
    history_.mark_saved(document_);
    view_mode_ = ViewMode::board;
    history_.execute(
        std::make_unique<AddObjectCommand>(Object::make_line(
            ObjectId::from_u64(0xC105EU),
            document_.next_z_order(),
            {{0.0, 0.0}, {50.0, 30.0}})),
        document_);
    const std::size_t dirty_untitled_size = document_.size();

    next_unsaved_choice_for_test_ = UnsavedDialogChoice::cancel;
    new_board();
    if (document_.size() != dirty_untitled_size
        || !document_.dirty() || board_file_.has_value()) {
        log::write(
            log::Level::error,
            "Canceling New Board discarded a dirty Untitled document");
        result = 1;
    }

    next_unsaved_choice_for_test_ = UnsavedDialogChoice::discard;
    enter_home();
    if (view_mode_ != ViewMode::home || document_.size() != 0U
        || document_.dirty() || board_file_.has_value()) {
        log::write(
            log::Level::error,
            "Discard and continue did not navigate Home cleanly");
        result = 1;
    }

    view_mode_ = ViewMode::board;
    history_.execute(
        std::make_unique<AddObjectCommand>(Object::make_line(
            ObjectId::from_u64(0xC105FU),
            document_.next_z_order(),
            {{0.0, 0.0}, {50.0, 30.0}})),
        document_);
    bool close_running = true;
    SDL_Event close_event{};
    close_event.type = SDL_EVENT_QUIT;
    handle_event(close_event, close_running);
    if (!close_running || !unsaved_dialog_.visible()
        || document_.size() != 1U || !document_.dirty()) {
        log::write(
            log::Level::error,
            "Close did not show the unsaved changes dialog");
        result = 1;
    }
    unsaved_dialog_.tick(0.2);
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        nullptr,
        toolbar_,
        selection_,
        nullptr,
        nullptr,
        &unsaved_dialog_));
    SDL_Event cancel_close{};
    cancel_close.type = SDL_EVENT_KEY_DOWN;
    cancel_close.key.scancode = SDL_SCANCODE_ESCAPE;
    handle_event(cancel_close, close_running);
    if (!close_running || unsaved_dialog_.visible()
        || document_.size() != 1U || !document_.dirty()) {
        log::write(
            log::Level::error,
            "Canceling the close dialog did not retain the document");
        result = 1;
    }

    next_unsaved_choice_for_test_ = UnsavedDialogChoice::discard;
    handle_event(close_event, close_running);
    if (close_running || document_.size() != 0U
        || document_.dirty()) {
        log::write(
            log::Level::error,
            "Discarding from the close flow did not close cleanly");
        result = 1;
    }

    board_file_.reset();
    document_.clear();
    history_ = {};
    const Uint64 cleanup_deadline = SDL_GetTicks() + 5'000U;
    while (!preview_tasks_.empty() && SDL_GetTicks() < cleanup_deadline) {
        static_cast<void>(poll_background_previews());
        SDL_Delay(1U);
    }
    std::filesystem::remove_all(directory, error);
    std::filesystem::remove_all(nav_dir, error);
    return result;
}

int Application::run_selection_input_test()
{
    const ObjectId line_id = ObjectId::from_u64(9001U);
    history_.execute(
        std::make_unique<AddObjectCommand>(Object::make_line(
            line_id,
            document_.next_z_order(),
            {{-100.0, 0.0}, {100.0, 0.0}},
            {
                .stroke = {225U, 232U, 244U, 255U},
                .fill = std::nullopt,
                .stroke_width = 8.0,
            })),
        document_);
    current_tool_ = Tool::select;
    sync_toolbar();

    const Vec2d center = camera_.world_to_screen({0.0, 0.0});
    bool running = true;
    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = static_cast<float>(center.x);
    down.button.y = static_cast<float>(center.y);
    handle_event(down, running);

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = static_cast<float>(center.x + 60.0);
    motion.motion.y = static_cast<float>(center.y + 35.0);
    handle_event(motion, running);

    const Object* const transient_object = document_.find(line_id);
    if (transient_object == nullptr
        || std::get<Line>(transient_object->geometry).start
            != Vec2d{-100.0, 0.0}
        || !selection_preview_.has_value()
        || selection_preview_->transform.translation
            != Vec2d{60.0, 35.0}) {
        log::write(
            log::Level::error,
            "Selection drag mutated the document before release");
        return 1;
    }
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        nullptr,
        toolbar_,
        selection_,
        nullptr,
        &*selection_preview_));

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.x = motion.motion.x;
    up.button.y = motion.motion.y;
    handle_event(up, running);

    const Object* const moved_object = document_.find(line_id);
    if (moved_object == nullptr
        || std::get<Line>(moved_object->geometry).start
            != Vec2d{-40.0, 35.0}
        || history_.size() != 2U) {
        log::write(log::Level::error, "Selection move was not one command");
        return 1;
    }

    activate_ui_action(UiAction::color_blue);
    const Object* const restyled_object = document_.find(line_id);
    if (restyled_object == nullptr
        || restyled_object->style.stroke
            != Toolbar::color_for(UiAction::color_blue)
        || history_.size() != 3U) {
        log::write(
            log::Level::error,
            "Selection appearance change was not one command");
        return 1;
    }

    const Color custom_color{34U, 156U, 126U, 255U};
    toolbar_.begin_custom_color(
        CustomColorTarget::stroke, custom_color);
    activate_ui_action(UiAction::custom_color_done);
    const Object* const custom_restyled_object = document_.find(line_id);
    if (custom_restyled_object == nullptr
        || custom_restyled_object->style.stroke != custom_color
        || history_.size() != 4U) {
        log::write(
            log::Level::error,
            "Custom selection color was not one command");
        return 1;
    }

    SDL_Event duplicate{};
    duplicate.type = SDL_EVENT_KEY_DOWN;
    duplicate.key.scancode = SDL_SCANCODE_D;
    duplicate.key.mod = SDL_KMOD_CTRL;
    handle_event(duplicate, running);
    if (document_.size() != 2U || selection_.ids().size() != 1U
        || history_.size() != 5U) {
        log::write(log::Level::error, "Selection duplicate failed");
        return 1;
    }

    SDL_Event remove{};
    remove.type = SDL_EVENT_KEY_DOWN;
    remove.key.scancode = SDL_SCANCODE_DELETE;
    handle_event(remove, running);
    if (document_.size() != 1U || history_.size() != 6U) {
        log::write(log::Level::error, "Selection delete failed");
        return 1;
    }
    undo_or_redo(false);
    if (document_.size() != 2U) {
        log::write(log::Level::error, "Selection edit undo failed");
        return 1;
    }
    return run_frame_test(1U);
}

int Application::run_large_board_test()
{
    constexpr std::size_t object_count = 100'000U;
    constexpr std::size_t clustered_count = 3'000U;
    constexpr std::size_t stroke_point_count = 1'000'000U;
    const auto fixture_start = std::chrono::steady_clock::now();
    std::uint64_t random_state = 0x8A5CD789635D2DFFULL;
    auto next_random = [&]() {
        random_state = random_state * 6364136223846793005ULL + 1ULL;
        return random_state;
    };
    const Style style{
        .stroke = {82U, 145U, 244U, 255U},
        .fill = std::nullopt,
        .stroke_width = 3.0,
    };

    for (std::size_t index = 0U; index < object_count; ++index) {
        double x = 0.0;
        double y = 0.0;
        if (index < clustered_count) {
            x = -590.0 + static_cast<double>(index % 60U) * 20.0;
            y = -270.0 + static_cast<double>(index / 60U) * 11.0;
        } else {
            x = static_cast<double>(next_random() % 1'900'000ULL) - 950'000.0;
            y = static_cast<double>(next_random() % 1'900'000ULL) - 950'000.0;
            if (std::abs(x) < 2'000.0 && std::abs(y) < 2'000.0) {
                x += 8'000.0;
            }
        }
        const ObjectId id = ObjectId::from_u64(index + 1U);
        Object object;
        switch (index % 4U) {
        case 0U:
            object = Object::make_line(
                id, static_cast<std::int64_t>(index),
                {{x, y}, {x + 12.0, y + 7.0}}, style);
            break;
        case 1U:
            object = Object::make_rectangle(
                id, static_cast<std::int64_t>(index),
                {{x, y}, {x + 14.0, y + 9.0}}, style);
            break;
        case 2U:
            object = Object::make_ellipse(
                id, static_cast<std::int64_t>(index),
                {{x, y}, {x + 16.0, y + 10.0}}, style);
            break;
        default:
            object = Object::make_stroke(
                id, static_cast<std::int64_t>(index),
                {{{x, y}, {x + 6.0, y + 5.0}, {x + 13.0, y + 2.0}}},
                style);
            break;
        }
        if (!document_.insert(std::move(object))) {
            log::write(log::Level::error, "Large-board fixture insertion failed");
            return 1;
        }
    }

    std::vector<Vec2d> long_points;
    long_points.reserve(stroke_point_count);
    for (std::size_t index = 0U; index < stroke_point_count; ++index) {
        const double fraction = static_cast<double>(index)
            / static_cast<double>(stroke_point_count - 1U);
        long_points.push_back({
            -999'000.0 + fraction * 1'998'000.0,
            std::sin(static_cast<double>(index) * 0.002) * 180.0,
        });
    }
    if (!document_.insert(Object::make_stroke(
            ObjectId::from_u64(object_count + 1U),
            static_cast<std::int64_t>(object_count),
            {std::move(long_points)},
            style))) {
        log::write(log::Level::error, "Long-stroke fixture insertion failed");
        return 1;
    }
    history_.mark_saved(document_);
    sync_toolbar();
    const auto fixture_end = std::chrono::steady_clock::now();

    if (document_.size() != object_count + 1U
        || document_.oversized_object_count() == 0U
        || document_.spatial_chunk_count() > object_count) {
        log::write(log::Level::error, "Large-board spatial index is unbounded");
        return 1;
    }

    if (run_frame_test(2U) != 0) {
        return 1;
    }
    double accumulated_build_ms = 0.0;
    constexpr std::size_t measured_frames = 20U;
    for (std::size_t frame = 0U; frame < measured_frames; ++frame) {
        if (!renderer_->render(camera_, document_, nullptr, toolbar_, selection_)) {
            --frame;
            SDL_Delay(1U);
            continue;
        }
        accumulated_build_ms += renderer_->stats().build_milliseconds;
    }
    const auto normal_stats = renderer_->stats();
    const DocumentMemoryStats document_memory =
        document_.memory_stats();
    const double average_build_ms =
        accumulated_build_ms / static_cast<double>(measured_frames);
    const std::size_t retained_geometry_capacity =
        normal_stats.cpu_geometry_capacity_bytes
        + normal_stats.gpu_geometry_capacity_bytes
        + normal_stats.transfer_capacity_bytes;
#if defined(NDEBUG)
    constexpr double maximum_average_build_ms = 2.0;
#else
    constexpr double maximum_average_build_ms = 25.0;
#endif
    if (normal_stats.visible_objects < clustered_count
        || normal_stats.visible_objects > 10'000U
        || normal_stats.tessellated_objects != 0U
        || normal_stats.scene_rebuilt
        || normal_stats.scene_upload_bytes != 0U
        || normal_stats.cache_bytes > 300U * 1024U * 1024U
        || retained_geometry_capacity > 80U * 1024U * 1024U
        || average_build_ms > maximum_average_build_ms) {
        log::write(log::Level::error, "Large-board warm-frame regression");
        return 1;
    }

    zoom_at_viewport_center(1.0e-9);
    const int zoom_out_result = run_frame_test(1U);
    if (zoom_out_result != 0
        || renderer_->stats().visible_objects < clustered_count
        || std::abs(camera_.zoom() - camera_.minimum_zoom()) > 1.0e-12) {
        std::ostringstream error;
        error << "Extreme zoom-out regression: result=" << zoom_out_result
              << ", visible=" << renderer_->stats().visible_objects
              << ", zoom=" << camera_.zoom();
        log::write(log::Level::error, error.str());
        return 1;
    }
    zoom_at_viewport_center(1.0 / camera_.zoom());
    if (run_frame_test(1U) != 0) {
        return 1;
    }

    const double fixture_ms = std::chrono::duration<double, std::milli>(
        fixture_end - fixture_start).count();
    std::ostringstream summary;
    summary
        << "large-board objects=" << document_.size()
        << ", points=" << stroke_point_count
        << ", visible=" << normal_stats.visible_objects
        << ", average-build-ms=" << average_build_ms
        << ", fixture-ms=" << fixture_ms
        << ", cache-bytes=" << normal_stats.cache_bytes
        << ", document-bytes=" << document_memory.total_bytes
        << ", spatial-index-bytes="
        << document_memory.spatial_index_bytes
        << ", retained-geometry-capacity="
        << retained_geometry_capacity
        << ", cpu-geometry-live="
        << normal_stats.cpu_geometry_live_bytes
        << ", cpu-geometry-capacity="
        << normal_stats.cpu_geometry_capacity_bytes
        << ", gpu-geometry-capacity="
        << normal_stats.gpu_geometry_capacity_bytes
        << ", transfer-capacity="
        << normal_stats.transfer_capacity_bytes
        << ", text-geometry-capacity="
        << normal_stats.text_geometry_capacity_bytes
        << ", query-scratch-capacity="
        << normal_stats.query_scratch_capacity_bytes
        << ", history-bytes=" << history_.retained_bytes()
        << ", vertices=" << normal_stats.emitted_vertices
        << ", upload-bytes="
        << normal_stats.scene_upload_bytes
            + normal_stats.geometry_upload_bytes
        << ", query-ms=" << normal_stats.query_milliseconds
        << ", objects-ms=" << normal_stats.objects_milliseconds
        << ", staging-ms=" << normal_stats.staging_milliseconds
        << ", swapchain-wait-ms=" << normal_stats.swapchain_wait_milliseconds
        << ", record-ms=" << normal_stats.command_record_milliseconds
        << ", submit-ms=" << normal_stats.submit_milliseconds
        << ", total-cpu-ms=" << normal_stats.total_cpu_milliseconds
        << ", draw-calls="
        << normal_stats.draw_calls + normal_stats.text_draw_calls;
    log::write(log::Level::info, summary.str());
    if (!renderer_->render(
            camera_,
            document_,
            nullptr,
            toolbar_,
            selection_,
            &home_view_)
        || renderer_->stats().cache_bytes != 0U
        || renderer_->stats().cache_entries != 0U
        || renderer_->stats().query_scratch_capacity_bytes != 0U) {
        log::write(
            log::Level::error,
            "Home transition did not release retained board memory");
        return 1;
    }
    return 0;
}

int Application::run_drawing_performance_test()
{
    constexpr std::size_t batch_count = 24U;
    constexpr std::size_t points_per_batch = 1'000U;
    constexpr std::size_t total_points =
        batch_count * points_per_batch + 2U;
    Stroke stroke;
    stroke.points.reserve(total_points);
    stroke.points.push_back({-500.0, 0.0});
    stroke.points.push_back({
        -499.96,
        std::sin(0.012) * 80.0,
    });
    active_draft_ = ObjectDraft{
        .geometry = std::move(stroke),
        .style = {
            .stroke = {38U, 104U, 220U, 255U},
            .fill = std::nullopt,
            .stroke_width = 3.0,
        },
        .generation = ++next_draft_generation_,
        .revision = 1U,
    };
    sync_toolbar();

    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    if (renderer_->stats().draft_full_rebuilds != 1U) {
        log::write(
            log::Level::error,
            "Initial live-stroke mesh was not built exactly once");
        return 1;
    }

    double accumulated_overlay_ms = 0.0;
    double maximum_overlay_ms = 0.0;
    std::size_t accumulated_draft_upload_bytes = 0U;
    std::size_t maximum_draft_upload_bytes = 0U;
    for (std::size_t batch = 0U; batch < batch_count; ++batch) {
        auto& points =
            std::get<Stroke>(active_draft_->geometry).points;
        for (std::size_t offset = 0U; offset < points_per_batch; ++offset) {
            const std::size_t index =
                batch * points_per_batch + offset + 2U;
            const double x = -500.0
                + static_cast<double>(index) * 0.04;
            const double y =
                std::sin(static_cast<double>(index) * 0.012) * 80.0;
            points.push_back({x, y});
        }
        ++active_draft_->revision;
        static_cast<void>(renderer_->render(
            camera_,
            document_,
            &*active_draft_,
            toolbar_,
            selection_));
        const RendererStats stats = renderer_->stats();
        if (stats.draft_full_rebuilds != 0U
            || stats.draft_appended_points != points_per_batch) {
            log::write(
                log::Level::error,
                "Live stroke rebuilt old geometry instead of extending it");
            return 1;
        }
        constexpr std::size_t maximum_tail_upload_bytes =
            points_per_batch * 1'024U;
        if (stats.draft_upload_bytes == 0U
            || stats.draft_upload_bytes > maximum_tail_upload_bytes) {
            log::write(
                log::Level::error,
                "Live stroke uploaded more than its newly appended tail");
            return 1;
        }
        accumulated_draft_upload_bytes += stats.draft_upload_bytes;
        maximum_draft_upload_bytes = std::max(
            maximum_draft_upload_bytes, stats.draft_upload_bytes);
        accumulated_overlay_ms += stats.overlay_ui_milliseconds;
        maximum_overlay_ms = std::max(
            maximum_overlay_ms, stats.overlay_ui_milliseconds);
    }

    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    const RendererStats settled = renderer_->stats();
    const auto* const settled_stroke =
        std::get_if<Stroke>(&active_draft_->geometry);
    const std::size_t draft_point_capacity_bytes =
        settled_stroke != nullptr
        ? settled_stroke->points.capacity() * sizeof(Vec2d)
        : 0U;
    const std::size_t retained_geometry_capacity =
        settled.cpu_geometry_capacity_bytes
        + settled.gpu_geometry_capacity_bytes
        + settled.transfer_capacity_bytes;
    if (settled.draft_full_rebuilds != 0U
        || settled.draft_appended_points != 0U
        || settled.scene_rebuilt
        || settled.scene_upload_bytes != 0U
        || settled.draft_upload_bytes != 0U
        || retained_geometry_capacity > 80U * 1024U * 1024U) {
        log::write(
            log::Level::error,
            "Unchanged live-stroke frame repeated retained work");
        return 1;
    }

    const double average_overlay_ms =
        accumulated_overlay_ms / static_cast<double>(batch_count);
#if defined(NDEBUG)
    constexpr double maximum_average_overlay_ms = 4.0;
#else
    constexpr double maximum_average_overlay_ms = 40.0;
#endif
    if (average_overlay_ms > maximum_average_overlay_ms) {
        log::write(
            log::Level::error,
            "Live-stroke overlay exceeded its CPU budget");
        return 1;
    }

    std::ostringstream summary;
    summary
        << "live-drawing points=" << total_points
        << ", average-overlay-ms=" << average_overlay_ms
        << ", maximum-overlay-ms=" << maximum_overlay_ms
        << ", vertices=" << settled.emitted_vertices
        << ", average-draft-upload-bytes="
        << accumulated_draft_upload_bytes / batch_count
        << ", maximum-draft-upload-bytes="
        << maximum_draft_upload_bytes
        << ", overlay-upload-bytes=" << settled.geometry_upload_bytes
        << ", retained-geometry-capacity="
        << retained_geometry_capacity
        << ", draft-point-capacity="
        << draft_point_capacity_bytes
        << ", total-cpu-ms=" << settled.total_cpu_milliseconds;
    log::write(log::Level::info, summary.str());
    active_draft_.reset();
    return 0;
}

int Application::run_buffer_growth_test()
{
    constexpr std::size_t growth_points = 90'000U;
    constexpr std::size_t tail_points = 1'000U;
    Stroke stroke;
    stroke.points.reserve(growth_points + tail_points + 2U);
    stroke.points.push_back({-500.0, 0.0});
    stroke.points.push_back({-499.98, 1.0});
    active_draft_ = ObjectDraft{
        .geometry = std::move(stroke),
        .style = {
            .stroke = {38U, 104U, 220U, 255U},
            .fill = std::nullopt,
            .stroke_width = 3.0,
        },
        .generation = ++next_draft_generation_,
        .revision = 1U,
    };
    sync_toolbar();
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    const std::size_t initial_gpu_capacity =
        renderer_->stats().gpu_geometry_capacity_bytes;

    const auto append_points = [&](const std::size_t count) {
        auto& points =
            std::get<Stroke>(active_draft_->geometry).points;
        const std::size_t first = points.size();
        for (std::size_t offset = 0U; offset < count; ++offset) {
            const std::size_t index = first + offset;
            points.push_back({
                -500.0 + static_cast<double>(index) * 0.02,
                std::sin(static_cast<double>(index) * 0.01) * 80.0,
            });
        }
        ++active_draft_->revision;
    };

    append_points(growth_points);
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    const RendererStats growth = renderer_->stats();
#if defined(NDEBUG)
    constexpr double maximum_growth_frame_ms = 30.0;
#else
    constexpr double maximum_growth_frame_ms = 300.0;
#endif
    if (growth.draft_full_rebuilds != 0U
        || growth.draft_appended_points != growth_points
        || growth.gpu_geometry_capacity_bytes <= initial_gpu_capacity
        || growth.total_cpu_milliseconds > maximum_growth_frame_ms) {
        std::ostringstream error;
        error
            << "Geometry buffer growth regression: rebuilds="
            << growth.draft_full_rebuilds
            << ", appended=" << growth.draft_appended_points
            << ", initial-gpu-capacity=" << initial_gpu_capacity
            << ", grown-gpu-capacity="
            << growth.gpu_geometry_capacity_bytes
            << ", frame-ms=" << growth.total_cpu_milliseconds;
        log::write(
            log::Level::error,
            error.str());
        return 1;
    }

    append_points(tail_points);
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    const RendererStats tail = renderer_->stats();
    if (tail.draft_full_rebuilds != 0U
        || tail.draft_appended_points != tail_points
        || tail.gpu_geometry_capacity_bytes
            != growth.gpu_geometry_capacity_bytes
        || tail.draft_upload_bytes == 0U
        || tail.draft_upload_bytes > tail_points * 1'024U) {
        log::write(
            log::Level::error,
            "Post-growth draft update did not return to tail uploads");
        return 1;
    }

    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    if (renderer_->stats().draft_upload_bytes != 0U) {
        log::write(
            log::Level::error,
            "Settled grown draft repeated its upload");
        return 1;
    }

    // A valid gesture can retain substantially more source points than one
    // frame's GPU vertex budget. It must remain drawable through bounded
    // visual sampling instead of terminating the application.
    constexpr std::size_t oversized_tail_points = 300'000U;
    append_points(oversized_tail_points);
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    const RendererStats oversized = renderer_->stats();
    if (oversized.draft_full_rebuilds != 1U
        || oversized.draft_upload_bytes == 0U) {
        log::write(
            log::Level::error,
            "Oversized live stroke was not rebuilt with bounded sampling");
        return 1;
    }
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        &*active_draft_,
        toolbar_,
        selection_));
    if (renderer_->stats().draft_upload_bytes != 0U) {
        log::write(
            log::Level::error,
            "Settled oversized draft repeated its upload");
        return 1;
    }

    std::ostringstream summary;
    summary
        << "buffer-growth points="
        << growth_points + tail_points + oversized_tail_points + 2U
        << ", growth-frame-ms=" << growth.total_cpu_milliseconds
        << ", grown-gpu-capacity="
        << growth.gpu_geometry_capacity_bytes
        << ", post-growth-tail-upload="
        << tail.draft_upload_bytes;
    log::write(log::Level::info, summary.str());
    active_draft_.reset();

    // A transformed selection is rendered in the shared overlay rather than
    // the retained scene. One valid, dense stroke can exceed that overlay's
    // frame budget; the edit preview should shed detail without failing.
    constexpr std::size_t selection_points = 120'000U;
    Stroke selection_stroke;
    selection_stroke.points.reserve(selection_points);
    for (std::size_t point = 0U; point < selection_points; ++point) {
        selection_stroke.points.push_back({
            -500.0 + 1'000.0 * static_cast<double>(point)
                / static_cast<double>(selection_points - 1U),
            point % 2U == 0U ? -60.0 : 60.0,
        });
    }
    const ObjectId selection_id = ObjectId::random();
    Object selected = Object::make_stroke(
        selection_id,
        document_.next_z_order(),
        std::move(selection_stroke));
    SelectionPreview dense_selection{
        {selection_id},
        {},
        selected,
    };
    static_cast<void>(document_.insert(std::move(selected)));
    selection_.select(selection_id);
    static_cast<void>(renderer_->render(
        camera_,
        document_,
        nullptr,
        toolbar_,
        selection_,
        nullptr,
        &dense_selection));
    selection_.clear();
    document_.clear();
    return 0;
}

int Application::run_zoom_state_test()
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("sawer-zoom-test-" + ObjectId::random().to_string());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    recent_files_ = std::make_unique<RecentFiles>(
        directory / "recent-files.json", 20U);

    bool passed = true;
    const auto check_zoom =
        [this, &passed](const double expected, const std::string_view context) {
            if (std::abs(camera_.zoom() - expected) <= 1.0e-9) {
                return;
            }
            passed = false;
            std::ostringstream message;
            message << context << ": expected zoom " << expected
                    << ", got " << camera_.zoom();
            log::write(log::Level::error, message.str());
        };

    const auto first = directory / "First.sawer";
    const auto second = directory / "Second.sawer";
    try {
        set_zoom(2.0);
        new_board();
        check_zoom(1.0, "New board did not start at 100%");

        save_as(first);
        set_zoom(2.0);
        new_board();
        check_zoom(1.0, "New board inherited the previous board zoom");

        save_as(second);
        set_zoom(0.5);

        open_board(first);
        check_zoom(2.0, "First board zoom was not restored");
        open_board(second);
        check_zoom(0.5, "Second board zoom was not restored");

        new_board();
        check_zoom(1.0, "Later new board did not start at 100%");

        const RecentFiles reloaded{
            directory / "recent-files.json", 20U};
        const auto first_zoom = reloaded.zoom(first);
        const auto second_zoom = reloaded.zoom(second);
        if (!first_zoom.has_value() || !second_zoom.has_value()
            || std::abs(*first_zoom - 2.0) > 1.0e-9
            || std::abs(*second_zoom - 0.5) > 1.0e-9) {
            passed = false;
            log::write(
                log::Level::error,
                "Per-board zoom values did not survive preferences reload");
        }
    } catch (const std::exception& exception) {
        passed = false;
        log::write(
            log::Level::error,
            "Per-board zoom regression failed: "
                + std::string{exception.what()});
    }

    board_file_.reset();
    document_.clear();
    std::filesystem::remove_all(directory, error);
    return passed ? 0 : 1;
}

std::string_view Application::gpu_diagnostics() const noexcept
{
    return renderer_->diagnostics();
}

void Application::new_board()
{
    if (dialog_active_ || background_open_.valid()) {
        status_error_ = "Finish the current file operation first.";
        sync_toolbar();
        return;
    }
    if (!confirm_unsaved_untitled({
            PendingUnsavedActionKind::new_board, {}, 0U})) {
        return;
    }
    if (!flush_board_safely()) {
        return;
    }
    remember_current_board_zoom();
    cancel_gesture();
    cancel_selection_gesture();
    selection_.clear();
    board_file_.reset();
    untitled_name_ = "Untitled";
    document_.clear();
    history_ = {};
    history_.mark_saved(document_);
    set_zoom(1.0);
    ++lifecycle_generation_;
    status_error_.clear();
    update_window_title();
}

void Application::open_board(const std::filesystem::path& path)
{
    if (has_unsaved_untitled()) {
        throw std::runtime_error{
            "Save the Untitled board before opening another board"};
    }
    flush_board();
    cancel_gesture();
    cancel_selection_gesture();
    selection_.clear();

    Document loaded;
    bool recovered_final_line = false;
    auto session =
        BoardFileSession::open(path, loaded, recovered_final_line);

    remember_current_board_zoom();
    document_ = std::move(loaded);
    history_ = {};
    history_.mark_saved(document_);
    board_file_ = std::move(session);
    ++lifecycle_generation_;
    if (recent_files_) {
        recent_files_->touch(path);
    }
    restore_board_zoom(path);
    status_error_.clear();
    update_window_title();

    if (recovered_final_line) {
        log::write(
            log::Level::warning,
            "Recovered an interrupted final record and compacted the board");
    }
}

void Application::save_as(const std::filesystem::path& path)
{
    flush_board();
    remember_current_board_zoom();
    board_file_ = BoardFileSession::create(path, document_);
    ++lifecycle_generation_;
    if (recent_files_) {
        recent_files_->touch(path);
        recent_files_->set_zoom(path, camera_.zoom());
    }
    status_error_.clear();
    history_.mark_saved(document_);
    update_window_title();
    synchronize_navigation_target();
}

void Application::start_session()
{
    // A board opened from the command line wins. Otherwise restore the most
    // recently used file when it is still available, then fall back to Home.
    if (board_file_.has_value()) {
        view_mode_ = ViewMode::board;
        reset_navigation_history();
        return;
    }
    if (recent_files_ && !recent_files_->entries().empty()) {
        const auto& last = recent_files_->entries().front();
        std::error_code error;
        if (std::filesystem::is_regular_file(last, error)) {
            try {
                open_board(last);
                view_mode_ = ViewMode::board;
                reset_navigation_history();
                return;
            } catch (const std::exception& exception) {
                status_error_ =
                    "Could not restore the last board: "
                    + std::string{exception.what()};
                log::write(
                    log::Level::warning,
                    status_error_);
            }
        }
    }
    view_mode_ = ViewMode::home;
    refresh_home();
    reset_navigation_history();
}

bool Application::has_unsaved_untitled() const noexcept
{
    return !board_file_.has_value()
        && (document_.dirty() || untitled_name_ != "Untitled");
}

bool Application::confirm_unsaved_untitled(PendingUnsavedAction action)
{
    if (!has_unsaved_untitled()) {
        return true;
    }

    pending_unsaved_action_ = std::move(action);
    if (next_unsaved_choice_for_test_.has_value()) {
        const UnsavedDialogChoice choice =
            *next_unsaved_choice_for_test_;
        next_unsaved_choice_for_test_.reset();
        if (choice == UnsavedDialogChoice::discard) {
            pending_unsaved_action_.reset();
            discard_unsaved_untitled();
            return true;
        }
        if (choice == UnsavedDialogChoice::cancel) {
            pending_unsaved_action_.reset();
            return false;
        }
    }

    cancel_gesture();
    cancel_selection_gesture();
    toolbar_.clear_pointer();
    unsaved_dialog_.open(
        camera_.viewport().x,
        camera_.viewport().y,
        display_scale_,
        untitled_name_);
    update_cursor();
    return false;
}

void Application::handle_unsaved_dialog_choice(
    const UnsavedDialogChoice choice)
{
    unsaved_dialog_.close();
    switch (choice) {
    case UnsavedDialogChoice::save:
        show_board_dialog(true);
        break;
    case UnsavedDialogChoice::discard:
        discard_unsaved_untitled();
        resume_pending_unsaved_action();
        break;
    case UnsavedDialogChoice::cancel:
        pending_unsaved_action_.reset();
        quit_requested_ = false;
        break;
    case UnsavedDialogChoice::count:
        pending_unsaved_action_.reset();
        quit_requested_ = false;
        break;
    }
    update_cursor();
}

void Application::discard_unsaved_untitled()
{
    cancel_gesture();
    cancel_selection_gesture();
    selection_.clear();
    board_file_.reset();
    untitled_name_ = "Untitled";
    document_.clear();
    history_ = {};
    history_.mark_saved(document_);
    ++lifecycle_generation_;
    status_error_.clear();
    update_window_title();
}

void Application::resume_pending_unsaved_action()
{
    if (!pending_unsaved_action_.has_value()) {
        return;
    }
    PendingUnsavedAction action =
        std::move(*pending_unsaved_action_);
    pending_unsaved_action_.reset();

    switch (action.kind) {
    case PendingUnsavedActionKind::new_board:
        new_board();
        if (status_error_.empty()) {
            view_mode_ = ViewMode::board;
            sync_toolbar();
            record_navigation_target();
        }
        break;
    case PendingUnsavedActionKind::home:
        enter_home();
        break;
    case PendingUnsavedActionKind::open_path:
        begin_background_open(action.path);
        break;
    case PendingUnsavedActionKind::navigate:
        if (restore_navigation_target(action.navigation_index)) {
            navigation_history_index_ = action.navigation_index;
        }
        break;
    case PendingUnsavedActionKind::quit:
        quit_requested_ = true;
        break;
    case PendingUnsavedActionKind::none:
        break;
    }
}

void Application::enter_home()
{
    if (dialog_active_ || background_open_.valid()) {
        status_error_ = "Finish the current file operation first.";
        sync_toolbar();
        return;
    }
    if (!confirm_unsaved_untitled({
            PendingUnsavedActionKind::home, {}, 0U})) {
        return;
    }
    if (!flush_board_safely()) {
        return;
    }
    remember_current_board_zoom();
    cancel_gesture();
    cancel_selection_gesture();
    selection_.clear();
    ui_pointer_down_ = false;
    title_pointer_down_ = false;
    left_button_down_ = false;
    middle_button_down_ = false;
    hand_dragging_ = false;
    space_down_ = false;
    shift_down_ = false;
    left_shift_down_ = false;
    right_shift_down_ = false;
    color_drag_action_.reset();
    toolbar_.clear_pointer();
    toolbar_.clear_focus();
    board_file_.reset();
    document_.clear();
    history_ = {};
    history_.mark_saved(document_);
    ++lifecycle_generation_;
    view_mode_ = ViewMode::home;
    home_view_.clear_pointer();
    refresh_home();
    update_cursor();
    update_window_title();
    record_navigation_target();
}

void Application::reset_navigation_history()
{
    navigation_history_.clear();
    navigation_history_.push_back({
        view_mode_,
        view_mode_ == ViewMode::board && board_file_.has_value()
            ? board_file_->path()
            : std::filesystem::path{},
    });
    navigation_history_index_ = 0U;
}

void Application::synchronize_navigation_target()
{
    if (replaying_navigation_) {
        return;
    }
    if (navigation_history_.empty()
        || navigation_history_index_ >= navigation_history_.size()) {
        reset_navigation_history();
        return;
    }
    navigation_history_[navigation_history_index_] = {
        view_mode_,
        view_mode_ == ViewMode::board && board_file_.has_value()
            ? board_file_->path()
            : std::filesystem::path{},
    };
}

void Application::record_navigation_target()
{
    if (replaying_navigation_) {
        return;
    }
    const NavigationTarget target{
        view_mode_,
        view_mode_ == ViewMode::board && board_file_.has_value()
            ? board_file_->path()
            : std::filesystem::path{},
    };
    if (navigation_history_.empty()) {
        navigation_history_.push_back(target);
        navigation_history_index_ = 0U;
        return;
    }
    if (navigation_history_[navigation_history_index_] == target) {
        return;
    }

    navigation_history_.erase(
        navigation_history_.begin()
            + static_cast<std::ptrdiff_t>(navigation_history_index_ + 1U),
        navigation_history_.end());
    navigation_history_.push_back(target);
    navigation_history_index_ = navigation_history_.size() - 1U;

    constexpr std::size_t maximum_navigation_entries = 64U;
    if (navigation_history_.size() > maximum_navigation_entries) {
        navigation_history_.erase(navigation_history_.begin());
        --navigation_history_index_;
    }
}

bool Application::restore_navigation_target(const std::size_t index)
{
    if (index >= navigation_history_.size()
        || dialog_active_
        || background_open_.valid()) {
        return false;
    }
    if (!confirm_unsaved_untitled({
            PendingUnsavedActionKind::navigate, {}, index})) {
        return false;
    }

    const NavigationTarget target = navigation_history_[index];
    replaying_navigation_ = true;
    bool restored = false;
    try {
        if (target.view == ViewMode::home) {
            enter_home();
            restored = view_mode_ == ViewMode::home;
        } else if (target.board_path.empty()) {
            new_board();
            if (status_error_.empty()) {
                view_mode_ = ViewMode::board;
                sync_toolbar();
                restored = true;
            }
        } else {
            open_board(target.board_path);
            view_mode_ = ViewMode::board;
            sync_toolbar();
            restored = true;
        }
    } catch (const std::exception& error) {
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        if (view_mode_ == ViewMode::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
    }
    replaying_navigation_ = false;
    return restored;
}

void Application::navigate_history(const int direction)
{
    if (direction == 0 || navigation_history_.empty()) {
        return;
    }
    synchronize_navigation_target();
    const std::size_t target_index = direction < 0
        ? (navigation_history_index_ == 0U
               ? navigation_history_index_
               : navigation_history_index_ - 1U)
        : std::min(
              navigation_history_index_ + 1U,
              navigation_history_.size() - 1U);
    if (target_index == navigation_history_index_) {
        return;
    }
    if (restore_navigation_target(target_index)) {
        navigation_history_index_ = target_index;
    }
}

void Application::refresh_home()
{
    std::vector<HomeBoard> boards;
    if (recent_files_) {
        boards.reserve(recent_files_->entries().size());
        std::vector<std::string> active_preview_keys;
        active_preview_keys.reserve(recent_files_->entries().size());
        for (const auto& path : recent_files_->entries()) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) {
                continue;
            }
            const auto modified = std::filesystem::last_write_time(path, error);
            if (error) {
                continue;
            }
            const std::uintmax_t file_size =
                std::filesystem::file_size(path, error);
            if (error) {
                continue;
            }

            // A small disk cache is checked synchronously; full board replay
            // and rasterization happen only on a background cache miss.
            std::shared_ptr<const BoardPreview> preview;
            const std::string key = path.string();
            active_preview_keys.push_back(key);
            const auto cached = preview_cache_.find(key);
            if (cached != preview_cache_.end()
                && cached->second.modified == modified
                && cached->second.file_size == file_size) {
                preview = cached->second.preview;
            } else {
                if (auto disk_preview = load_preview_cache(
                        preview_cache_directory_,
                        path,
                        modified,
                        file_size)) {
                    preview = std::make_shared<const BoardPreview>(
                        std::move(*disk_preview));
                    preview_cache_[key] =
                        PreviewCacheEntry{modified, file_size, preview};
                } else if (!preview_tasks_.contains(key)
                           && preview_tasks_.size()
                               < maximum_background_preview_tasks) {
                    const auto cache_directory = preview_cache_directory_;
                    preview_tasks_.emplace(
                        key,
                        PreviewTask{
                            modified,
                            file_size,
                            std::async(
                                std::launch::async,
                                [path,
                                 modified,
                                 file_size,
                                 cache_directory]() {
                                    BoardPreview built =
                                        Application::build_board_preview(path);
                                    store_preview_cache(
                                        cache_directory,
                                        path,
                                        modified,
                                        file_size,
                                        built);
                                    return built;
                                }),
                        });
                }
            }
            const bool editing =
                renaming_ && rename_target_ == RenameTarget::home
                && path == rename_path_;
            boards.push_back(HomeBoard{
                editing ? rename_text_ : path.stem().string(),
                format_modified_time(modified),
                std::move(preview),
                path,
                editing,
                editing ? rename_cursor_ : 0U,
                editing ? rename_anchor_ : 0U,
            });
        }
        std::erase_if(
            preview_cache_,
            [&](const auto& entry) {
                return std::ranges::find(
                           active_preview_keys, entry.first)
                    == active_preview_keys.end();
            });
    }
    home_view_.update(
        camera_.viewport().x,
        camera_.viewport().y,
        display_scale_,
        toolbar_.theme(),
        std::move(boards),
        status_error_);
}

BoardPreview Application::build_board_preview(
    const std::filesystem::path& path)
{
    const Document document = read_board_preview(path);
    return rasterize_board_preview(document);
}

void Application::open_board_from_home(const std::size_t index)
{
    if (dialog_active_ || background_open_.valid()) {
        status_error_ = "Finish the current file operation first.";
        refresh_home();
        return;
    }
    const auto& boards = home_view_.boards();
    if (index >= boards.size() || boards[index].path.empty()) {
        return;
    }
    // Open synchronously so the board is fully loaded before the view switches.
    // A background open would leave a window where the first stroke is either
    // eaten by the still-showing home screen or drawn into a document that the
    // pending load then discards.
    try {
        open_board(boards[index].path);
        view_mode_ = ViewMode::board;
        sync_toolbar();
        record_navigation_target();
    } catch (const std::exception& error) {
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        refresh_home();
    }
}

void Application::start_rename_current()
{
    if (renaming_ || dialog_active_
        || background_open_.valid()) {
        return;
    }
    cancel_gesture();
    cancel_selection_gesture();
    rename_target_ = RenameTarget::board;
    rename_text_ = board_file_.has_value()
        ? board_file_->path().stem().string()
        : untitled_name_;
    rename_anchor_ = 0U;
    rename_cursor_ = rename_text_.size();
    renaming_ = true;
    static_cast<void>(SDL_StartTextInput(window_.get()));
    sync_toolbar();
}

void Application::start_rename_home(const std::size_t index)
{
    if (renaming_ || dialog_active_ || background_open_.valid()
        || index >= home_view_.boards().size()
        || home_view_.boards()[index].path.empty()) {
        return;
    }
    rename_path_ = home_view_.boards()[index].path;
    rename_target_ = RenameTarget::home;
    rename_text_ = home_view_.boards()[index].name;
    rename_anchor_ = 0U;
    rename_cursor_ = rename_text_.size();
    renaming_ = true;
    static_cast<void>(SDL_StartTextInput(window_.get()));
    refresh_home();
}

void Application::commit_rename()
{
    if (!renaming_) {
        return;
    }
    const RenameTarget target = rename_target_;
    const std::string name = sanitize_board_name(rename_text_);
    static_cast<void>(SDL_StopTextInput(window_.get()));
    renaming_ = false;
    rename_pointer_selecting_ = false;
    rename_target_ = RenameTarget::none;
    rename_text_.clear();
    rename_cursor_ = 0U;
    rename_anchor_ = 0U;

    try {
        if (target == RenameTarget::board) {
            if (board_file_.has_value()) {
                if (name != board_file_->path().stem().string()) {
                    flush_board();
                    const auto old_path = board_file_->path();
                    const auto new_path =
                        unique_board_path(old_path.parent_path(), name);
                    board_file_->rename(new_path);
                    if (recent_files_) {
                        recent_files_->replace(old_path, new_path);
                    }
                    for (auto& navigation_target : navigation_history_) {
                        if (navigation_target.board_path == old_path) {
                            navigation_target.board_path = new_path;
                        }
                    }
                    ++lifecycle_generation_;
                }
            } else {
                untitled_name_ = name;
            }
            status_error_.clear();
            update_window_title();
            synchronize_navigation_target();
        } else if (target == RenameTarget::home) {
            status_error_.clear();
            if (std::filesystem::exists(rename_path_)
                && name != rename_path_.stem().string()) {
                const auto new_path =
                    unique_board_path(rename_path_.parent_path(), name);
                std::error_code error;
                std::filesystem::rename(rename_path_, new_path, error);
                if (error) {
                    status_error_ =
                        "Could not rename board: " + error.message();
                    log::write(
                        log::Level::error,
                        status_error_);
                } else {
                    if (recent_files_) {
                        recent_files_->replace(rename_path_, new_path);
                    }
                    for (auto& navigation_target : navigation_history_) {
                        if (navigation_target.board_path == rename_path_) {
                            navigation_target.board_path = new_path;
                        }
                    }
                    ++lifecycle_generation_;
                }
            }
            refresh_home();
        }
    } catch (const std::exception& error) {
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        if (target == RenameTarget::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
    }
    rename_path_.clear();
}

void Application::cancel_rename()
{
    if (!renaming_) {
        return;
    }
    const RenameTarget target = rename_target_;
    static_cast<void>(SDL_StopTextInput(window_.get()));
    renaming_ = false;
    rename_pointer_selecting_ = false;
    rename_target_ = RenameTarget::none;
    rename_text_.clear();
    rename_cursor_ = 0U;
    rename_anchor_ = 0U;
    rename_path_.clear();
    if (target == RenameTarget::home) {
        refresh_home();
    } else {
        sync_toolbar();
    }
}

bool Application::handle_rename_event(const SDL_Event& event)
{
    const auto refresh_editor = [this]() {
        if (rename_target_ == RenameTarget::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
    };
    const auto selection_start = [this]() {
        return std::min(rename_cursor_, rename_anchor_);
    };
    const auto selection_end = [this]() {
        return std::max(rename_cursor_, rename_anchor_);
    };
    const auto erase_selection = [this, &selection_start, &selection_end]() {
        const std::size_t first = selection_start();
        const std::size_t last = selection_end();
        if (first == last) return false;
        rename_text_.erase(first, last - first);
        rename_cursor_ = first;
        rename_anchor_ = first;
        return true;
    };
    const auto previous_codepoint = [this](std::size_t offset) {
        if (offset == 0U) return std::size_t{0U};
        --offset;
        while (offset > 0U
               && (static_cast<unsigned char>(rename_text_[offset]) & 0xC0U)
                   == 0x80U) {
            --offset;
        }
        return offset;
    };
    const auto next_codepoint = [this](std::size_t offset) {
        if (offset >= rename_text_.size()) return rename_text_.size();
        ++offset;
        while (offset < rename_text_.size()
               && (static_cast<unsigned char>(rename_text_[offset]) & 0xC0U)
                   == 0x80U) {
            ++offset;
        }
        return offset;
    };

    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT: {
        std::string insertion;
        for (const char* character = event.text.text;
             character != nullptr && *character != '\0'; ++character) {
            const char value = *character;
            const bool illegal = value == '\\' || value == '/'
                || value == ':' || value == '*' || value == '?'
                || value == '"' || value == '<' || value == '>'
                || value == '|'
                || static_cast<unsigned char>(value) < 0x20U;
            if (!illegal) insertion.push_back(value);
        }
        if (!insertion.empty()) {
            static_cast<void>(erase_selection());
            const std::size_t available =
                rename_text_.size() < board_name_max_bytes
                ? board_name_max_bytes - rename_text_.size()
                : 0U;
            if (insertion.size() > available) {
                std::size_t limit = available;
                while (limit > 0U && limit < insertion.size()
                       && (static_cast<unsigned char>(insertion[limit]) & 0xC0U)
                           == 0x80U) {
                    --limit;
                }
                insertion.resize(limit);
            }
            rename_text_.insert(rename_cursor_, insertion);
            rename_cursor_ += insertion.size();
            rename_anchor_ = rename_cursor_;
        }
        refresh_editor();
        return true;
    }
    case SDL_EVENT_KEY_DOWN: {
        const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0U;
        const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0U;
        if (control && event.key.scancode == SDL_SCANCODE_A) {
            rename_anchor_ = 0U;
            rename_cursor_ = rename_text_.size();
            refresh_editor();
            return true;
        }
        switch (event.key.scancode) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            commit_rename();
            return true;
        case SDL_SCANCODE_ESCAPE:
            cancel_rename();
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (!erase_selection() && rename_cursor_ > 0U) {
                const std::size_t previous = previous_codepoint(rename_cursor_);
                rename_text_.erase(previous, rename_cursor_ - previous);
                rename_cursor_ = previous;
                rename_anchor_ = previous;
            }
            refresh_editor();
            return true;
        case SDL_SCANCODE_DELETE:
            if (!erase_selection() && rename_cursor_ < rename_text_.size()) {
                const std::size_t next = next_codepoint(rename_cursor_);
                rename_text_.erase(rename_cursor_, next - rename_cursor_);
            }
            refresh_editor();
            return true;
        case SDL_SCANCODE_LEFT:
            if (!shift && selection_start() != selection_end()) {
                rename_cursor_ = selection_start();
            } else {
                rename_cursor_ = control
                    ? 0U
                    : previous_codepoint(rename_cursor_);
            }
            if (!shift) rename_anchor_ = rename_cursor_;
            refresh_editor();
            return true;
        case SDL_SCANCODE_RIGHT:
            if (!shift && selection_start() != selection_end()) {
                rename_cursor_ = selection_end();
            } else {
                rename_cursor_ = control
                    ? rename_text_.size()
                    : next_codepoint(rename_cursor_);
            }
            if (!shift) rename_anchor_ = rename_cursor_;
            refresh_editor();
            return true;
        case SDL_SCANCODE_HOME:
            rename_cursor_ = 0U;
            if (!shift) rename_anchor_ = rename_cursor_;
            refresh_editor();
            return true;
        case SDL_SCANCODE_END:
            rename_cursor_ = rename_text_.size();
            if (!shift) rename_anchor_ = rename_cursor_;
            refresh_editor();
            return true;
        default:
            // Swallow other keys so editing does not trigger tool shortcuts.
            return true;
        }
    }
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        // Clicking inside the title field positions the caret (and begins a
        // drag selection); clicking anywhere else commits the rename.
        if (event.button.button == SDL_BUTTON_LEFT && renderer_) {
            const Vec2d point{
                static_cast<double>(event.button.x),
                static_cast<double>(event.button.y),
            };
            bool inside_title = rename_target_ == RenameTarget::board
                && toolbar_.filename_bounds().contains(point);
            if (rename_target_ == RenameTarget::home) {
                for (std::size_t index = 0U;
                     index < home_view_.boards().size(); ++index) {
                    if (home_view_.boards()[index].path == rename_path_
                        && home_view_.board_name_bounds(index).contains(point)) {
                        inside_title = true;
                        break;
                    }
                }
            }
            if (inside_title) {
                if (const auto offset =
                        renderer_->filename_index_at_x(point.x)) {
                    rename_cursor_ = std::min(*offset, rename_text_.size());
                    rename_anchor_ = rename_cursor_;
                    rename_pointer_selecting_ = true;
                    refresh_editor();
                    return true;
                }
            }
        }
        commit_rename();
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (rename_pointer_selecting_ && renderer_) {
            if (const auto offset = renderer_->filename_index_at_x(
                    static_cast<double>(event.motion.x))) {
                // Extend the selection: move the caret, keep the anchor.
                rename_cursor_ = std::min(*offset, rename_text_.size());
                refresh_editor();
            }
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (rename_pointer_selecting_) {
            rename_pointer_selecting_ = false;
            return true;
        }
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        commit_rename();
        return false;
    default:
        return false;
    }
}

void Application::start_width_edit()
{
    if (width_editing_ || renaming_
        || current_tool_ == Tool::hand
        || (current_tool_ == Tool::select && selection_.empty())) {
        return;
    }

    width_editing_ = true;
    width_edit_replace_all_ = !toolbar_.context_stroke_width_mixed();
    width_edit_text_.clear();
    if (!toolbar_.context_stroke_width_mixed()) {
        std::array<char, 24> value{};
        static_cast<void>(std::snprintf(
            value.data(),
            value.size(),
            "%.1f",
            toolbar_.context_stroke_width()));
        width_edit_text_ = value.data();
    }
    static_cast<void>(SDL_StartTextInput(window_.get()));
    sync_toolbar();
}

void Application::commit_width_edit()
{
    if (!width_editing_) {
        return;
    }

    static_cast<void>(SDL_StopTextInput(window_.get()));
    const std::string text = width_edit_text_;
    width_editing_ = false;
    width_edit_replace_all_ = false;
    width_edit_text_.clear();

    try {
        std::size_t consumed = 0U;
        const double width = std::stod(text, &consumed);
        if (consumed == text.size() && std::isfinite(width)) {
            set_context_stroke_width(width);
            return;
        }
    } catch (const std::exception&) {
        // Invalid or empty input simply restores the previous width.
    }
    sync_toolbar();
}

void Application::cancel_width_edit()
{
    if (!width_editing_) {
        return;
    }
    static_cast<void>(SDL_StopTextInput(window_.get()));
    width_editing_ = false;
    width_edit_replace_all_ = false;
    width_edit_text_.clear();
    sync_toolbar();
}

bool Application::handle_width_edit_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT: {
        std::string insertion;
        bool decimal_present = !width_edit_replace_all_
            && width_edit_text_.find('.') != std::string::npos;
        for (const char* character = event.text.text;
             character != nullptr && *character != '\0'; ++character) {
            char value = *character;
            if (value == ',') value = '.';
            if (value >= '0' && value <= '9') {
                insertion.push_back(value);
            } else if (value == '.' && !decimal_present) {
                insertion.push_back(value);
                decimal_present = true;
            }
        }
        if (!insertion.empty()) {
            if (width_edit_replace_all_) {
                width_edit_text_.clear();
                width_edit_replace_all_ = false;
            }
            const std::size_t available =
                width_edit_text_.size() < 6U
                ? 6U - width_edit_text_.size()
                : 0U;
            width_edit_text_.append(insertion.substr(0U, available));
            sync_toolbar();
        }
        return true;
    }
    case SDL_EVENT_KEY_DOWN: {
        const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0U;
        if (control && event.key.scancode == SDL_SCANCODE_A) {
            width_edit_replace_all_ = true;
            sync_toolbar();
            return true;
        }
        switch (event.key.scancode) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            commit_width_edit();
            return true;
        case SDL_SCANCODE_ESCAPE:
            cancel_width_edit();
            return true;
        case SDL_SCANCODE_BACKSPACE:
            if (width_edit_replace_all_) {
                width_edit_text_.clear();
                width_edit_replace_all_ = false;
            } else if (!width_edit_text_.empty()) {
                width_edit_text_.pop_back();
            }
            sync_toolbar();
            return true;
        case SDL_SCANCODE_DELETE:
            width_edit_text_.clear();
            width_edit_replace_all_ = false;
            sync_toolbar();
            return true;
        default:
            // Text input supplies printable characters; swallow shortcuts.
            return true;
        }
    }
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.button != SDL_BUTTON_LEFT) {
            return false;
        }
        const UiControl* const field = toolbar_.find(UiAction::width_cycle);
        const Vec2d point{
            static_cast<double>(event.button.x),
            static_cast<double>(event.button.y),
        };
        if (field != nullptr && field->bounds.contains(point)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                width_edit_replace_all_ = true;
                sync_toolbar();
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            commit_width_edit();
        }
        return false;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        commit_width_edit();
        return false;
    default:
        return false;
    }
}

void Application::handle_home_event(const SDL_Event& event, bool& running)
{
    switch (event.type) {
    case SDL_EVENT_QUIT:
        request_quit(running);
        break;

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        static_cast<void>(update_viewport());
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        home_view_.clear_pointer();
        space_down_ = false;
        shift_down_ = false;
        left_shift_down_ = false;
        right_shift_down_ = false;
        update_cursor();
        break;

    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        home_view_.clear_pointer();
        update_cursor();
        break;

    case SDL_EVENT_MOUSE_MOTION:
        home_view_.set_pointer({
            static_cast<double>(event.motion.x),
            static_cast<double>(event.motion.y),
        });
        update_cursor();
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
            home_view_.pointer_down({
                static_cast<double>(event.button.x),
                static_cast<double>(event.button.y),
            });
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            const Vec2d point{
                static_cast<double>(event.button.x),
                static_cast<double>(event.button.y),
            };
            if (home_view_.pointer_up(point)) {
                if (const auto rename = home_view_.rename_at(point)) {
                    start_rename_home(*rename);
                } else if (const auto board = home_view_.board_at(point)) {
                    open_board_from_home(*board);
                } else if (const auto action = home_view_.action_at(point)) {
                    if (*action == UiAction::home_new_board) {
                        new_board();
                        if (status_error_.empty()) {
                            view_mode_ = ViewMode::board;
                            sync_toolbar();
                            record_navigation_target();
                        }
                    } else if (*action == UiAction::open_board) {
                        show_board_dialog(false);
                    } else if (*action == UiAction::toggle_theme) {
                        activate_ui_action(UiAction::toggle_theme);
                    }
                }
            }
        }
        break;

    case SDL_EVENT_MOUSE_WHEEL: {
        const double direction =
            event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
        const double amount = static_cast<double>(event.wheel.y) * direction;
        if (amount != 0.0) {
            home_view_.scroll_rows(amount > 0.0 ? -1 : 1);
        }
        break;
    }

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        {
        const bool key_down = event.type == SDL_EVENT_KEY_DOWN;
        if (event.key.scancode == SDL_SCANCODE_SPACE) {
            space_down_ = key_down;
        } else if (event.key.scancode == SDL_SCANCODE_LSHIFT) {
            left_shift_down_ = key_down;
            shift_down_ = left_shift_down_ || right_shift_down_;
        } else if (event.key.scancode == SDL_SCANCODE_RSHIFT) {
            right_shift_down_ = key_down;
            shift_down_ = left_shift_down_ || right_shift_down_;
        }
        if (!key_down) {
            break;
        }
        const bool command =
            (event.key.mod & SDL_KMOD_CTRL) != 0U
            && (event.key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0U;
        const bool plain =
            (event.key.mod
             & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0U;
        const auto activate_focused = [this]() {
            const auto focused = home_view_.focused_index();
            if (!focused.has_value()) {
                return;
            }
            if (*focused < home_view_.boards().size()) {
                open_board_from_home(*focused);
                return;
            }
            const UiAction action =
                home_view_.controls()[*focused].action;
            if (action == UiAction::home_new_board) {
                new_board();
                if (status_error_.empty()) {
                    view_mode_ = ViewMode::board;
                    sync_toolbar();
                    record_navigation_target();
                }
            } else if (action == UiAction::open_board) {
                show_board_dialog(false);
            } else if (action == UiAction::toggle_theme) {
                activate_ui_action(UiAction::toggle_theme);
            }
        };
        if (!event.key.repeat
            && plain
            && event.key.scancode == SDL_SCANCODE_TAB) {
            home_view_.focus_next(
                (event.key.mod & SDL_KMOD_SHIFT) != 0U);
            update_cursor();
        } else if (!event.key.repeat
                   && plain
                   && event.key.scancode == SDL_SCANCODE_ESCAPE) {
            home_view_.clear_focus();
        } else if (!event.key.repeat
                   && plain
                   && (event.key.scancode == SDL_SCANCODE_RETURN
                       || event.key.scancode == SDL_SCANCODE_KP_ENTER
                       || event.key.scancode == SDL_SCANCODE_SPACE)) {
            activate_focused();
        } else if (!event.key.repeat
                   && plain
                   && event.key.scancode == SDL_SCANCODE_F2) {
            const auto focused = home_view_.focused_index();
            if (focused.has_value()
                && *focused < home_view_.boards().size()) {
                start_rename_home(*focused);
            }
        } else if (!event.key.repeat
            && command
            && event.key.scancode == SDL_SCANCODE_N) {
            new_board();
            if (status_error_.empty()) {
                view_mode_ = ViewMode::board;
                sync_toolbar();
                record_navigation_target();
            }
        } else if (!event.key.repeat
                   && command
                   && event.key.scancode == SDL_SCANCODE_O) {
            show_board_dialog(false);
        } else if (!event.key.repeat
                   && plain
                   && event.key.scancode == SDL_SCANCODE_T) {
            activate_ui_action(UiAction::toggle_theme);
        }
        break;
        }

    default:
        break;
    }
}

void Application::handle_unsaved_dialog_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        static_cast<void>(update_viewport());
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        unsaved_dialog_.clear_pointer();
        update_cursor();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        unsaved_dialog_.set_pointer({
            static_cast<double>(event.motion.x),
            static_cast<double>(event.motion.y),
        });
        update_cursor();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
            unsaved_dialog_.pointer_down({
                static_cast<double>(event.button.x),
                static_cast<double>(event.button.y),
            });
            update_cursor();
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            const auto choice = unsaved_dialog_.pointer_up({
                static_cast<double>(event.button.x),
                static_cast<double>(event.button.y),
            });
            if (choice.has_value()) {
                handle_unsaved_dialog_choice(*choice);
            } else {
                update_cursor();
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            break;
        }
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
            handle_unsaved_dialog_choice(UnsavedDialogChoice::cancel);
        } else if (event.key.scancode == SDL_SCANCODE_TAB) {
            unsaved_dialog_.focus_next(
                (event.key.mod & SDL_KMOD_SHIFT) != 0U);
        } else if (event.key.scancode == SDL_SCANCODE_RETURN
                   || event.key.scancode == SDL_SCANCODE_KP_ENTER) {
            handle_unsaved_dialog_choice(
                unsaved_dialog_.focused_choice());
        }
        break;
    case SDL_EVENT_QUIT:
        // The close request is already represented by the visible modal.
        break;
    default:
        break;
    }
}

void Application::handle_event(const SDL_Event& event, bool& running)
{
    if (event.type == dialog_event_type_) {
        handle_board_dialog_result(event.user.data1);
        return;
    }
    if (unsaved_dialog_.visible()) {
        handle_unsaved_dialog_event(event);
        return;
    }

    if (event.type == SDL_EVENT_DROP_FILE) {
        if (event.drop.data == nullptr) {
            return;
        }
        if (renaming_) {
            commit_rename();
        }
        if (width_editing_) {
            commit_width_edit();
        }
        if (dialog_active_ || background_open_.valid()) {
            status_error_ = "Finish the current file operation first.";
        } else {
            const std::filesystem::path path{event.drop.data};
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error) {
                status_error_ =
                    "The dropped item is not a readable board file.";
            } else {
                begin_background_open(path);
                return;
            }
        }
        if (view_mode_ == ViewMode::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const bool alt = (event.key.mod & SDL_KMOD_ALT) != 0U;
        const bool back =
            event.key.scancode == SDL_SCANCODE_AC_BACK
            || (alt && event.key.scancode == SDL_SCANCODE_LEFT);
        const bool forward =
            event.key.scancode == SDL_SCANCODE_AC_FORWARD
            || (alt && event.key.scancode == SDL_SCANCODE_RIGHT);
        if (back || forward) {
            if (renaming_) {
                cancel_rename();
            }
            if (width_editing_) {
                cancel_width_edit();
            }
            navigate_history(back ? -1 : 1);
            return;
        }
    }
    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
         || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        && (event.button.button == SDL_BUTTON_X1
            || event.button.button == SDL_BUTTON_X2)) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (renaming_) {
                cancel_rename();
            }
            if (width_editing_) {
                cancel_width_edit();
            }
            navigate_history(
                event.button.button == SDL_BUTTON_X1 ? -1 : 1);
        }
        return;
    }

    if (renaming_ && handle_rename_event(event)) {
        return;
    }
    if (width_editing_ && handle_width_edit_event(event)) {
        return;
    }

    if (view_mode_ == ViewMode::home) {
        handle_home_event(event, running);
        return;
    }

    switch (event.type) {
    case SDL_EVENT_QUIT:
        request_quit(running);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const Vec2d screen_point{
            static_cast<double>(event.button.x),
            static_cast<double>(event.button.y),
        };
        pointer_screen_ = screen_point;
        toolbar_.set_pointer(screen_point);
        if (event.button.button == SDL_BUTTON_MIDDLE) {
            middle_button_down_ = down && !toolbar_.contains(screen_point);
            if (middle_button_down_) {
                cancel_gesture();
                cancel_selection_gesture();
            }
            update_cursor();
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            left_button_down_ = down;
            if (down && toolbar_.contains(screen_point)) {
                ui_pointer_down_ = true;
                title_pointer_down_ = false;
                hand_dragging_ = false;
                toolbar_.pointer_down(screen_point);
                cancel_gesture();
                cancel_selection_gesture();
                if (const auto action = toolbar_.action_at(screen_point)) {
                    if (*action == UiAction::custom_hue_field
                        || *action == UiAction::custom_sv_field) {
                        color_drag_action_ = *action;
                        apply_custom_color(*action, screen_point);
                    }
                } else {
                    title_pointer_down_ =
                        toolbar_.filename_bounds().contains(screen_point);
                }
                update_cursor();
                break;
            }
            if (!down && ui_pointer_down_) {
                if (color_drag_action_.has_value()) {
                    apply_custom_color(*color_drag_action_, screen_point);
                }
                const std::optional<UiAction> action =
                    toolbar_.pointer_up(screen_point);
                const bool activate_title = title_pointer_down_
                    && toolbar_.filename_bounds().contains(screen_point);
                ui_pointer_down_ = false;
                title_pointer_down_ = false;
                hand_dragging_ = false;
                const bool was_color_drag = color_drag_action_.has_value();
                color_drag_action_.reset();
                if (!was_color_drag && action.has_value()) {
                    activate_ui_action(*action);
                } else if (!action.has_value() && activate_title) {
                    start_rename_current();
                }
                update_cursor();
                break;
            }
            // Clicking the canvas while a flyout is open dismisses it rather
            // than starting a drawing or panning gesture.
            if (down && toolbar_.settings_open()) {
                hand_dragging_ = false;
                toolbar_.clear_focus();
                toolbar_.close_settings_panel();
                sync_toolbar();
                update_cursor();
                break;
            }
            if (down) {
                toolbar_.clear_focus();
            }
            const auto sample = mouse_sample(
                event.button.x, event.button.y, event.button.timestamp);
            if (down && !space_down_ && current_tool_ == Tool::hand) {
                hand_dragging_ = true;
            } else if (down && !space_down_ && current_tool_ == Tool::select) {
                begin_selection_gesture(sample);
            } else if (down && !space_down_) {
                begin_gesture(sample);
            } else if (!down && active_draft_.has_value()) {
                finish_gesture(sample);
            } else if (!down
                       && selection_interaction_
                           != SelectionInteraction::none) {
                finish_selection_gesture(sample);
            }
            if (!down) {
                hand_dragging_ = false;
            }
            update_cursor();
        }
        break;
    }

    case SDL_EVENT_MOUSE_MOTION:
        pointer_screen_ = Vec2d{
            static_cast<double>(event.motion.x),
            static_cast<double>(event.motion.y),
        };
        toolbar_.set_pointer(*pointer_screen_);
        update_cursor();
        if (ui_pointer_down_) {
            if (color_drag_action_.has_value()) {
                apply_custom_color(
                    *color_drag_action_,
                    {static_cast<double>(event.motion.x),
                     static_cast<double>(event.motion.y)});
            }
            break;
        }
        if (selection_interaction_ != SelectionInteraction::none) {
            update_selection_gesture(mouse_sample(
                event.motion.x,
                event.motion.y,
                event.motion.timestamp));
        } else if (active_draft_.has_value()) {
            update_gesture(mouse_sample(
                event.motion.x,
                event.motion.y,
                event.motion.timestamp));
        } else if (middle_button_down_ || hand_dragging_
                   || (space_down_ && left_button_down_)) {
            camera_.pan_by_screen_delta({
                static_cast<double>(event.motion.xrel),
                static_cast<double>(event.motion.yrel),
            });
        }
        break;

    case SDL_EVENT_MOUSE_WHEEL: {
        const Vec2d screen_point{
            static_cast<double>(event.wheel.mouse_x),
            static_cast<double>(event.wheel.mouse_y),
        };
        toolbar_.set_pointer(screen_point);
        if (toolbar_.contains(screen_point)
            || left_button_down_
            || middle_button_down_
            || active_draft_.has_value()
            || selection_interaction_ != SelectionInteraction::none) {
            break;
        }
        const double direction =
            event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
        zoom_by_steps_at(
            screen_point,
            static_cast<double>(event.wheel.y) * direction);
        break;
    }

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool key_down = event.type == SDL_EVENT_KEY_DOWN;
        if (event.key.scancode == SDL_SCANCODE_SPACE) {
            const bool entering_pan =
                key_down && !space_down_ && left_button_down_;
            space_down_ = key_down;
            if (entering_pan) {
                cancel_gesture();
                cancel_selection_gesture();
            }
            update_cursor();
        }
        if (event.key.scancode == SDL_SCANCODE_LSHIFT) {
            left_shift_down_ = key_down;
            shift_down_ = left_shift_down_ || right_shift_down_;
        } else if (event.key.scancode == SDL_SCANCODE_RSHIFT) {
            right_shift_down_ = key_down;
            shift_down_ = left_shift_down_ || right_shift_down_;
        }

        if (key_down && !event.key.repeat) {
            const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0U;
            const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0U;
            const bool alt = (event.key.mod & SDL_KMOD_ALT) != 0U;
            const bool gui = (event.key.mod & SDL_KMOD_GUI) != 0U;
            const bool command = control && !alt && !gui;
            const bool plain = !control && !alt && !gui;
            if (plain && event.key.scancode == SDL_SCANCODE_TAB) {
                toolbar_.focus_next(shift);
                sync_toolbar();
            } else if (plain
                       && (event.key.scancode == SDL_SCANCODE_RETURN
                           || event.key.scancode == SDL_SCANCODE_KP_ENTER)
                       && toolbar_.focused_control() != nullptr) {
                activate_ui_action(toolbar_.focused_control()->action);
            } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                const bool had_gesture = active_draft_.has_value()
                    || selection_interaction_ != SelectionInteraction::none;
                toolbar_.clear_focus();
                if (toolbar_.settings_open()) {
                    toolbar_.close_settings_panel();
                } else if (!had_gesture && current_tool_ == Tool::select
                           && !selection_.empty()) {
                    selection_.clear();
                } else if (!had_gesture && current_tool_ != Tool::select) {
                    current_tool_ = Tool::select;
                }
                cancel_gesture();
                cancel_selection_gesture();
                sync_toolbar();
            } else if (event.key.scancode == SDL_SCANCODE_DELETE
                       || event.key.scancode == SDL_SCANCODE_BACKSPACE) {
                delete_selection();
            } else if (command && event.key.scancode == SDL_SCANCODE_Z) {
                undo_or_redo(shift);
            } else if (command && event.key.scancode == SDL_SCANCODE_Y) {
                undo_or_redo(true);
            } else if (command && event.key.scancode == SDL_SCANCODE_S) {
                if (shift || !board_file_.has_value()) {
                    show_board_dialog(true);
                } else {
                    static_cast<void>(flush_board_safely());
                }
            } else if (command && event.key.scancode == SDL_SCANCODE_N) {
                activate_ui_action(UiAction::new_board);
            } else if (command && event.key.scancode == SDL_SCANCODE_O) {
                activate_ui_action(UiAction::open_board);
            } else if (command && event.key.scancode == SDL_SCANCODE_D) {
                duplicate_selection();
            } else if (command && event.key.scancode == SDL_SCANCODE_C) {
                static_cast<void>(copy_selection());
            } else if (command && event.key.scancode == SDL_SCANCODE_X) {
                if (copy_selection()) delete_selection();
            } else if (command && event.key.scancode == SDL_SCANCODE_V) {
                paste_clipboard();
            } else if (command && event.key.scancode == SDL_SCANCODE_MINUS) {
                zoom_by_steps_at_viewport_center(-1.0);
            } else if (command && (event.key.scancode == SDL_SCANCODE_EQUALS
                                  || event.key.scancode == SDL_SCANCODE_KP_PLUS)) {
                zoom_by_steps_at_viewport_center(1.0);
            } else if (command && (event.key.scancode == SDL_SCANCODE_0
                                  || event.key.scancode == SDL_SCANCODE_KP_0)) {
                zoom_at_viewport_center(1.0 / camera_.zoom());
            } else if (plain && event.key.scancode == SDL_SCANCODE_P) {
                activate_ui_action(UiAction::pencil);
            } else if (plain && event.key.scancode == SDL_SCANCODE_L) {
                activate_ui_action(UiAction::line);
            } else if (plain && event.key.scancode == SDL_SCANCODE_R) {
                activate_ui_action(UiAction::rectangle);
            } else if (plain && event.key.scancode == SDL_SCANCODE_E) {
                activate_ui_action(UiAction::ellipse);
            } else if (plain && event.key.scancode == SDL_SCANCODE_V) {
                activate_ui_action(UiAction::select);
            } else if (plain && event.key.scancode == SDL_SCANCODE_H) {
                activate_ui_action(UiAction::hand);
            } else if (plain && event.key.scancode == SDL_SCANCODE_F) {
                activate_ui_action(UiAction::toggle_fill);
            } else if (plain && event.key.scancode >= SDL_SCANCODE_1
                       && event.key.scancode <= SDL_SCANCODE_7) {
                activate_ui_action(static_cast<UiAction>(
                    static_cast<int>(UiAction::color_white)
                    + static_cast<int>(event.key.scancode - SDL_SCANCODE_1)));
            } else if (plain
                       && event.key.scancode == SDL_SCANCODE_LEFTBRACKET) {
                activate_ui_action(UiAction::width_decrease);
            } else if (plain
                       && event.key.scancode == SDL_SCANCODE_RIGHTBRACKET) {
                activate_ui_action(UiAction::width_increase);
            } else if (plain && event.key.scancode == SDL_SCANCODE_T) {
                activate_ui_action(UiAction::toggle_theme);
            }
        }
        break;
    }

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        static_cast<void>(update_viewport());
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        middle_button_down_ = false;
        left_button_down_ = false;
        hand_dragging_ = false;
        ui_pointer_down_ = false;
        title_pointer_down_ = false;
        space_down_ = false;
        shift_down_ = false;
        left_shift_down_ = false;
        right_shift_down_ = false;
        color_drag_action_.reset();
        pointer_screen_.reset();
        toolbar_.clear_pointer();
        cancel_gesture();
        cancel_selection_gesture();
        update_cursor();
        static_cast<void>(flush_board_safely());
        break;

    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (!left_button_down_ && !middle_button_down_) {
            pointer_screen_.reset();
            toolbar_.clear_pointer();
            update_cursor();
        }
        break;

    default:
        break;
    }
}

void Application::begin_gesture(const PointerSample sample)
{
    // Never start editing while a board is still loading in the background; the
    // pending load would overwrite the document and lose the edit.
    if (background_open_.valid()) {
        return;
    }
    active_raw_stroke_points_.clear();
    switch (current_tool_) {
    case Tool::select:
    case Tool::hand:
        return;
    case Tool::pencil: {
        pointer_resampler_.reset(sample);
        active_raw_stroke_points_.push_back(sample.position);
        if (drawing_settings_.stabilization_enabled()) {
            velocity_gaussian_stabilizer_.reset(
                drawing_settings_.gaussian_sigma(),
                sample,
                camera_.zoom());
        }
        active_draft_ = ObjectDraft{
            .geometry = Stroke{{sample.position}},
            .style = current_style_,
            .generation = ++next_draft_generation_,
        };
        break;
    }
    case Tool::line:
        active_draft_ = ObjectDraft{
            .geometry = Line{sample.position, sample.position},
            .style = current_style_,
            .generation = ++next_draft_generation_,
        };
        break;
    case Tool::rectangle:
        active_draft_ = ObjectDraft{
            .geometry = RectangleShape{
                sample.position,
                sample.position,
                rectangle_roundness_,
            },
            .style = current_style_,
            .generation = ++next_draft_generation_,
        };
        break;
    case Tool::ellipse:
        active_draft_ = ObjectDraft{
            .geometry = Ellipse{sample.position, sample.position},
            .style = current_style_,
            .generation = ++next_draft_generation_,
        };
        break;
    }
}

void Application::update_gesture(
    const PointerSample sample,
    const bool complete)
{
    if (!active_draft_.has_value()) {
        return;
    }

    if (auto* const stroke =
            std::get_if<Stroke>(&active_draft_->geometry)) {
        const auto samples =
            pointer_resampler_.push(sample, camera_.zoom(), complete);
        bool changed = false;
        for (const PointerSample resampled : samples) {
            const PointerSample stabilized =
                drawing_settings_.stabilization_enabled()
                ? velocity_gaussian_stabilizer_.push(resampled)
                : resampled;
            if (active_raw_stroke_points_.size()
                >= maximum_stroke_samples) {
                break;
            }
            if (!append_filtered_point(
                active_raw_stroke_points_,
                stabilized.position,
                drawing_settings_.sampling_distance() / camera_.zoom())) {
                continue;
            }
            const Vec2d live_endpoint = trailing_smoothed_stroke_point(
                active_raw_stroke_points_,
                drawing_settings_.completion_smoothing_radius());
            const std::size_t previous_size = stroke->points.size();
            const Vec2d previous_endpoint =
                stroke->points.empty() ? Vec2d{} : stroke->points.back();
            if (stroke->points.size() < 2U) {
                stroke->points.push_back(live_endpoint);
            } else {
                append_smooth_tail(
                    stroke->points,
                    live_endpoint,
                    0.05 / camera_.zoom());
            }
            changed = changed
                || stroke->points.size() != previous_size
                || (!stroke->points.empty()
                    && stroke->points.back() != previous_endpoint);
        }
        if (changed) {
            ++active_draft_->revision;
        }
    } else if (auto* const line =
                   std::get_if<Line>(&active_draft_->geometry)) {
        const Vec2d endpoint = constrain_line_endpoint(
            line->start, sample.position, shift_down_);
        if (line->end != endpoint) {
            line->end = endpoint;
            ++active_draft_->revision;
        }
    } else if (auto* const rectangle =
                   std::get_if<RectangleShape>(&active_draft_->geometry)) {
        const Vec2d endpoint = constrain_shape_endpoint(
            rectangle->first, sample.position, shift_down_);
        if (rectangle->second != endpoint) {
            rectangle->second = endpoint;
            ++active_draft_->revision;
        }
    } else if (auto* const ellipse =
                   std::get_if<Ellipse>(&active_draft_->geometry)) {
        const Vec2d endpoint = constrain_shape_endpoint(
            ellipse->first, sample.position, shift_down_);
        if (ellipse->second != endpoint) {
            ellipse->second = endpoint;
            ++active_draft_->revision;
        }
    }
}

void Application::finish_gesture(const PointerSample sample)
{
    if (!active_draft_.has_value()) {
        return;
    }

    update_gesture(sample, true);
    std::optional<Object> completed;
    const ObjectId id = ObjectId::random();

    if (auto* const stroke =
            std::get_if<Stroke>(&active_draft_->geometry)) {
        if (!active_raw_stroke_points_.empty()) {
            stroke->points = std::move(active_raw_stroke_points_);
        }
        auto curved = complete_stroke_points(
            std::move(stroke->points),
            sample.position,
            camera_.zoom(),
            drawing_settings_.completion_smoothing_radius(),
            drawing_settings_.stabilization_enabled());
        if (!curved.empty()) {
            completed = Object::make_stroke(
                id,
                document_.next_z_order(),
                Stroke{std::move(curved)},
                active_draft_->style);
        }
    } else if (const auto* const line =
                   std::get_if<Line>(&active_draft_->geometry)) {
        if (std::hypot(
                line->end.x - line->start.x,
                line->end.y - line->start.y) > 0.01) {
            completed = Object::make_line(
                id,
                document_.next_z_order(),
                *line,
                active_draft_->style);
        }
    } else if (const auto* const rectangle =
                   std::get_if<RectangleShape>(&active_draft_->geometry)) {
        if (std::abs(rectangle->second.x - rectangle->first.x) > 0.01
            && std::abs(rectangle->second.y - rectangle->first.y) > 0.01) {
            completed = Object::make_rectangle(
                id,
                document_.next_z_order(),
                *rectangle,
                active_draft_->style);
        }
    } else if (const auto* const ellipse =
                   std::get_if<Ellipse>(&active_draft_->geometry)) {
        if (std::abs(ellipse->second.x - ellipse->first.x) > 0.01
            && std::abs(ellipse->second.y - ellipse->first.y) > 0.01) {
            completed = Object::make_ellipse(
                id,
                document_.next_z_order(),
                *ellipse,
                active_draft_->style);
        }
    }

    if (completed.has_value()) {
        history_.execute(
            std::make_unique<AddObjectCommand>(std::move(*completed)),
            document_);
        record_object_state(id);
    }
    active_draft_.reset();
    active_raw_stroke_points_.clear();
    update_window_title();
}

void Application::cancel_gesture() noexcept
{
    active_draft_.reset();
    active_raw_stroke_points_.clear();
}

void Application::begin_selection_gesture(const PointerSample sample)
{
    if (background_open_.valid()) {
        return;
    }
    cancel_selection_gesture();
    selection_start_ = sample.position;
    const double handle_tolerance = 8.0 / camera_.zoom();
    selection_handle_ = selection_handle_at(
        document_, selection_, sample.position, handle_tolerance);
    if (selection_handle_ != SelectionHandle::none) {
        selection_interaction_ = SelectionInteraction::resize;
    } else if (const auto hit = hit_test(
                   document_, sample.position, 5.0 / camera_.zoom())) {
        if (!selection_.contains(*hit)) {
            selection_.select(*hit);
        }
        selection_interaction_ = SelectionInteraction::move;
    } else {
        selection_.clear();
        selection_interaction_ = SelectionInteraction::marquee;
        selection_.set_marquee(Aabb::from_points(sample.position, sample.position));
    }

    selection_edit_ids_.clear();
    selection_original_bounds_.reset();
    selection_preview_.reset();
    if (selection_interaction_ == SelectionInteraction::move
        || selection_interaction_ == SelectionInteraction::resize) {
        selection_edit_ids_ = selection_.ids();
        selection_original_bounds_ = selection_.bounds(document_);
        if (!selection_edit_ids_.empty()
            && selection_original_bounds_.has_value()) {
            selection_preview_.emplace(selection_edit_ids_);
        }
    }
}

void Application::update_selection_gesture(const PointerSample sample)
{
    if (selection_interaction_ == SelectionInteraction::marquee) {
        selection_.set_marquee(
            Aabb::from_points(selection_start_, sample.position));
        return;
    }
    if (!selection_preview_.has_value()
        || !selection_original_bounds_.has_value()
        || selection_edit_ids_.empty()) {
        return;
    }

    if (selection_interaction_ == SelectionInteraction::move) {
        Vec2d delta{
            sample.position.x - selection_start_.x,
            sample.position.y - selection_start_.y,
        };
        if (shift_down_) {
            if (std::abs(delta.x) >= std::abs(delta.y)) {
                delta.y = 0.0;
            } else {
                delta.x = 0.0;
            }
        }
        const Aabb bounds = *selection_original_bounds_;
        delta.x = clamp_board_translation_axis(
            delta.x, bounds.min_x, bounds.max_x);
        delta.y = clamp_board_translation_axis(
            delta.y, bounds.min_y, bounds.max_y);
        selection_preview_->transform = {
            .anchor = {},
            .translation = delta,
        };
        selection_preview_->replacement.reset();
    } else if (selection_interaction_ == SelectionInteraction::resize) {
        if (selection_edit_ids_.size() == 1U) {
            const Object* const original =
                document_.find(selection_edit_ids_.front());
            if (original == nullptr) {
                return;
            }
            Object resized = *original;
            resize_object(
                resized,
                selection_handle_,
                sample.position,
                shift_down_);
            selection_preview_->transform = {};
            selection_preview_->replacement = std::move(resized);
        } else {
            selection_preview_->replacement.reset();
            selection_preview_->transform = group_resize_transform(
                *selection_original_bounds_,
                selection_handle_,
                sample.position,
                shift_down_);
        }
    }
}

void Application::finish_selection_gesture(const PointerSample sample)
{
    update_selection_gesture(sample);
    if (selection_interaction_ == SelectionInteraction::marquee) {
        if (selection_.marquee().has_value()) {
            selection_.select(
                marquee_hit_test(document_, *selection_.marquee()));
        }
    } else if (selection_preview_.has_value()
               && selection_original_bounds_.has_value()) {
        std::vector<std::unique_ptr<Command>> commands;
        commands.reserve(selection_edit_ids_.size());
        if (selection_preview_->replacement.has_value()) {
            const Object& replacement = *selection_preview_->replacement;
            const Object* const original = document_.find(replacement.id);
            if (original != nullptr
                && replacement.geometry != original->geometry) {
                commands.push_back(std::make_unique<ModifyObjectCommand>(
                    replacement));
            }
        } else if (!selection_preview_->transform.identity()) {
            for (const auto id : selection_edit_ids_) {
                const Object* const original = document_.find(id);
                if (original == nullptr) {
                    continue;
                }
                Object replacement = *original;
                if (selection_interaction_ == SelectionInteraction::move) {
                    commands.push_back(
                        std::make_unique<MoveObjectCommand>(
                            id,
                            selection_preview_->transform.translation));
                    continue;
                } else {
                    resize_object_in_group(
                        replacement,
                        *selection_original_bounds_,
                        selection_handle_,
                        sample.position,
                        shift_down_);
                }
                if (replacement.geometry != original->geometry) {
                    commands.push_back(
                        std::make_unique<ModifyObjectCommand>(
                            std::move(replacement)));
                }
            }
        }
        if (!commands.empty()) {
            history_.execute(
                std::make_unique<CompositeCommand>(std::move(commands)),
                document_);
            if (selection_interaction_ == SelectionInteraction::move) {
                record_move_states(
                    selection_edit_ids_,
                    selection_preview_->transform.translation);
            } else {
                record_object_states(selection_edit_ids_);
            }
        }
    }

    selection_interaction_ = SelectionInteraction::none;
    selection_handle_ = SelectionHandle::none;
    selection_edit_ids_.clear();
    selection_original_bounds_.reset();
    selection_preview_.reset();
    selection_.set_marquee(std::nullopt);
    update_window_title();
}

void Application::cancel_selection_gesture() noexcept
{
    selection_interaction_ = SelectionInteraction::none;
    selection_handle_ = SelectionHandle::none;
    selection_edit_ids_.clear();
    selection_original_bounds_.reset();
    selection_preview_.reset();
    selection_.set_marquee(std::nullopt);
}

void Application::delete_selection()
{
    cancel_selection_gesture();
    if (selection_.empty()) {
        return;
    }
    const auto ids = selection_.ids();
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(ids.size());
    for (const auto id : ids) {
        if (document_.find(id) != nullptr) {
            commands.push_back(std::make_unique<DeleteObjectCommand>(id));
        }
    }
    if (commands.empty()) {
        selection_.clear();
        return;
    }
    history_.execute(
        std::make_unique<CompositeCommand>(std::move(commands)), document_);
    selection_.clear();
    record_object_states(ids);
}

void Application::duplicate_selection()
{
    cancel_selection_gesture();
    if (selection_.empty()) {
        return;
    }
    std::vector<std::unique_ptr<Command>> commands;
    std::vector<ObjectId> duplicated_ids;
    const Vec2d requested_offset{16.0 / camera_.zoom(), 16.0 / camera_.zoom()};
    for (const auto id : selection_.ids()) {
        const Object* const original = document_.find(id);
        if (original == nullptr) {
            continue;
        }
        Object duplicate = *original;
        duplicate.id = ObjectId::random();
        duplicate.z_order = document_.next_z_order();
        duplicate.revision = 1U;
        Vec2d offset = requested_offset;
        offset.x = clamp_board_translation_axis(
            offset.x, duplicate.bounds.min_x, duplicate.bounds.max_x);
        offset.y = clamp_board_translation_axis(
            offset.y, duplicate.bounds.min_y, duplicate.bounds.max_y);
        translate_object(duplicate, offset);
        duplicated_ids.push_back(duplicate.id);
        commands.push_back(
            std::make_unique<AddObjectCommand>(std::move(duplicate)));
    }
    if (commands.empty()) {
        return;
    }
    history_.execute(
        std::make_unique<CompositeCommand>(std::move(commands)), document_);
    selection_.select(duplicated_ids);
    record_object_states(duplicated_ids);
}

bool Application::copy_selection()
{
    if (selection_.empty()) {
        status_error_ = "Nothing is selected to copy.";
        sync_toolbar();
        return false;
    }
    const std::vector<ObjectId> ids = selection_.ids();
    Clipboard::Payload payload;
    try {
        payload.emplace_back(
            std::string{sawer_clipboard_mime},
            serialize_clipboard_fragment(document_, ids));
    } catch (const std::exception& error) {
        status_error_ = std::string{"Could not copy selection: "} + error.what();
        log::write(log::Level::error, status_error_);
        sync_toolbar();
        return false;
    }
    if (ids.size() == 1U) {
        const Object* const object = document_.find(ids.front());
        const Image* const image = object == nullptr
            ? nullptr : std::get_if<Image>(&object->geometry);
        if (image != nullptr && image->asset && !image->asset->png.empty()) {
            payload.emplace_back("image/png", image->asset->png);
        }
    }
    if (!clipboard_.offer(std::move(payload))) {
        status_error_ = "Could not offer the selection to the clipboard.";
        log::write(log::Level::error, status_error_);
        sync_toolbar();
        return false;
    }
    status_error_.clear();
    sync_toolbar();
    return true;
}

void Application::paste_clipboard()
{
    if (view_mode_ != ViewMode::board || background_open_.valid()) {
        return;
    }
    if (clipboard_paste_.valid() || clipboard_fragment_paste_.valid()) {
        status_error_ = "A clipboard paste is already in progress.";
        sync_toolbar();
        return;
    }
    const bool has_fragment = clipboard_.has(sawer_clipboard_mime);
    const char* mime_type = has_fragment ? sawer_clipboard_mime.data()
        : (clipboard_.has("image/png") ? "image/png"
            : (clipboard_.has("image/bmp") ? "image/bmp" : nullptr));
    if (mime_type == nullptr) {
        status_error_ = "Clipboard is empty.";
        sync_toolbar();
        return;
    }
    const auto bytes = clipboard_.read(mime_type);
    if (!bytes.has_value() || bytes->empty()) {
        status_error_ = "Could not read the clipboard data.";
        sync_toolbar();
        return;
    }
    const Vec2d screen_anchor = pointer_screen_.value_or(Vec2d{
        camera_.viewport().x * 0.5, camera_.viewport().y * 0.5});
    clipboard_paste_anchor_ = camera_.screen_to_world(screen_anchor);
    clipboard_paste_generation_ = lifecycle_generation_;
    clipboard_paste_digest_ = sha256(*bytes);
    try {
        if (has_fragment) {
            clipboard_fragment_paste_ = std::async(
                std::launch::async,
                [encoded = std::move(*bytes)]() {
                    return deserialize_clipboard_fragment(encoded);
                });
        } else {
            clipboard_paste_ = std::async(
                std::launch::async,
                [encoded = std::move(*bytes)]() {
                    return canonicalize_image(encoded);
                });
        }
        // Paste progress is not an error and therefore does not use the
        // persistent red error banner.
        status_error_.clear();
        sync_toolbar();
    } catch (const std::exception& error) {
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        sync_toolbar();
    }
}

bool Application::poll_clipboard_paste()
{
    if (clipboard_fragment_paste_.valid()
        && clipboard_fragment_paste_.wait_for(std::chrono::milliseconds{0})
            == std::future_status::ready) {
        try {
            ClipboardFragment fragment = clipboard_fragment_paste_.get();
            if (clipboard_paste_generation_ != lifecycle_generation_
                || view_mode_ != ViewMode::board) {
                return true;
            }
            if (clipboard_paste_digest_ != last_clipboard_paste_digest_) {
                last_clipboard_paste_digest_ = clipboard_paste_digest_;
                clipboard_fragment_paste_count_ = 0U;
            }
            ++clipboard_fragment_paste_count_;

            Aabb bounds = fragment.objects.front().bounds;
            for (const Object& object : fragment.objects) {
                bounds.min_x = std::min(bounds.min_x, object.bounds.min_x);
                bounds.min_y = std::min(bounds.min_y, object.bounds.min_y);
                bounds.max_x = std::max(bounds.max_x, object.bounds.max_x);
                bounds.max_y = std::max(bounds.max_y, object.bounds.max_y);
            }
            const bool already_visible =
                bounds.intersects(camera_.visible_world_bounds());
            const double step = 24.0 / camera_.zoom();
            const double repeat =
                static_cast<double>(clipboard_fragment_paste_count_);
            Vec2d translation = already_visible
                ? Vec2d{step * repeat, step * repeat}
                : Vec2d{
                    clipboard_paste_anchor_.x
                        - (bounds.min_x + bounds.max_x) * 0.5,
                    clipboard_paste_anchor_.y
                        - (bounds.min_y + bounds.max_y) * 0.5,
                };
            translation.x = std::clamp(
                translation.x,
                -board_half_extent - bounds.min_x,
                board_half_extent - bounds.max_x);
            translation.y = std::clamp(
                translation.y,
                -board_half_extent - bounds.min_y,
                board_half_extent - bounds.max_y);

            std::vector<std::unique_ptr<Command>> commands;
            std::vector<ObjectId> pasted_ids;
            commands.reserve(fragment.objects.size());
            pasted_ids.reserve(fragment.objects.size());
            for (Object& object : fragment.objects) {
                object.id = ObjectId::random();
                object.z_order = document_.next_z_order();
                object.revision = 1U;
                object.translate(translation);
                pasted_ids.push_back(object.id);
                commands.push_back(
                    std::make_unique<AddObjectCommand>(std::move(object)));
            }
            history_.execute(
                std::make_unique<CompositeCommand>(std::move(commands)),
                document_);
            selection_.select(pasted_ids);
            current_tool_ = Tool::select;
            record_object_states(pasted_ids);
            status_error_.clear();
            sync_toolbar();
            update_window_title();
        } catch (const std::exception& error) {
            status_error_ =
                std::string{"Could not paste selection: "} + error.what();
            log::write(log::Level::error, status_error_);
            sync_toolbar();
        }
        return true;
    }
    if (!clipboard_paste_.valid()
        || clipboard_paste_.wait_for(std::chrono::milliseconds{0})
            != std::future_status::ready) {
        return false;
    }
    try {
        CanonicalImage pasted = clipboard_paste_.get();
        if (clipboard_paste_generation_ != lifecycle_generation_
            || view_mode_ != ViewMode::board) {
            return true;
        }
        const double viewport_width = camera_.viewport().x / camera_.zoom();
        const double viewport_height = camera_.viewport().y / camera_.zoom();
        const double scale = std::min({
            1.0,
            viewport_width * 0.6 / static_cast<double>(pasted.asset->pixel_width),
            viewport_height * 0.6 / static_cast<double>(pasted.asset->pixel_height),
        });
        const double width = static_cast<double>(pasted.asset->pixel_width) * scale;
        const double height = static_cast<double>(pasted.asset->pixel_height) * scale;
        const Vec2d first{
            std::clamp(clipboard_paste_anchor_.x - width * 0.5,
                -board_half_extent, board_half_extent - width),
            std::clamp(clipboard_paste_anchor_.y - height * 0.5,
                -board_half_extent, board_half_extent - height),
        };
        const ObjectId id = ObjectId::random();
        Object object = Object::make_image(
            id, document_.next_z_order(),
            {std::move(pasted.asset), first, {first.x + width, first.y + height}});
        history_.execute(std::make_unique<AddObjectCommand>(std::move(object)), document_);
        selection_.select(id);
        current_tool_ = Tool::select;
        record_object_state(id);
        status_error_.clear();
        sync_toolbar();
        update_window_title();
    } catch (const std::exception& error) {
        status_error_ = std::string{"Could not paste image: "} + error.what();
        log::write(log::Level::error, status_error_);
        sync_toolbar();
    }
    return true;
}

void Application::restyle_selection(const UiAction action)
{
    cancel_selection_gesture();
    if (selection_.empty()) {
        return;
    }

    const bool color_action = action >= UiAction::color_white
        && action <= UiAction::color_black;
    const bool width_action = action >= UiAction::width_thin
        && action <= UiAction::width_heavy;
    const bool fill_action = action == UiAction::toggle_fill
        || action == UiAction::fill_none;
    const bool roundness_action = action >= UiAction::roundness_square
        && action <= UiAction::roundness_full;
    if (!color_action && !width_action && !fill_action
        && !roundness_action) {
        return;
    }

    bool enable_fill = false;
    if (action == UiAction::toggle_fill) {
        for (const auto id : selection_.ids()) {
            const Object* const object = document_.find(id);
            if (object == nullptr) continue;
            const bool closed =
                std::holds_alternative<RectangleShape>(object->geometry)
                || std::holds_alternative<Ellipse>(object->geometry);
            if (closed && !object->style.fill.has_value()) {
                enable_fill = true;
                break;
            }
        }
    }

    const StyleColorTarget color_target = effective_style_color_target();
    std::vector<std::unique_ptr<Command>> commands;
    std::vector<ObjectId> changed_ids;
    commands.reserve(selection_.ids().size());
    changed_ids.reserve(selection_.ids().size());
    for (const auto id : selection_.ids()) {
        const Object* const existing = document_.find(id);
        if (existing == nullptr) continue;
        if (std::holds_alternative<Image>(existing->geometry)) continue;
        if (roundness_action) {
            Object replacement = *existing;
            auto* const rectangle =
                std::get_if<RectangleShape>(&replacement.geometry);
            if (rectangle == nullptr) continue;
            rectangle->roundness = Toolbar::roundness_for(action);
            if (replacement.geometry == existing->geometry) continue;
            changed_ids.push_back(id);
            commands.push_back(
                std::make_unique<ModifyObjectCommand>(
                    std::move(replacement)));
            continue;
        }

        Style replacement = existing->style;
        if (color_action) {
            const Color color = Toolbar::color_for(action);
            if (color_target == StyleColorTarget::fill) {
                const bool closed =
                    std::holds_alternative<RectangleShape>(
                        existing->geometry)
                    || std::holds_alternative<Ellipse>(
                        existing->geometry);
                if (!closed) continue;
                replacement.fill = color;
            } else {
                replacement.stroke = color;
            }
        } else if (width_action) {
            replacement.stroke_width = Toolbar::width_for(action);
        } else if (fill_action) {
            const bool closed =
                std::holds_alternative<RectangleShape>(existing->geometry)
                || std::holds_alternative<Ellipse>(existing->geometry);
            if (!closed) continue;
            if (enable_fill) {
                if (!replacement.fill.has_value()) {
                    replacement.fill = replacement.stroke;
                }
            } else {
                replacement.fill = std::nullopt;
            }
        }
        if (replacement == existing->style) continue;
        changed_ids.push_back(id);
        commands.push_back(
            std::make_unique<ChangeStyleCommand>(
                id, std::move(replacement)));
    }

    if (commands.empty()) {
        sync_toolbar();
        return;
    }
    history_.execute(
        std::make_unique<CompositeCommand>(std::move(commands)), document_);
    if (roundness_action) {
        record_object_states(changed_ids);
    } else {
        record_style_states(changed_ids);
    }
    sync_toolbar();
}

void Application::restyle_selection(const Color color)
{
    cancel_selection_gesture();
    if (selection_.empty()) {
        return;
    }

    const StyleColorTarget color_target = effective_style_color_target();
    std::vector<std::unique_ptr<Command>> commands;
    std::vector<ObjectId> changed_ids;
    commands.reserve(selection_.ids().size());
    changed_ids.reserve(selection_.ids().size());
    for (const auto id : selection_.ids()) {
        const Object* const existing = document_.find(id);
        if (existing == nullptr) continue;
        if (std::holds_alternative<Image>(existing->geometry)) continue;
        Style replacement = existing->style;
        if (color_target == StyleColorTarget::fill) {
            const bool closed =
                std::holds_alternative<RectangleShape>(existing->geometry)
                || std::holds_alternative<Ellipse>(existing->geometry);
            if (!closed) continue;
            replacement.fill = color;
        } else {
            replacement.stroke = color;
        }
        if (replacement == existing->style) continue;
        changed_ids.push_back(id);
        commands.push_back(
            std::make_unique<ChangeStyleCommand>(
                id, std::move(replacement)));
    }

    if (commands.empty()) {
        sync_toolbar();
        return;
    }
    history_.execute(
        std::make_unique<CompositeCommand>(std::move(commands)), document_);
    record_style_states(changed_ids);
    sync_toolbar();
}

void Application::restyle_selection_width(
    const double value, const bool relative)
{
    cancel_selection_gesture();
    if (selection_.empty()) {
        return;
    }

    std::vector<std::unique_ptr<Command>> commands;
    std::vector<ObjectId> changed_ids;
    commands.reserve(selection_.ids().size());
    changed_ids.reserve(selection_.ids().size());
    for (const auto id : selection_.ids()) {
        const Object* const existing = document_.find(id);
        if (existing == nullptr) continue;
        if (std::holds_alternative<Image>(existing->geometry)) continue;
        Style replacement = existing->style;
        const double requested = relative
            ? replacement.stroke_width + value
            : value;
        replacement.stroke_width = std::clamp(
            std::round(requested * 10.0) / 10.0,
            minimum_stroke_width,
            maximum_stroke_width);
        if (replacement == existing->style) continue;
        changed_ids.push_back(id);
        commands.push_back(
            std::make_unique<ChangeStyleCommand>(
                id, std::move(replacement)));
    }

    if (commands.empty()) {
        sync_toolbar();
        return;
    }
    history_.execute(
        std::make_unique<CompositeCommand>(std::move(commands)), document_);
    record_style_states(changed_ids);
    sync_toolbar();
}

void Application::set_context_stroke_width(const double width)
{
    if (current_tool_ == Tool::hand
        || (current_tool_ == Tool::select && selection_.empty())) {
        return;
    }
    const double clamped = std::clamp(
        std::round(width * 10.0) / 10.0,
        minimum_stroke_width,
        maximum_stroke_width);
    if (current_tool_ == Tool::select) {
        restyle_selection_width(clamped, false);
        return;
    }
    current_style_.stroke_width = clamped;
    sync_toolbar();
}

void Application::adjust_context_stroke_width(const double delta)
{
    if (current_tool_ == Tool::select && !selection_.empty()) {
        restyle_selection_width(delta, true);
        return;
    }
    set_context_stroke_width(current_style_.stroke_width + delta);
}

PointerSample Application::mouse_sample(
    const float x,
    const float y,
    const std::uint64_t timestamp) const
{
    Vec2d position = camera_.screen_to_world({
        static_cast<double>(x),
        static_cast<double>(y),
    });
    position.x = std::clamp(
        position.x, -board_half_extent, board_half_extent);
    position.y = std::clamp(
        position.y, -board_half_extent, board_half_extent);
    return {
        .source = PointerSource::mouse,
        .position = position,
        .pressure = 1.0F,
        .timestamp = timestamp,
        .buttons = PointerButtons::primary,
    };
}

void Application::record_object_state(const ObjectId id)
{
    if (!board_file_.has_value()) {
        update_window_title();
        return;
    }

    if (const Object* const object = document_.find(id)) {
        board_file_->queue_put(*object);
    } else {
        board_file_->queue_delete(id);
    }
    update_window_title();
}

void Application::record_object_states(const std::vector<ObjectId>& ids)
{
    if (!board_file_.has_value()) {
        update_window_title();
        return;
    }
    for (const auto id : ids) {
        if (const Object* const object = document_.find(id)) {
            board_file_->queue_put(*object);
        } else {
            board_file_->queue_delete(id);
        }
    }
    update_window_title();
}

void Application::record_move_states(
    const std::vector<ObjectId>& ids,
    const Vec2d delta)
{
    if (!board_file_.has_value()) {
        update_window_title();
        return;
    }
    for (const auto id : ids) {
        if (document_.find(id) != nullptr) {
            board_file_->queue_move(id, delta);
        }
    }
    update_window_title();
}

void Application::record_style_states(const std::vector<ObjectId>& ids)
{
    if (!board_file_.has_value()) {
        update_window_title();
        return;
    }
    for (const auto id : ids) {
        if (const Object* const object = document_.find(id)) {
            board_file_->queue_style(id, object->style);
        }
    }
    update_window_title();
}

void Application::flush_board()
{
    if (!board_file_.has_value()) {
        return;
    }

    const auto previous_path = board_file_->path();
    board_file_->flush(document_);
    history_.mark_saved(document_);
    if (board_file_->path() != previous_path) {
        ++lifecycle_generation_;
        log::write(
            log::Level::warning,
            "The board changed externally; continuing in conflict copy "
                + board_file_->path().string());
        if (recent_files_) {
            recent_files_->touch(board_file_->path());
        }
        synchronize_navigation_target();
    }
    update_window_title();
}

bool Application::flush_board_safely() noexcept
{
    try {
        flush_board();
        status_error_.clear();
        sync_toolbar();
        return true;
    } catch (const std::exception& error) {
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        sync_toolbar();
        return false;
    }
}

void Application::autosave_if_due()
{
    if (!board_file_.has_value()) {
        return;
    }

    const bool had_pending = board_file_->has_pending_operations();
    const auto previous_path = board_file_->path();
    board_file_->flush_if_due(document_);
    if (had_pending && !board_file_->has_pending_operations()) {
        history_.mark_saved(document_);
        if (board_file_->path() != previous_path) {
            ++lifecycle_generation_;
            log::write(
                log::Level::warning,
                "Autosave detected an external change; switched to "
                    + board_file_->path().string());
            if (recent_files_) {
                recent_files_->touch(board_file_->path());
            }
            synchronize_navigation_target();
        }
        update_window_title();
    }
}

void Application::autosave_safely() noexcept
{
    const bool had_pending = board_file_.has_value()
        && board_file_->has_pending_operations();
    try {
        autosave_if_due();
        if (!status_error_.empty() && had_pending
            && board_file_.has_value()
            && !board_file_->has_pending_operations()) {
            status_error_.clear();
            sync_toolbar();
        }
    } catch (const std::exception& error) {
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        sync_toolbar();
    }
}

StyleColorTarget Application::effective_style_color_target() const noexcept
{
    if (style_color_target_ != StyleColorTarget::fill) {
        return StyleColorTarget::stroke;
    }
    if (current_tool_ == Tool::rectangle
        || current_tool_ == Tool::ellipse) {
        return StyleColorTarget::fill;
    }
    if (current_tool_ == Tool::select) {
        for (const auto id : selection_.ids()) {
            const Object* const object = document_.find(id);
            if (object == nullptr) {
                continue;
            }
            if (std::holds_alternative<RectangleShape>(object->geometry)
                || std::holds_alternative<Ellipse>(object->geometry)) {
                return StyleColorTarget::fill;
            }
        }
    }
    return StyleColorTarget::stroke;
}

void Application::sync_toolbar()
{
    std::string filename;
    if (renaming_ && rename_target_ == RenameTarget::board) {
        filename = rename_text_;
    } else if (board_file_.has_value()) {
        filename = board_file_->path().stem().string();
    } else {
        filename = untitled_name_;
    }
    SelectionStyleSummary selection_style;
    std::size_t selected_object_count = 0U;
    std::size_t fill_eligible_count = 0U;
    std::size_t filled_count = 0U;
    std::size_t rectangle_count = 0U;
    if (current_tool_ == Tool::select && !selection_.empty()) {
        for (const auto id : selection_.ids()) {
            const Object* const object = document_.find(id);
            if (object == nullptr) continue;
            ++selected_object_count;
            // Images participate in selection actions and group transforms,
            // but do not expose vector stroke/fill controls. Mixed selections
            // summarize only the vector members and mark shape-only controls
            // partial through selected_object_count below.
            if (std::holds_alternative<Image>(object->geometry)) continue;

            if (selection_style.stroke_color
                == PropertyValueState::unavailable) {
                selection_style.stroke_color = PropertyValueState::uniform;
                selection_style.stroke_color_value = object->style.stroke;
                selection_style.stroke_width = PropertyValueState::uniform;
                selection_style.stroke_width_value =
                    object->style.stroke_width;
            } else {
                if (selection_style.stroke_color_value
                    != object->style.stroke) {
                    selection_style.stroke_color = PropertyValueState::mixed;
                }
                if (std::abs(
                        selection_style.stroke_width_value
                        - object->style.stroke_width) > 0.001) {
                    selection_style.stroke_width = PropertyValueState::mixed;
                }
            }

            const bool fill_eligible =
                std::holds_alternative<RectangleShape>(object->geometry)
                || std::holds_alternative<Ellipse>(object->geometry);
            if (fill_eligible) {
                ++fill_eligible_count;
                if (object->style.fill.has_value()) {
                    ++filled_count;
                    if (selection_style.fill_color
                        == PropertyValueState::unavailable) {
                        selection_style.fill_color =
                            PropertyValueState::uniform;
                        selection_style.fill_color_value =
                            *object->style.fill;
                    } else if (selection_style.fill_color_value
                               != *object->style.fill) {
                        selection_style.fill_color =
                            PropertyValueState::mixed;
                    }
                }
            }

            if (const auto* const rectangle =
                    std::get_if<RectangleShape>(&object->geometry)) {
                ++rectangle_count;
                if (selection_style.roundness
                    == PropertyValueState::unavailable) {
                    selection_style.roundness = PropertyValueState::uniform;
                    selection_style.roundness_value = rectangle->roundness;
                } else if (std::abs(
                               selection_style.roundness_value
                               - rectangle->roundness) > 0.001) {
                    selection_style.roundness = PropertyValueState::mixed;
                }
            }
        }
    }
    if (fill_eligible_count > 0U) {
        selection_style.fill_partial =
            fill_eligible_count < selected_object_count;
        if (filled_count == 0U || filled_count == fill_eligible_count) {
            selection_style.fill = PropertyValueState::uniform;
            selection_style.fill_enabled = filled_count > 0U;
        } else {
            selection_style.fill = PropertyValueState::mixed;
        }
    }
    if (rectangle_count > 0U) {
        selection_style.roundness_partial =
            rectangle_count < selected_object_count;
    }
    const bool selection_supports_fill =
        selection_style.fill != PropertyValueState::unavailable;
    const bool selection_supports_roundness =
        selection_style.roundness != PropertyValueState::unavailable;
    const bool rectangle_roundness_mixed =
        selection_style.roundness == PropertyValueState::mixed;
    const double displayed_rectangle_roundness =
        selection_supports_roundness
        ? selection_style.roundness_value
        : rectangle_roundness_;
    toolbar_.update(
        camera_.viewport().x,
        camera_.viewport().y,
        display_scale_,
        current_tool_,
        current_style_,
        history_.can_undo() && !background_open_.valid(),
        history_.can_redo() && !background_open_.valid(),
        !selection_.empty(),
        camera_.zoom(),
        filename,
        document_.dirty(),
        status_error_,
        background_style_,
        background_color_,
        grid_color_,
        drawing_settings_,
        renaming_ && rename_target_ == RenameTarget::board,
        rename_cursor_,
        rename_anchor_,
        board_file_.has_value(),
        selection_supports_fill,
        displayed_rectangle_roundness,
        selection_supports_roundness,
        rectangle_roundness_mixed,
        selection_style,
        effective_style_color_target(),
        width_editing_,
        width_edit_text_);
    update_cursor();
}

std::optional<DrawingCursor> Application::drawing_cursor_preview() const noexcept
{
    if (view_mode_ == ViewMode::home
        || unsaved_dialog_.visible()
        || !pointer_screen_.has_value()
        || toolbar_.hovered_control() != nullptr
        || middle_button_down_
        || hand_dragging_
        || space_down_) {
        return std::nullopt;
    }
    const bool drawing_tool = current_tool_ == Tool::pencil
        || current_tool_ == Tool::line
        || current_tool_ == Tool::rectangle
        || current_tool_ == Tool::ellipse;
    if (!drawing_tool) {
        return std::nullopt;
    }
    return DrawingCursor{
        *pointer_screen_,
        std::max(1.0, current_style_.stroke_width * camera_.zoom()),
    };
}

void Application::update_cursor() noexcept
{
    const bool show_drawing_cursor = drawing_cursor_preview().has_value();
    SDL_Cursor* cursor = SDL_GetDefaultCursor();
    if (unsaved_dialog_.visible()) {
        if (unsaved_dialog_.hovered_choice().has_value() && hand_cursor_) {
            cursor = hand_cursor_.get();
        }
        static_cast<void>(SDL_SetCursor(cursor));
        static_cast<void>(SDL_ShowCursor());
        return;
    }
    if (view_mode_ == ViewMode::home) {
        if (home_view_.hovered_control() != nullptr && hand_cursor_) {
            cursor = hand_cursor_.get();
        }
        static_cast<void>(SDL_SetCursor(cursor));
        static_cast<void>(SDL_ShowCursor());
        return;
    }

    SelectionHandle resize_handle = SelectionHandle::none;
    if (selection_interaction_ == SelectionInteraction::resize) {
        resize_handle = selection_handle_;
    } else if (current_tool_ == Tool::select
               && selection_interaction_ == SelectionInteraction::none
               && toolbar_.hovered_control() == nullptr
               && pointer_screen_.has_value()) {
        resize_handle = selection_handle_at(
            document_,
            selection_,
            camera_.screen_to_world(*pointer_screen_),
            8.0 / camera_.zoom());
    }
    const bool northwest_southeast =
        resize_handle == SelectionHandle::top_left
        || resize_handle == SelectionHandle::bottom_right;
    const bool northeast_southwest =
        resize_handle == SelectionHandle::top_right
        || resize_handle == SelectionHandle::bottom_left;
    const bool actively_moving = middle_button_down_
        || hand_dragging_
        || (space_down_ && left_button_down_)
        || selection_interaction_ == SelectionInteraction::move;
    if (northwest_southeast && nwse_resize_cursor_) {
        cursor = nwse_resize_cursor_.get();
    } else if (northeast_southwest && nesw_resize_cursor_) {
        cursor = nesw_resize_cursor_.get();
    } else if (actively_moving && move_cursor_) {
        cursor = move_cursor_.get();
    } else if ((toolbar_.hovered_control() != nullptr
                || current_tool_ == Tool::hand
                || space_down_)
               && hand_cursor_) {
        cursor = hand_cursor_.get();
    }
    static_cast<void>(SDL_SetCursor(cursor));
    if (show_drawing_cursor) {
        static_cast<void>(SDL_HideCursor());
    } else {
        static_cast<void>(SDL_ShowCursor());
    }
}

void Application::tick_ui() noexcept
{
    // Restart the entrance animation whenever the visible view changes so
    // both the first frame and every home/board switch animate in.
    if (!animated_view_mode_.has_value()
        || *animated_view_mode_ != view_mode_) {
        animated_view_mode_ = view_mode_;
        if (view_mode_ == ViewMode::home) {
            home_view_.play_entrance();
        } else {
            toolbar_.play_entrance();
        }
    }
    const std::uint64_t now = SDL_GetTicksNS();
    if (last_ui_tick_ == 0U) {
        last_ui_tick_ = now;
        return;
    }
    const double elapsed = static_cast<double>(now - last_ui_tick_)
        / 1'000'000'000.0;
    last_ui_tick_ = now;
    // Keep the shared theme transition current even while Home is visible,
    // so opening a board does not replay a stale palette crossfade.
    toolbar_.tick(elapsed);
    unsaved_dialog_.tick(elapsed);
    if (view_mode_ == ViewMode::home) {
        home_view_.tick(elapsed);
    } else {
        if (const auto repeated = toolbar_.take_repeated_action()) {
            try {
                adjust_context_stroke_width(
                    adaptive_stroke_width_delta(
                        toolbar_.context_stroke_width(),
                        *repeated == UiAction::width_increase,
                        shift_down_));
            } catch (const std::exception& error) {
                status_error_ = error.what();
                log::write(log::Level::error, status_error_);
                sync_toolbar();
            }
        }
    }
}

void Application::request_quit(bool& running) noexcept
{
    quit_requested_ = true;
    if (unsaved_dialog_.visible()) {
        quit_requested_ = false;
        return;
    }
    if (dialog_active_) {
        status_error_ = "Finish the file dialog before closing.";
        if (view_mode_ == ViewMode::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
        return;
    }
    if (background_open_.valid()) {
        status_error_ = "Wait for the board to finish opening.";
        if (view_mode_ == ViewMode::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
        return;
    }
    if (has_unsaved_untitled()) {
        quit_requested_ = false;
        try {
            if (!confirm_unsaved_untitled({
                    PendingUnsavedActionKind::quit, {}, 0U})) {
                return;
            }
        } catch (const std::exception& error) {
            status_error_ = error.what();
            log::write(log::Level::error, status_error_);
            sync_toolbar();
            return;
        }
    }
    if (!flush_board_safely()) {
        quit_requested_ = false;
        return;
    }
    quit_requested_ = false;
    running = false;
}

void Application::activate_ui_action(const UiAction action)
{
    cancel_gesture();
    switch (action) {
    case UiAction::new_board:
        new_board();
        if (status_error_.empty()) {
            record_navigation_target();
        }
        return;
    case UiAction::go_home:
        enter_home();
        return;
    case UiAction::rename_board:
        start_rename_current();
        return;
    case UiAction::home_new_board:
    case UiAction::home_open_board:
        // These actions originate only from the home view, never the toolbar.
        return;
    case UiAction::open_board:
        show_board_dialog(false);
        return;
    case UiAction::save:
        static_cast<void>(flush_board_safely());
        return;
    case UiAction::save_as:
        show_board_dialog(true);
        return;
    case UiAction::select:
        current_tool_ = Tool::select;
        toolbar_.close_settings_panel();
        break;
    case UiAction::hand:
        current_tool_ = Tool::hand;
        toolbar_.close_settings_panel();
        break;
    case UiAction::pencil:
        current_tool_ = Tool::pencil;
        selection_.clear();
        toolbar_.close_settings_panel();
        break;
    case UiAction::line:
        current_tool_ = Tool::line;
        selection_.clear();
        toolbar_.close_settings_panel();
        break;
    case UiAction::rectangle:
        current_tool_ = Tool::rectangle;
        selection_.clear();
        toolbar_.close_settings_panel();
        break;
    case UiAction::ellipse:
        current_tool_ = Tool::ellipse;
        selection_.clear();
        toolbar_.close_settings_panel();
        break;
    case UiAction::color_white:
    case UiAction::color_red:
    case UiAction::color_amber:
    case UiAction::color_green:
    case UiAction::color_blue:
    case UiAction::color_violet:
    case UiAction::color_black:
        if (current_tool_ == Tool::hand
            || (current_tool_ == Tool::select && selection_.empty())) {
            return;
        }
        if (current_tool_ == Tool::select && !selection_.empty()) {
            restyle_selection(action);
            return;
        }
        if (effective_style_color_target() == StyleColorTarget::fill) {
            current_style_.fill = Toolbar::color_for(action);
        } else {
            current_style_.stroke = Toolbar::color_for(action);
        }
        break;
    case UiAction::edit_stroke_custom: {
        const StyleColorTarget target = effective_style_color_target();
        toolbar_.begin_custom_color(
            target == StyleColorTarget::fill
                ? CustomColorTarget::fill
                : CustomColorTarget::stroke,
            toolbar_.current_color());
        break;
    }
    case UiAction::color_target_stroke: {
        style_color_target_ = StyleColorTarget::stroke;
        break;
    }
    case UiAction::color_target_fill: {
        style_color_target_ = StyleColorTarget::fill;
        break;
    }
    case UiAction::fill_none:
    case UiAction::toggle_fill:
        if (current_tool_ == Tool::select && !selection_.empty()) {
            restyle_selection(action);
            return;
        }
        if (current_tool_ != Tool::rectangle
            && current_tool_ != Tool::ellipse) {
            return;
        }
        if (action == UiAction::fill_none
            || (action == UiAction::toggle_fill
                && current_style_.fill.has_value())) {
            current_style_.fill.reset();
        } else {
            current_style_.fill = current_style_.stroke;
        }
        break;
    case UiAction::width_thin:
    case UiAction::width_regular:
    case UiAction::width_bold:
    case UiAction::width_heavy:
        if (current_tool_ == Tool::hand
            || (current_tool_ == Tool::select && selection_.empty())) {
            return;
        }
        if (current_tool_ == Tool::select && !selection_.empty()) {
            restyle_selection(action);
            return;
        }
        current_style_.stroke_width = Toolbar::width_for(action);
        break;
    case UiAction::width_decrease:
        adjust_context_stroke_width(adaptive_stroke_width_delta(
            toolbar_.context_stroke_width(), false, shift_down_));
        return;
    case UiAction::width_cycle:
        start_width_edit();
        return;
    case UiAction::width_increase:
        adjust_context_stroke_width(adaptive_stroke_width_delta(
            toolbar_.context_stroke_width(), true, shift_down_));
        return;
    case UiAction::roundness_square:
    case UiAction::roundness_soft:
    case UiAction::roundness_round:
    case UiAction::roundness_full:
        if (current_tool_ == Tool::select && !selection_.empty()) {
            restyle_selection(action);
            return;
        }
        if (current_tool_ != Tool::rectangle) {
            return;
        }
        rectangle_roundness_ = Toolbar::roundness_for(action);
        break;
    case UiAction::undo:
        undo_or_redo(false);
        return;
    case UiAction::redo:
        undo_or_redo(true);
        return;
    case UiAction::copy_selection:
        static_cast<void>(copy_selection());
        return;
    case UiAction::cut_selection:
        if (copy_selection()) delete_selection();
        return;
    case UiAction::paste:
        paste_clipboard();
        return;
    case UiAction::duplicate:
        duplicate_selection();
        return;
    case UiAction::delete_selection:
        delete_selection();
        return;
    case UiAction::zoom_out:
        zoom_by_steps_at_viewport_center(-1.0);
        return;
    case UiAction::zoom_reset:
        zoom_at_viewport_center(1.0 / camera_.zoom());
        toolbar_.close_settings_panel();
        sync_toolbar();
        return;
    case UiAction::zoom_menu:
        toolbar_.toggle_settings_panel(SettingsPage::view);
        break;
    case UiAction::zoom_fit_content:
        fit_content();
        return;
    case UiAction::zoom_fit_selection:
        fit_selection();
        return;
    case UiAction::zoom_in:
        zoom_by_steps_at_viewport_center(1.0);
        return;
    case UiAction::toggle_theme: {
        toolbar_.toggle_theme();
        // Flip the neutral drawing color with the theme so freshly drawn
        // strokes stay visible against the new background. Explicit accent
        // colors (red, blue, ...) and existing objects are left untouched.
        const bool now_light = toolbar_.theme() == Theme::light;
        Color& stroke = current_style_.stroke;
        const bool neutral_light = stroke.red > 200U
            && stroke.green > 200U && stroke.blue > 200U;
        const bool neutral_dark = stroke.red < 60U
            && stroke.green < 60U && stroke.blue < 60U;
        if (now_light && neutral_light) {
            stroke = Toolbar::color_for(UiAction::color_black);
        } else if (!now_light && neutral_dark) {
            stroke = Toolbar::color_for(UiAction::color_white);
        }
        if (view_mode_ == ViewMode::home) {
            home_view_.relayout(
                camera_.viewport().x,
                camera_.viewport().y,
                display_scale_,
                toolbar_.theme());
        }
        break;
    }
    case UiAction::format_background:
        toolbar_.toggle_settings_panel(SettingsPage::canvas);
        break;
    case UiAction::settings_close:
        if (toolbar_.settings_page() == SettingsPage::color_editor) {
            if (toolbar_.custom_color_target()
                    == CustomColorTarget::stroke
                || toolbar_.custom_color_target()
                    == CustomColorTarget::fill) {
                toolbar_.close_settings_panel();
            } else {
                toolbar_.set_settings_page(SettingsPage::canvas);
            }
        } else {
            toolbar_.close_settings_panel();
        }
        break;
    case UiAction::stabilization_off:
        drawing_settings_.stabilization = StrokeStabilization::off;
        break;
    case UiAction::stabilization_light:
        drawing_settings_.stabilization = StrokeStabilization::light;
        break;
    case UiAction::stabilization_default:
        drawing_settings_.stabilization = StrokeStabilization::standard;
        break;
    case UiAction::stabilization_strong:
        drawing_settings_.stabilization = StrokeStabilization::strong;
        break;
    case UiAction::edit_background_custom:
        toolbar_.begin_custom_color(
            CustomColorTarget::background, background_color_);
        break;
    case UiAction::edit_grid_custom:
        toolbar_.begin_custom_color(
            CustomColorTarget::grid,
            grid_color_.value_or(Color{110U, 120U, 140U, 255U}));
        break;
    case UiAction::grid_color_auto:
        grid_color_.reset();
        break;
    case UiAction::grid_color_0:
    case UiAction::grid_color_1:
    case UiAction::grid_color_2:
    case UiAction::grid_color_3:
    case UiAction::grid_color_4:
    case UiAction::grid_color_5:
    case UiAction::grid_color_6:
    case UiAction::grid_color_7:
    case UiAction::grid_color_8:
    case UiAction::grid_color_9:
        grid_color_ = Toolbar::grid_color_for(action);
        break;
    case UiAction::custom_color_done:
        if (toolbar_.custom_color_target() == CustomColorTarget::stroke
            || toolbar_.custom_color_target() == CustomColorTarget::fill) {
            const Color color = toolbar_.custom_color();
            toolbar_.close_settings_panel();
            if (current_tool_ == Tool::select && !selection_.empty()) {
                restyle_selection(color);
                return;
            }
            if (toolbar_.custom_color_target() == CustomColorTarget::fill) {
                current_style_.fill = color;
            } else {
                current_style_.stroke = color;
            }
        } else {
            toolbar_.set_settings_page(SettingsPage::canvas);
        }
        break;
    case UiAction::custom_hue_field:
    case UiAction::custom_sv_field:
        return;
    case UiAction::bg_color_0:
    case UiAction::bg_color_1:
    case UiAction::bg_color_2:
    case UiAction::bg_color_3:
    case UiAction::bg_color_4:
    case UiAction::bg_color_5:
    case UiAction::bg_color_6:
    case UiAction::bg_color_7:
    case UiAction::bg_color_8:
    case UiAction::bg_color_9:
        background_color_ = Toolbar::background_color_for(action);
        break;
    case UiAction::grid_solid:
    case UiAction::grid_dot:
    case UiAction::grid_square:
    case UiAction::grid_graph:
    case UiAction::grid_hybrid:
    case UiAction::grid_diamond:
    case UiAction::grid_wide_rule:
    case UiAction::grid_triangle:
    case UiAction::grid_narrow_rule:
        background_style_ = Toolbar::style_for(action);
        break;
    case UiAction::count:
        return;
    }
    sync_toolbar();
}

void Application::apply_custom_color(
    const UiAction field, const Vec2d point)
{
    const auto color = toolbar_.update_custom_color(field, point);
    if (!color.has_value()) return;
    if (toolbar_.custom_color_target() == CustomColorTarget::background) {
        background_color_ = *color;
    } else if (toolbar_.custom_color_target() == CustomColorTarget::grid) {
        grid_color_ = *color;
    }
    sync_toolbar();
}

void Application::undo_or_redo(const bool redo)
{
    if (background_open_.valid()) {
        status_error_ = "Wait for the board to finish opening.";
        sync_toolbar();
        return;
    }
    const auto affected = redo
        ? history_.redo(document_)
        : history_.undo(document_);
    if (!affected.empty()) {
        record_object_states(affected);
        selection_.prune(document_);
    } else {
        sync_toolbar();
    }
}

void Application::zoom_at_viewport_center(const double factor)
{
    const auto viewport = camera_.viewport();
    camera_.zoom_at({viewport.x * 0.5, viewport.y * 0.5}, factor);
    sync_toolbar();
}

void Application::zoom_by_steps_at(
    const Vec2d screen_anchor,
    const double steps)
{
    const double target = stepped_zoom_target(camera_.zoom(), steps);
    camera_.zoom_at(screen_anchor, target / camera_.zoom());
    sync_toolbar();
}

void Application::zoom_by_steps_at_viewport_center(const double steps)
{
    const auto viewport = camera_.viewport();
    zoom_by_steps_at(
        {viewport.x * 0.5, viewport.y * 0.5},
        steps);
}

void Application::set_zoom(const double zoom)
{
    if (!std::isfinite(zoom) || zoom <= 0.0) {
        return;
    }
    zoom_at_viewport_center(zoom / camera_.zoom());
}

void Application::remember_current_board_zoom() noexcept
{
    if (!board_file_.has_value() || !recent_files_) {
        return;
    }
    try {
        recent_files_->set_zoom(board_file_->path(), camera_.zoom());
    } catch (const std::exception& error) {
        log::write(
            log::Level::warning,
            "Could not remember board zoom: " + std::string{error.what()});
    }
}

void Application::restore_board_zoom(const std::filesystem::path& path)
{
    const auto remembered =
        recent_files_ ? recent_files_->zoom(path) : std::nullopt;
    set_zoom(remembered.value_or(1.0));
}

void Application::fit_content()
{
    const auto objects = document_.all_objects();
    if (objects.empty()) {
        toolbar_.close_settings_panel();
        sync_toolbar();
        return;
    }
    Aabb bounds = objects.front()->bounds;
    for (std::size_t index = 1U; index < objects.size(); ++index) {
        const Aabb next = objects[index]->bounds;
        bounds.min_x = std::min(bounds.min_x, next.min_x);
        bounds.min_y = std::min(bounds.min_y, next.min_y);
        bounds.max_x = std::max(bounds.max_x, next.max_x);
        bounds.max_y = std::max(bounds.max_y, next.max_y);
    }
    const Vec2d viewport = camera_.viewport();
    camera_.frame_bounds(
        bounds,
        {72.0 * display_scale_, toolbar_.height() + 12.0 * display_scale_},
        {viewport.x - 12.0 * display_scale_,
         viewport.y - 60.0 * display_scale_},
        24.0 * display_scale_);
    toolbar_.close_settings_panel();
    sync_toolbar();
}

void Application::fit_selection()
{
    const std::optional<Aabb> bounds = selection_.bounds(document_);
    if (!bounds.has_value()) {
        toolbar_.close_settings_panel();
        sync_toolbar();
        return;
    }
    const Vec2d viewport = camera_.viewport();
    camera_.frame_bounds(
        *bounds,
        {72.0 * display_scale_, toolbar_.height() + 12.0 * display_scale_},
        {viewport.x - 12.0 * display_scale_,
         viewport.y - 60.0 * display_scale_},
        24.0 * display_scale_);
    toolbar_.close_settings_panel();
    sync_toolbar();
}

void Application::show_board_dialog(const bool save)
{
    if (dialog_active_ || background_open_.valid()) {
        return;
    }

    static constexpr SDL_DialogFileFilter filter{
        "Sawer boards",
        "sawer",
    };
    dialog_active_ = true;
    auto request = std::make_unique<DialogRequest>(DialogRequest{
        dialog_event_type_,
        save ? DialogKind::save : DialogKind::open,
        lifecycle_generation_,
        dialog_delivery_failed_,
    });
    if (save) {
        const std::string default_location = board_file_.has_value()
            ? board_file_->path().string()
            : untitled_name_ + ".sawer";
        SDL_ShowSaveFileDialog(
            board_dialog_callback,
            request.release(),
            window_.get(),
            &filter,
            1,
            default_location.c_str());
    } else {
        SDL_ShowOpenFileDialog(
            board_dialog_callback,
            request.release(),
            window_.get(),
            &filter,
            1,
            nullptr,
            false);
    }
}

void Application::handle_board_dialog_result(void* const result_pointer) noexcept
{
    const std::unique_ptr<DialogResult> result{
        static_cast<DialogResult*>(result_pointer)};
    dialog_active_ = false;
    if (!result) {
        pending_unsaved_action_.reset();
        return;
    }
    if (!result->error.empty()) {
        pending_unsaved_action_.reset();
        status_error_ = result->error;
        log::write(log::Level::error, status_error_);
        sync_toolbar();
        return;
    }
    if (!result->path.has_value()) {
        // The user dismissed the picker; stay on the current screen.
        pending_unsaved_action_.reset();
        return;
    }
    if (result->lifecycle_generation != lifecycle_generation_) {
        pending_unsaved_action_.reset();
        status_error_ =
            "The file operation was canceled because the board changed.";
        if (view_mode_ == ViewMode::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
        return;
    }

    try {
        auto path = *result->path;
        if (result->kind == DialogKind::save) {
            if (!path.has_extension()) {
                path += ".sawer";
            }
            save_as(path);
            resume_pending_unsaved_action();
        } else {
            begin_background_open(path);
            return;
        }
        status_error_.clear();
        sync_toolbar();
    } catch (const std::exception& error) {
        pending_unsaved_action_.reset();
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        sync_toolbar();
    }
}

void Application::begin_background_open(const std::filesystem::path& path)
{
    if (dialog_active_ || background_open_.valid()) {
        return;
    }
    if (!confirm_unsaved_untitled({
            PendingUnsavedActionKind::open_path, path, 0U})) {
        return;
    }
    if (!flush_board_safely()) {
        return;
    }
    cancel_gesture();
    cancel_selection_gesture();
    selection_.clear();
    background_open_origin_generation_ = lifecycle_generation_;

    background_open_ = std::async(
        std::launch::async,
        [path]() {
            BackgroundOpenResult result;
            result.path = path;
            result.session = BoardFileSession::open(
                path, result.document, result.recovered_final_line);
            return result;
        });
}

void Application::poll_background_open() noexcept
{
    if (!background_open_.valid()
        || background_open_.wait_for(std::chrono::milliseconds{0})
            != std::future_status::ready) {
        return;
    }
    try {
        const std::uint64_t origin_generation =
            background_open_origin_generation_;
        auto result = background_open_.get();
        background_open_origin_generation_ = 0U;
        if (origin_generation != lifecycle_generation_) {
            status_error_ =
                "Open was canceled because the current board changed.";
            if (view_mode_ == ViewMode::home) {
                refresh_home();
            } else {
                sync_toolbar();
            }
            return;
        }
        remember_current_board_zoom();
        document_ = std::move(result.document);
        board_file_ = std::move(result.session);
        history_ = {};
        history_.mark_saved(document_);
        view_mode_ = ViewMode::board;
        ++lifecycle_generation_;
        if (recent_files_) {
            recent_files_->touch(result.path);
        }
        restore_board_zoom(result.path);
        status_error_.clear();
        update_window_title();
        sync_toolbar();
        record_navigation_target();
        if (result.recovered_final_line) {
            log::write(
                log::Level::warning,
                "Recovered an interrupted final record while opening board");
        }
    } catch (const std::exception& error) {
        background_open_origin_generation_ = 0U;
        status_error_ = error.what();
        log::write(log::Level::error, status_error_);
        if (view_mode_ == ViewMode::home) {
            refresh_home();
        } else {
            sync_toolbar();
        }
    }
}

bool Application::poll_background_previews() noexcept
{
    bool changed = false;
    for (auto task = preview_tasks_.begin(); task != preview_tasks_.end();) {
        if (task->second.result.wait_for(std::chrono::milliseconds{0})
            != std::future_status::ready) {
            ++task;
            continue;
        }
        try {
            auto preview = std::make_shared<const BoardPreview>(
                task->second.result.get());
            preview_cache_[task->first] = PreviewCacheEntry{
                task->second.modified,
                task->second.file_size,
                std::move(preview),
            };
            changed = true;
        } catch (const std::exception& error) {
            log::write(
                log::Level::warning,
                "Could not build recent-board preview "
                    + task->first + ": " + error.what());
            // Freeing a failed worker must still schedule the next recent
            // board instead of stalling the preview queue.
            changed = true;
        }
        task = preview_tasks_.erase(task);
    }
    if (changed && view_mode_ == ViewMode::home) {
        refresh_home();
    }
    return changed;
}

void Application::update_window_title()
{
    std::string title{"Sawer"};
    if (board_file_.has_value()) {
        title += " — " + board_file_->path().stem().string();
    } else {
        title += " — " + untitled_name_;
    }
    if (document_.dirty()) {
        title += " *";
    }
    SDL_SetWindowTitle(window_.get(), title.c_str());
    sync_toolbar();
}

bool Application::update_viewport()
{
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(window_.get(), &width, &height)) {
        throw_sdl("Window-size query");
    }
    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(
            window_.get(), &pixel_width, &pixel_height)) {
        throw_sdl("Window pixel-size query");
    }
    const float reported_scale = SDL_GetWindowDisplayScale(window_.get());
    const double scale = reported_scale > 0.0F
        ? static_cast<double>(reported_scale)
        : 1.0;
    const Vec2d previous = camera_.viewport();
    const bool layout_changed =
        !viewport_initialized_
        || previous.x != static_cast<double>(width)
        || previous.y != static_cast<double>(height)
        || display_scale_ != scale;
    const bool pixel_size_changed =
        !viewport_initialized_
        || viewport_pixel_width_ != pixel_width
        || viewport_pixel_height_ != pixel_height;
    if (!layout_changed && !pixel_size_changed) {
        return false;
    }

    viewport_initialized_ = true;
    viewport_pixel_width_ = pixel_width;
    viewport_pixel_height_ = pixel_height;
    if (layout_changed) {
        camera_.set_viewport(
            static_cast<double>(width),
            static_cast<double>(height));
        display_scale_ = scale;
        if (view_mode_ == ViewMode::home) {
            home_view_.relayout(
                camera_.viewport().x,
                camera_.viewport().y,
                display_scale_,
                toolbar_.theme());
        } else {
            sync_toolbar();
        }
        if (unsaved_dialog_.visible()) {
            unsaved_dialog_.relayout(
                static_cast<double>(width),
                static_cast<double>(height),
                display_scale_);
        }
    }
    return true;
}

void Application::SdlDeleter::operator()(SDL_Window* const window) const noexcept
{
    SDL_DestroyWindow(window);
}

void Application::SdlDeleter::operator()(SDL_Cursor* const cursor) const noexcept
{
    SDL_DestroyCursor(cursor);
}

} // namespace sawer
