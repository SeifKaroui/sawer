#include "app/Application.hpp"

#include "core/Log.hpp"
#include "image/Sha256.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>

namespace sawer {

int Application::run_board_loading_test()
{
    // Regression checks must not change the user's recent-file preferences.
    recent_files_.reset();
    const auto path = std::filesystem::temp_directory_path()
        / ("sawer-loading-" + ObjectId::random().to_string() + ".sawer");
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};

    DecodedImage pixels{1024U, 1024U, std::vector<std::uint8_t>(1024U * 1024U * 4U)};
    for (std::size_t index = 0U; index < pixels.rgba.size(); index += 4U) {
        pixels.rgba[index] = 32U;
        pixels.rgba[index + 1U] = 120U;
        pixels.rgba[index + 2U] = 240U;
        pixels.rgba[index + 3U] = 255U;
    }
    auto asset = std::make_shared<ImageAsset>();
    asset->pixel_width = pixels.width;
    asset->pixel_height = pixels.height;
    asset->png = encode_png_rgba(pixels);
    asset->id = sha256(asset->png);
    Document original;
    for (std::uint64_t id = 1U; id <= 2U; ++id) {
        if (!original.insert(Object::make_image(ObjectId::from_u64(id),
                static_cast<std::int64_t>(id), {asset, {-200.0, -100.0}, {200.0, 100.0}}))) {
            return 1;
        }
    }
    static_cast<void>(BoardFileSession::create(path, original));
    // A cold first render establishes the normal fallback and sampled output.
    bool recovered = false;
    board_file_ = BoardFileSession::open(path, document_, recovered);
    history_.mark_saved(document_);
    view_mode_ = ViewMode::board;
    sync_toolbar();
    const auto sample_x = static_cast<std::uint32_t>(camera_.viewport().x / 2.0);
    const auto sample_y = static_cast<std::uint32_t>(camera_.viewport().y / 2.0);
    renderer_->request_rendered_pixel(sample_x, sample_y);
    if (run_frame_test(1U) != 0 || renderer_->stats().image_decodes != 1U
        || renderer_->stats().image_texture_cache_entries != 1U) {
        log::write(log::Level::error, "Cold image loading did not decode exactly one shared asset");
        return 1;
    }
    const auto expected_pixel = renderer_->read_rendered_pixel(sample_x, sample_y);
    if (!expected_pixel) return 1;
    const double cold_objects_ms = renderer_->stats().objects_milliseconds;

    // Recreate GPU resources so a retained texture cannot mask a missed handoff.
    renderer_.reset();
    renderer_ = std::make_unique<GpuRenderer>(*window_);
    open_board(path);
    renderer_->request_rendered_pixel(sample_x, sample_y);
    if (run_frame_test(1U) != 0 || renderer_->stats().image_decodes != 0U
        || renderer_->stats().image_decode_cache_bytes != pixels.rgba.size()
        || renderer_->read_rendered_pixel(sample_x, sample_y) != expected_pixel) {
        log::write(log::Level::error, "Synchronous open did not reuse validated pixels with identical output");
        return 1;
    }
    const double reused_objects_ms = renderer_->stats().objects_milliseconds;
    const auto wait_for_open = [this]() {
        const Uint64 deadline = SDL_GetTicks() + 5'000U;
        while (background_open_.valid() && SDL_GetTicks() < deadline) {
            poll_background_open();
            SDL_Delay(1U);
        }
        return !background_open_.valid();
    };
    for (unsigned index = 0U; index < 5U; ++index) {
        renderer_.reset();
        renderer_ = std::make_unique<GpuRenderer>(*window_);
        begin_background_open(path);
        if (!wait_for_open() || !status_error_.empty()) return 1;
        renderer_->request_rendered_pixel(sample_x, sample_y);
        if (run_frame_test(1U) != 0 || renderer_->stats().image_decodes != 0U
            || renderer_->stats().image_decode_cache_bytes != pixels.rgba.size()
            || renderer_->read_rendered_pixel(sample_x, sample_y) != expected_pixel) {
            log::write(log::Level::error, "Background open lost decoded pixels or changed rendered output");
            return 1;
        }
    }
    const auto revision = document_.revision();
    // A rejected result must leave the current document and its cache intact.
    begin_background_open(path);
    ++lifecycle_generation_;
    if (!wait_for_open() || status_error_.empty()
        || document_.revision() != revision || run_frame_test(1U) != 0
        || renderer_->stats().image_decode_cache_bytes != pixels.rgba.size()) {
        log::write(log::Level::error, "Superseded open changed the active image cache");
        return 1;
    }
    auto missing = path;
    missing += ".missing";
    begin_background_open(missing);
    if (!wait_for_open() || status_error_.empty()
        || document_.revision() != revision || run_frame_test(1U) != 0
        || renderer_->stats().image_decode_cache_bytes != pixels.rgba.size()) {
        log::write(log::Level::error, "Failed open changed the active image cache");
        return 1;
    }
    std::printf("board-loading cold_object_preparation_ms=%.3f reused_object_preparation_ms=%.3f "
                "retained_decoded_bytes=%zu first_render_decodes=1->0\n",
        cold_objects_ms, reused_objects_ms, pixels.rgba.size());
    board_file_.reset();
    return 0;
}

} // namespace sawer
