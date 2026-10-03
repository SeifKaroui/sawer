#include "app/Application.hpp"
#include "core/Log.hpp"
#include "renderer/BackgroundGrid.hpp"
#include "renderer/BoardTheme.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace sawer {

int Application::run_rendering_performance_test()
{
    const auto fail = [](const std::string& message) {
        log::write(log::Level::error, "Rendering performance regression: " + message);
        return 1;
    };
    // Check fixtures must be independent of the user's persisted theme.
    recent_files_.reset();
    if (toolbar_.theme() != Theme::light) toolbar_.toggle_theme();
    for (int frame = 0; frame < 4; ++frame) toolbar_.tick(0.1);
    document_ = {};
    history_ = {};
    selection_.clear();
    background_style_ = BackgroundStyle::solid;
    background_color_ = {240U, 242U, 247U, 255U};
    grid_color_ = Color{0U, 0U, 0U, 255U};
    const Color blue{30U, 90U, 210U, 255U}, red{210U, 40U, 45U, 255U};
    const Style style{blue, std::nullopt, 6.0};
    for (std::uint64_t object = 0U; object < 1'000U; ++object) {
        const double x = -300.0 + static_cast<double>(object % 40U) * 15.0;
        const double y = -200.0 + static_cast<double>(object / 40U) * 16.0;
        if (!document_.insert(Object::make_line(ObjectId::from_u64(object + 1U),
                static_cast<std::int64_t>(object), {{x, y}, {x + 8.0, y}}, style)))
            return fail("fixture insertion");
    }
    sync_toolbar();
    const ObjectDraft* render_preview = nullptr;
    const SelectionPreview* render_selection_preview = nullptr;
    const auto render = [&] {
        for (std::size_t attempt = 0U; attempt < 20U; ++attempt) {
            SDL_PumpEvents();
            if (renderer_->render(camera_, document_, render_preview, toolbar_, selection_,
                    nullptr, render_selection_preview)) return true;
            SDL_Delay(1U);
        }
        return false;
    };
    const auto pixel = [&](const Vec2d world, const Color expected) {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window_.get(), &width, &height)) return false;
        const auto screen = camera_.world_to_screen(world);
        const auto viewport = camera_.viewport();
        const auto x = static_cast<std::uint32_t>(std::clamp(screen.x * width / viewport.x, 0.0,
            static_cast<double>(width - 1)));
        const auto y = static_cast<std::uint32_t>(std::clamp(screen.y * height / viewport.y, 0.0,
            static_cast<double>(height - 1)));
        renderer_->request_rendered_pixel(x, y);
        if (!render()) return false;
        const auto rgba = renderer_->read_rendered_pixel(x, y);
        return rgba && std::abs(static_cast<int>((*rgba)[0]) - expected.red) <= 10
            && std::abs(static_cast<int>((*rgba)[1]) - expected.green) <= 10
            && std::abs(static_cast<int>((*rgba)[2]) - expected.blue) <= 10;
    };
    if (!render() || renderer_->stats().resident_upload_bytes == 0U
        || renderer_->stats().resident_pages == 0U || !pixel({-296.0, -200.0}, blue))
        return fail("resident mesh first frame or output");
    std::vector<double> frame_work;
    for (std::size_t frame = 0U; frame < 30U; ++frame) {
        camera_.pan_by_screen_delta({1.0, 0.5});
        if (!render()) return fail("pan presentation");
        const auto& stats = renderer_->stats();
        if (stats.scene_upload_bytes != 0U || stats.tessellated_objects != 0U
            || stats.visibility_query_reuses != 1U || stats.resident_draws == 0U)
            return fail("warm pan repeated mesh or spatial work");
        // Recording includes swapchain acquisition; exclude its presentation
        // wait rather than labeling the nested wait as CPU construction work.
        frame_work.push_back(stats.build_milliseconds + stats.staging_milliseconds
            + std::max(0.0, stats.command_record_milliseconds - stats.swapchain_wait_milliseconds
                - stats.batch_wait_milliseconds));
    }
    camera_.zoom_at({camera_.viewport().x * 0.5, camera_.viewport().y * 0.5}, 1.0001);
    if (!render() || renderer_->stats().scene_upload_bytes != 0U
        || renderer_->stats().tessellated_objects != 0U) return fail("same-detail zoom uploaded meshes");
    const auto first = ObjectId::from_u64(1U);
    auto changed = style;
    changed.stroke = red;
    history_.execute(std::make_unique<ChangeStyleCommand>(first, changed), document_);
    if (!render() || renderer_->stats().tessellated_objects != 1U
        || renderer_->stats().resident_upload_bytes == 0U
        || renderer_->stats().resident_upload_bytes > 4'096U
        || renderer_->stats().visibility_query_reuses != 1U
        || !pixel({-296.0, -200.0}, red)) return fail("single style update");
    history_.execute(std::make_unique<MoveObjectCommand>(first, Vec2d{0.0, 8.0}), document_);
    if (!render() || renderer_->stats().tessellated_objects != 1U
        || !pixel({-296.0, -192.0}, red) || !pixel({-296.0, -200.0}, background_color_))
        return fail("single translation update");
    history_.execute(std::make_unique<DeleteObjectCommand>(first), document_);
    if (!render() || renderer_->stats().scene_upload_bytes != 0U
        || !pixel({-296.0, -192.0}, background_color_)) return fail("deletion retirement");
    if (history_.undo(document_).empty() || !pixel({-296.0, -192.0}, red)
        || history_.redo(document_).empty() || !pixel({-296.0, -192.0}, background_color_))
        return fail("undo redo mesh invalidation");
    for (std::size_t edit = 0U; edit < 180U; ++edit) {
        const auto id = ObjectId::from_u64(2U + edit % 50U);
        if (!document_.translate(id, {0.001, 0.0}) || !render()) return fail("repeated edits");
        if (renderer_->stats().resident_pages > 8U || renderer_->stats().tessellated_objects != 1U)
            return fail("repeated edits grew GPU residency");
    }
    std::sort(frame_work.begin(), frame_work.end());
    std::ostringstream summary;
    summary << "retained-pan objects=1000, frames=30, upload-bytes=0, tessellations=0, "
        << "CPU-work-p50-ms=" << frame_work[frame_work.size() / 2U]
        << ", CPU-work-p95-ms=" << frame_work[frame_work.size() * 95U / 100U]
        << ", pages=" << renderer_->stats().resident_pages;
    log::write(log::Level::info, summary.str());

    // Returning to a warm zoom bucket must reuse the stroke mesh on CPU and GPU.
    document_ = {};
    history_ = {};
    camera_ = Camera{camera_.viewport().x, camera_.viewport().y};
    Stroke stroke;
    for (std::size_t point = 0U; point < 2'000U; ++point)
        stroke.points.push_back({-120.0 + static_cast<double>(point) * 0.1,
            std::sin(static_cast<double>(point) * 0.02) * 20.0});
    if (!document_.insert(Object::make_stroke(first, 0, std::move(stroke),
            {blue, std::nullopt, 3.0}))) return fail("detail fixture");
    sync_toolbar();
    if (!render()) return fail("initial detail");
    const auto anchor = Vec2d{camera_.viewport().x * 0.5, camera_.viewport().y * 0.5};
    camera_.zoom_at(anchor, 0.99);
    if (!render()) return fail("lower detail");
    camera_.zoom_at(anchor, 1.1 / camera_.zoom());
    if (!render()) return fail("higher detail");
    for (const double zoom : {0.8, 1.1, 0.8, 1.1}) {
        camera_.zoom_at(anchor, zoom / camera_.zoom());
        if (!render() || renderer_->stats().tessellated_objects != 0U
            || renderer_->stats().resident_upload_bytes != 0U) return fail("detail variant oscillation");
    }

    // Actual shader output at lattice anchors and blank cells, accounting for
    // the window's physical-to-logical drawable scaling.
    document_ = {};
    camera_ = Camera{camera_.viewport().x, camera_.viewport().y};
    for (const bool msaa : {true, false}) {
        renderer_.reset();
        renderer_ = std::make_unique<GpuRenderer>(*window_, msaa);
        document_ = {};
        background_style_ = BackgroundStyle::solid;
        background_color_ = {255, 255, 255, 255};
        grid_color_.reset();
        camera_ = Camera{camera_.viewport().x, camera_.viewport().y};
        const Color white{255, 255, 255, 255}, black{30, 34, 42, 255};
        if (!document_.insert(Object::make_line(ObjectId::from_u64(1), 0,
                {{-120, -80}, {120, -80}}, {black, std::nullopt, 8}))
            || !document_.insert(Object::make_rectangle(ObjectId::from_u64(2), 1,
                {{-100, -50}, {-50, 0}}, {black, white, 8}))
            || !document_.insert(Object::make_line(ObjectId::from_u64(3), 2,
                {{-120, 30}, {120, 30}}, {blue, std::nullopt, 8}))
            // Wide geometry exercises the camera-rebased streaming path.
            || !document_.insert(Object::make_line(ObjectId::from_u64(4), 3,
                {{-3000, 65}, {3000, 65}}, {black, std::nullopt, 8}))
            || !document_.insert(Object::make_line(ObjectId::from_u64(5), 4,
                {{60, 30}, {110, 30}}, {white, std::nullopt, 8})))
            return fail("theme fixture insertion");
        sync_toolbar();
        if (!pixel({0, -80}, black) || !pixel({-75, -25}, white)) return fail("light theme pixels");
        const auto original_revision = document_.revision();
        const auto original_dirty = document_.dirty();
        toolbar_.toggle_theme();
        for (int frame = 0; frame < 4; ++frame) toolbar_.tick(0.1);
        if (!render() || renderer_->stats().tessellated_objects != 0U
            || renderer_->stats().resident_upload_bytes != 0U)
            return fail("theme switch rebuilt retained meshes");
        if (!pixel({0, -80}, dark_board_ink) || !pixel({-75, -25}, dark_board_paper)
            || !pixel({0, 30}, blue) || !pixel({0, 65}, dark_board_ink)
            || !pixel({80, 30}, dark_board_paper)
            || !pixel({150, -50}, dark_board_paper)) return fail("dark theme pixels");
        ObjectDraft draft{Stroke{{{-20, 0}, {20, 0}}}, {black, std::nullopt, 8}, 100, 1};
        render_preview = &draft;
        if (!pixel({0, 0}, dark_board_ink)) return fail("dark live stroke");
        draft = {RectangleShape{{-20, -20}, {20, 20}}, {black, white, 8}, 101, 1};
        if (!pixel({0, 0}, dark_board_paper) || !pixel({0, -20}, dark_board_ink))
            return fail("dark shape draft fill and stroke");
        render_preview = nullptr;
        SelectionPreview moved{{ObjectId::from_u64(1)}, {{0, 0}, {0, 80}, 1, 1}};
        render_selection_preview = &moved;
        if (!pixel({0, 0}, dark_board_ink)) return fail("dark selection drag preview");
        render_selection_preview = nullptr;
        if (document_.revision() != original_revision || document_.dirty() != original_dirty
            || document_.find(ObjectId::from_u64(1))->style.stroke != black
            || document_.find(ObjectId::from_u64(2))->style.fill != white
            || background_color_ != white) return fail("theme switch changed saved colors");
        toolbar_.toggle_theme();
        for (int frame = 0; frame < 4; ++frame) toolbar_.tick(0.1);
        if (!pixel({0, -80}, black) || !pixel({-75, -25}, white)
            || !pixel({150, -50}, white)) return fail("light theme restoration");
        const Color cream{245, 232, 150, 255};
        background_color_ = cream;
        toolbar_.toggle_theme();
        for (int frame = 0; frame < 4; ++frame) toolbar_.tick(0.1);
        sync_toolbar();
        if (!pixel({150, -50}, cream) || !pixel({0, -80}, black)
            || !pixel({-75, -25}, white)) return fail("fixed colored background");
        toolbar_.toggle_theme();
        for (int frame = 0; frame < 4; ++frame) toolbar_.tick(0.1);
        document_ = {};
        background_color_ = {240, 242, 247, 255};
        grid_color_ = Color{0, 0, 0, 255};
        camera_.zoom_at(anchor, 8.0 / camera_.zoom());
        for (const auto pattern : {BackgroundStyle::solid, BackgroundStyle::dot,
                BackgroundStyle::square, BackgroundStyle::graph, BackgroundStyle::hybrid,
                BackgroundStyle::diamond, BackgroundStyle::wide_rule,
                BackgroundStyle::triangle, BackgroundStyle::narrow_rule}) {
            background_style_ = pattern;
            sync_toolbar();
            if (!render()) return fail("background pattern presentation");
            if (renderer_->stats().scene_upload_bytes != 0U || renderer_->stats().background_draw_calls != 1U)
                return fail("background uploaded triangles or added draws");
            Color anchor_color = background_color_;
            if (pattern == BackgroundStyle::dot || pattern == BackgroundStyle::hybrid)
                anchor_color = {53U, 53U, 54U, 255U};
            else if (pattern == BackgroundStyle::square || pattern == BackgroundStyle::graph)
                anchor_color = {0U, 0U, 0U, 255U};
            else if (pattern != BackgroundStyle::solid) anchor_color = {84U, 85U, 86U, 255U};
            if (!pixel({0.0, 0.0}, anchor_color)
                || !pixel({13.225, 21.275}, background_color_)) return fail("background anchor or blank cell");
            if (pattern == BackgroundStyle::solid) {
                if (!pixel({0.0, 0.0}, background_color_)) return fail("solid background pixel");
            } else if (pattern == BackgroundStyle::dot) {
                const Color dot{53U, 53U, 54U, 255U};
                if (!pixel({0.0, 0.0}, dot) || !pixel({20.0, 20.0}, background_color_))
                    return fail("dot anchor or blank-cell pixel");
                camera_.pan_by_screen_delta({17.25, -8.5});
                if (!pixel({0.0, 0.0}, dot)) return fail("grid drift after pan");
                camera_.zoom_at(anchor, 1.25);
                if (!pixel({0.0, 0.0}, dot)) return fail("grid drift after zoom");
                camera_ = Camera{camera_.viewport().x, camera_.viewport().y};
                camera_.zoom_at(anchor, 8.0);
            }
        }
    }
    if (!SDL_SetWindowSize(window_.get(), 3840, 2160) || !SDL_SyncWindow(window_.get()))
        return fail("4K drawable resize");
    static_cast<void>(update_viewport());
    background_style_ = BackgroundStyle::graph;
    sync_toolbar();
    if (!render() || renderer_->stats().background_draw_calls != 1U
        || renderer_->stats().scene_upload_bytes != 0U) return fail("4K graph work scaled with grid density");
    int drawable_width = 0, drawable_height = 0;
    if (!SDL_GetWindowSizeInPixels(window_.get(), &drawable_width, &drawable_height))
        return fail("4K drawable dimensions");
    log::write(log::Level::info, "grid-drawable=" + std::to_string(drawable_width)
        + "x" + std::to_string(drawable_height) + ", background-draws=1, scene-upload-bytes=0");
    document_ = {};
    background_style_ = BackgroundStyle::solid;
    const Line distant{{998'999.5, 998'999.9375}, {999'000.5, 998'999.9375}};
    if (!document_.insert(Object::make_line(first, 0, distant, {blue, std::nullopt, 0.04})))
        return fail("precision fixture insertion");
    const auto bounds = document_.find(first)->bounds;
    camera_.frame_bounds({bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y},
        {0, 0}, camera_.viewport(), 20.0);
    sync_toolbar();
    if (!pixel({999'000.0, 998'999.9375}, blue) || renderer_->stats().resident_draws == 0U)
        return fail("resident float precision at distant coordinates");
    if (!renderer_->render(camera_, document_, nullptr, toolbar_, selection_, &home_view_)
        || renderer_->stats().resident_pages != 0U || renderer_->stats().cache_bytes != 0U)
        return fail("Home residency release");
    log::write(log::Level::info, "Rendering navigation, edits, detail reuse and all grid patterns passed");
    return 0;
}

} // namespace sawer
