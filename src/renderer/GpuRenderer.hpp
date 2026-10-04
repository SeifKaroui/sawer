#pragma once

#include "document/ObjectId.hpp"
#include "core/ObjectRecency.hpp"
#include "renderer/VisibleObjectCache.hpp"
#include "renderer/MeshArena.hpp"
#include "geometry/Geometry.hpp"
#include "image/ImageDecodeCache.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct SDL_GPUBuffer;
struct SDL_GPUFence;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUDevice;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUTransferBuffer;
struct SDL_GPUSampler;
struct SDL_GPUTexture;
struct SDL_Window;
struct TTF_Font;
struct TTF_Text;
struct TTF_TextEngine;

namespace sawer {

struct UiControl;

// Contiguous run of geometry vertices drawn with one constant opacity.
// Spans with alpha 1 use the opaque pipeline; anything lower is drawn with
// the constant-factor blend pipeline so shadows, glows, and scrims can
// composite over arbitrary canvas content without per-vertex alpha.
struct GeometrySpan final {
    std::uint32_t first_vertex{};
    float alpha{1.0F};
    std::optional<std::array<double, 4>> clip{};
};

enum class TextStyle {
    regular,
    secondary,
    bold,
    title,
    document,
    caption,
};

// Camera-independent retained vertex. World positions stay double precision;
// byte colors avoid spending three doubles on immutable 8-bit style data.
struct CachedWorldVertex final {
    Vec2d position;
    std::array<std::uint8_t, 4> color;
};

// Camera-relative GPU vertex. Z is always zero for Sawer's 2D geometry, and
// colors originate as 8-bit channels, so neither needs a float lane.
struct GeometryVertex final {
    std::array<float, 2> position;
    std::array<std::uint8_t, 4> color;
};

// A GPU-drawn pointer indicator. Unlike an operating-system cursor it stays
// sharp at every DPI and can communicate the active drawing diameter.
struct DrawingCursor final {
    Vec2d screen_position;
    double diameter{};
};

struct RendererStats final {
    std::uint64_t frame{};
    std::size_t visible_objects{};
    std::size_t visibility_query_reuses{};
    std::size_t rendered_objects{};
    std::size_t scene_batches{};
    std::size_t batch_reuse_waits{};
    double batch_wait_milliseconds{};
    std::size_t peak_scene_batch_vertices{};
    std::size_t peak_stroke_page_vertices{};
    std::size_t tessellated_objects{};
    std::size_t cache_hits{};
    std::size_t emitted_vertices{};
    std::size_t cache_entries{};
    std::size_t cache_bytes{};
    std::size_t cache_metadata_bytes{};
    std::size_t evictions{};
    std::size_t segment_index_builds{};
    std::size_t scene_upload_bytes{};
    std::size_t resident_upload_bytes{};
    std::size_t resident_draws{};
    std::size_t resident_pages{};
    std::size_t draft_upload_bytes{};
    std::size_t geometry_upload_bytes{};
    std::size_t text_upload_bytes{};
    std::size_t thumbnail_upload_bytes{};
    std::size_t image_upload_bytes{};
    std::size_t cpu_geometry_live_bytes{};
    std::size_t cpu_geometry_capacity_bytes{};
    std::size_t gpu_geometry_capacity_bytes{};
    std::size_t transfer_capacity_bytes{};
    std::size_t text_geometry_capacity_bytes{};
    std::size_t query_scratch_capacity_bytes{};
    std::size_t text_cache_entries{};
    std::size_t thumbnail_cache_entries{};
    std::size_t image_texture_cache_entries{};
    std::size_t image_texture_tiles{};
    std::size_t image_texture_cache_bytes{};
    std::size_t image_texture_evictions{};
    std::size_t image_decodes{};
    std::size_t image_decode_cache_bytes{};
    std::size_t draw_calls{};
    std::size_t background_draw_calls{};
    std::size_t text_draw_calls{};
    std::size_t tooltip_draw_calls{};
    std::size_t draft_full_rebuilds{};
    std::size_t draft_appended_points{};
    double build_milliseconds{};
    double background_milliseconds{};
    double query_milliseconds{};
    double objects_milliseconds{};
    double overlay_ui_milliseconds{};
    double staging_milliseconds{};
    double command_acquire_milliseconds{};
    double swapchain_wait_milliseconds{};
    double command_record_milliseconds{};
    double submit_milliseconds{};
    double total_cpu_milliseconds{};
    bool scene_rebuilt{};
};

class Camera;
class Document;
class Selection;
class StrokeSegmentIndex;
struct SelectionPreview;
class Toolbar;
class HomeView;
class UnsavedDialog;
struct BoardPreview;
struct ObjectDraft;
struct UiRect;

class GpuRenderer final {
public:
    explicit GpuRenderer(SDL_Window& window, bool enable_antialiasing = true);
    ~GpuRenderer();

    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;
    GpuRenderer(GpuRenderer&&) = delete;
    GpuRenderer& operator=(GpuRenderer&&) = delete;

    // Returns false when presentation is temporarily unavailable, such as
    // while the window is minimized.
    // When home is non-null the full-screen home gallery is drawn instead of
    // the board and toolbar.
    [[nodiscard]] bool render(
        const Camera& camera,
        const Document& document,
        const ObjectDraft* preview,
        const Toolbar& toolbar,
        const Selection& selection,
        const HomeView* home = nullptr,
        const SelectionPreview* selection_preview = nullptr,
        const UnsavedDialog* unsaved_dialog = nullptr,
        const DrawingCursor* drawing_cursor = nullptr,
        double navigation_opacity = 0.0);

    // Interactive-resize mode: frames present immediately (no vsync stall).
    // MSAA remains active through a capacity-reserved offscreen target. Ends
    // automatically via the main loop.
    // A loaded document may reuse the previous document's revision and IDs.
    // Drop retained meshes/draw descriptors before rendering its replacement.
    void invalidate_document_cache() noexcept;
    // Called on the main thread after accepting a successfully loaded board.
    // Moves validated pixels without copying them or creating GPU resources.
    void adopt_decoded_images(ImageDecodeCache images) noexcept;
    void begin_live_resize() noexcept;
    void end_live_resize() noexcept;
    // Event watches may be invoked from inside SDL presentation calls during
    // a native window resize. The application uses this to avoid recursively
    // entering render() with the same CPU/GPU staging buffers.
    [[nodiscard]] bool rendering() const noexcept;
    [[nodiscard]] const std::string& diagnostics() const noexcept;
    [[nodiscard]] const RendererStats& stats() const noexcept;
    // Opt-in diagnostic capture of the next frame after all canvas/UI draws.
    // The captured final surface is also the source of the presentation blit.
    void request_rendered_pixel(std::uint32_t x, std::uint32_t y);
    [[nodiscard]] std::optional<std::array<std::uint8_t, 4>> read_rendered_pixel(
        std::uint32_t x, std::uint32_t y) const noexcept;

    // Maps a screen-space x coordinate to the nearest byte offset in the
    // filename currently being edited, using the glyph positions recorded
    // during the last frame's layout. Returns nullopt when the filename is
    // not being edited (no layout has been recorded).
    [[nodiscard]] std::optional<std::size_t> filename_index_at_x(
        double screen_x) const noexcept;

private:
    struct CachedMesh {
        std::uint64_t revision{};
        std::uint32_t detail{};
        int stroke_detail{};
        bool paged{};
        std::int64_t region_min_x{};
        std::int64_t region_min_y{};
        std::int64_t region_max_x{};
        std::int64_t region_max_y{};
        bool region_limited{};
        std::uint64_t last_used_frame{};
        std::vector<CachedWorldVertex> vertices;
        std::vector<MeshRange> resident;
        Vec2d origin{};
        std::uint64_t last_submission{};
    };
    struct CachedGeometry final : CachedMesh {
        std::optional<CachedMesh> alternate;
        [[nodiscard]] std::size_t capacity_bytes() const noexcept
        { return (vertices.capacity() + (alternate ? alternate->vertices.capacity() : 0U))
            * sizeof(CachedWorldVertex); }
    };

    struct TextVertex final {
        std::array<float, 2> position;
        std::array<std::uint8_t, 4> color;
        std::array<float, 2> uv;
    };

    struct ImageVertex final {
        std::array<float, 2> position;
        std::array<std::uint8_t, 4> color;
        std::array<float, 2> uv;
    };

    struct SceneDraw final {
        enum class Kind { vector, image, resident };
        Kind kind{Kind::vector};
        std::uint32_t first_vertex{};
        std::uint32_t vertex_count{};
        SDL_GPUTexture* texture{};
        const struct Object* object{};
        SDL_GPUBuffer* buffer{};
        Vec2d origin{};
    };

    struct ImageTile final {
        SDL_GPUTexture* texture{};
        std::uint32_t pixel_x{};
        std::uint32_t pixel_y{};
        std::uint32_t pixel_width{};
        std::uint32_t pixel_height{};
        std::uint32_t mip_levels{1U};
    };

    struct CachedImage final {
        std::weak_ptr<const ImageAsset> owner;
        std::vector<ImageTile> tiles;
        std::uint64_t last_used_frame{};
        std::size_t gpu_bytes{};
    };

    struct ImageUpload final {
        SDL_GPUTexture* texture{};
        std::uint32_t data_offset{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t mip_levels{1U};
    };

    struct TextBatch final {
        SDL_GPUTexture* texture{};
        std::uint32_t first_index{};
        std::int32_t vertex_offset{};
        std::uint32_t index_count{};
        std::optional<std::array<double, 4>> clip{};
    };

    struct CachedText final {
        TTF_Text* text{};
        std::uint64_t last_used_frame{};
    };

    struct ThumbnailTexture final {
        SDL_GPUTexture* texture{};
        std::weak_ptr<const BoardPreview> owner;
        std::uint64_t last_used_frame{};
        bool uploaded{};
        std::uint8_t dark_amount{};
    };

    struct ThumbnailUpload final {
        SDL_GPUTexture* texture{};
        const BoardPreview* key{};
        std::uint32_t data_offset{};
    };

    void create_geometry_buffers();
    [[nodiscard]] CachedGeometry& cached_geometry_for(
        const struct Object& object, const Document& document,
        const Camera& camera, const Aabb& visible);
    void render_scene_batches(
        const Camera& camera, const Document& document, const Toolbar& toolbar,
        std::uint32_t width, std::uint32_t height);
    void release_batch_slots() noexcept;
    [[nodiscard]] bool ensure_vertex_buffer_capacity(
        SDL_GPUBuffer*& buffer,
        std::size_t& capacity_vertices,
        std::size_t required_vertices,
        std::string_view label);
    void ensure_transfer_capacity(std::size_t required_bytes);
    void create_pipeline();
    void create_background_pipeline();
    void create_retained_pipeline();
    [[nodiscard]] bool ensure_resident_mesh(CachedMesh& mesh, const Object& object);
    void release_mesh(CachedMesh& mesh);
    void release_mesh_storage() noexcept;
    void reap_mesh_submissions();
    void sync_document_changes(const Document& document);
    [[nodiscard]] std::size_t geometry_metadata_bytes() const noexcept;
    void draw_resident(SDL_GPUCommandBuffer* command, SDL_GPURenderPass* pass,
        const SceneDraw& draw, const Camera& camera, bool multisampled);
    void draw_background(SDL_GPUCommandBuffer* command, SDL_GPURenderPass* pass,
        const Camera& camera, const Toolbar& toolbar, std::uint32_t width, std::uint32_t height,
        bool multisampled);
    void create_image_resources();
    [[nodiscard]] bool ensure_image_vertex_buffer_capacity(
        std::size_t required_vertices);
    [[nodiscard]] CachedImage& ensure_image_texture(
        const std::shared_ptr<const ImageAsset>& asset);
    void append_image_draws(
        const Image& image,
        const Camera& camera);
    void prune_image_cache();
    void create_text_resources();
    void ensure_msaa_target(
        std::uint32_t width, std::uint32_t height, bool force = false);
    void build_text_geometry(
        const Toolbar& toolbar,
        const Camera& camera,
        double background_opacity = 1.0);
    void build_home_text_geometry(
        const HomeView& home,
        const Camera& camera);
    void build_unsaved_dialog_text_geometry(
        const UnsavedDialog& dialog,
        const Toolbar& toolbar);
    [[nodiscard]] double prepare_text_frame(
        double viewport_width,
        double viewport_height,
        double scale);
    void queue_text(
        std::string_view text,
        double x,
        double y,
        std::array<float, 4> color,
        TextStyle style = TextStyle::regular,
        double wrap_width = 0.0);
    [[nodiscard]] TTF_Font* text_font(TextStyle style) const noexcept;
    [[nodiscard]] Vec2d tooltip_text_extent(const Toolbar& toolbar, const UiControl& control);
    [[nodiscard]] SDL_GPUTexture* ensure_thumbnail_texture(
        const std::shared_ptr<const BoardPreview>& preview, std::uint8_t dark_amount);
    void queue_home_thumbnail(
        const std::shared_ptr<const BoardPreview>& preview,
        UiRect bounds,
        std::array<float, 4> color,
        std::uint8_t dark_amount);
    [[nodiscard]] double measure_text_width(
        std::string_view text,
        TextStyle style = TextStyle::regular);
    [[nodiscard]] std::string fit_text_to_width(
        std::string_view text,
        double maximum_width,
        TextStyle style = TextStyle::regular);
    void build_board_geometry(
        const Camera& camera,
        const Document& document,
        const ObjectDraft* preview,
        const Toolbar& toolbar,
        const Selection& selection,
        const SelectionPreview* selection_preview,
        const DrawingCursor* drawing_cursor);
    void collect_diagnostics();
    [[nodiscard]] bool tessellate_indexed_stroke(
        std::vector<CachedWorldVertex>& output,
        const struct Stroke& stroke,
        const struct Style& style,
        std::uint32_t detail,
        double zoom,
        const struct Aabb& visible,
        const StrokeSegmentIndex& index);
    [[nodiscard]] bool visit_indexed_stroke(
        const struct Stroke& stroke, const struct Style& style,
        std::uint32_t detail, double zoom, const Aabb& visible,
        const StrokeSegmentIndex& index,
        const std::function<bool(std::span<const CachedWorldVertex>)>& consume);
    void prune_cache(const ObjectId* protected_id = nullptr);
    void trim_board_caches_for_home();
    void release() noexcept;

    SDL_Window* window_{};
    SDL_GPUDevice* device_{};
    SDL_GPUGraphicsPipeline* pipeline_{};
    SDL_GPUGraphicsPipeline* blend_pipeline_{};
    SDL_GPUGraphicsPipeline* background_pipeline_{};
    SDL_GPUGraphicsPipeline* background_pipeline_direct_{};
    SDL_GPUGraphicsPipeline* retained_pipeline_{};
    SDL_GPUGraphicsPipeline* retained_pipeline_direct_{};
    // Single-sample variants used when the device cannot support MSAA.
    SDL_GPUGraphicsPipeline* pipeline_direct_{};
    SDL_GPUGraphicsPipeline* blend_pipeline_direct_{};
    SDL_GPUGraphicsPipeline* text_pipeline_direct_{};
    SDL_GPUBuffer* scene_vertex_buffer_{};
    struct BatchSlot final {
        SDL_GPUBuffer* vertices{};
        SDL_GPUTransferBuffer* transfer{};
        SDL_GPUFence* fence{};
    };
    std::array<BatchSlot, 2> batch_slots_{};
    std::size_t next_batch_slot_{};
    SDL_GPUTexture* scene_stream_texture_{};
    SDL_GPUTexture* pixel_readback_surface_{};
    SDL_GPUTransferBuffer* pixel_readback_transfer_{};
    std::uint32_t pixel_readback_width_{};
    std::uint32_t pixel_readback_height_{};
    std::optional<std::array<std::uint32_t, 2>> pixel_readback_request_;
    std::optional<std::array<std::uint32_t, 2>> pixel_readback_position_;
    std::optional<std::array<std::uint8_t, 4>> pixel_readback_result_;
    SDL_GPUBuffer* scene_stream_quad_{};
    std::uint32_t scene_stream_width_{};
    std::uint32_t scene_stream_height_{};
    std::uint32_t scene_stream_capacity_width_{};
    std::uint32_t scene_stream_capacity_height_{};
    std::size_t scene_stream_batches_{};
    std::size_t scene_stream_peak_vertices_{};
    SDL_GPUBuffer* draft_vertex_buffer_{};
    SDL_GPUBuffer* vertex_buffer_{};
    SDL_GPUTransferBuffer* transfer_buffer_{};
    SDL_GPUGraphicsPipeline* text_pipeline_{};
    SDL_GPUGraphicsPipeline* image_pipeline_{};
    SDL_GPUGraphicsPipeline* image_pipeline_direct_{};
    SDL_GPUBuffer* image_vertex_buffer_{};
    SDL_GPUSampler* image_sampler_{};
    SDL_GPUBuffer* text_vertex_buffer_{};
    SDL_GPUBuffer* text_index_buffer_{};
    SDL_GPUTransferBuffer* text_transfer_buffer_{};
    SDL_GPUSampler* text_sampler_{};
    TTF_TextEngine* text_engine_{};
    TTF_Font* font_{};
    TTF_Font* secondary_font_{};
    TTF_Font* bold_font_{};
    TTF_Font* title_font_{};
    TTF_Font* document_font_{};
    TTF_Font* caption_font_{};
    SDL_GPUTexture* msaa_texture_{};
    SDL_GPUTexture* msaa_resolve_texture_{};
    std::uint32_t msaa_width_{};
    std::uint32_t msaa_height_{};
    // Literal sample count (1, 2, 4, or 8), not SDL_GPUSampleCount's
    // zero-based enum value.
    int antialiasing_samples_{1};
    bool antialiasing_enabled_{true};
    bool window_claimed_{};
    bool live_resize_{};
    bool rendering_{};
    std::string diagnostics_;
    std::vector<GeometryVertex> scene_geometry_;
    std::vector<ImageVertex> image_vertices_;
    std::vector<SceneDraw> scene_draws_;
    std::vector<SceneDraw> scene_stream_draws_;
    std::vector<SceneDraw> scene_batch_draws_;
    std::vector<GeometryVertex> scene_batch_geometry_;
    std::vector<Vec2d> stroke_render_points_;
    std::vector<CachedWorldVertex> stroke_page_vertices_;
    std::vector<std::uint32_t> stroke_segment_scratch_;
    std::size_t scene_background_vertices_{};
    std::size_t scene_total_vertices_{};
    bool scene_streamed_{};
    bool resident_scene_invalidated_{};
    std::vector<ObjectId> resident_scene_ids_;
    std::size_t scene_rendered_images_{};
    std::size_t scene_rendered_residents_{};
    std::vector<GeometryVertex> draft_gpu_geometry_;
    std::vector<GeometryVertex> geometry_;
    std::vector<GeometrySpan> geometry_spans_;
    std::optional<std::uint32_t> tooltip_first_vertex_;
    std::size_t tooltip_first_text_batch_{};
    bool tooltip_allowed_this_frame_{};
    std::unordered_map<ObjectId, CachedGeometry, ObjectIdHash> object_cache_;
    ObjectRecency geometry_recency_;
    VisibleObjectCache visibility_cache_;
    MeshArena mesh_arena_;
    std::array<SDL_GPUBuffer*, MeshArena::maximum_pages> mesh_pages_{};
    struct MeshUpload final { MeshRange range; std::uint32_t first{}; };
    std::vector<MeshUpload> mesh_uploads_;
    std::vector<GeometryVertex> mesh_upload_vertices_;
    struct MeshRetirement final { std::vector<MeshRange> ranges; std::uint64_t submission{}; };
    std::vector<MeshRetirement> mesh_retirements_;
    struct MeshSubmission final { SDL_GPUFence* fence{}; std::uint64_t serial{}; };
    std::vector<MeshSubmission> mesh_submissions_;
    std::uint64_t submitted_mesh_serial_{};
    std::uint64_t completed_mesh_serial_{};
    std::optional<ObjectId> mesh_document_identity_;
    std::uint64_t mesh_document_revision_{};
    std::vector<DocumentChange> mesh_changes_;
    std::size_t resident_metadata_bytes_{};
    std::vector<const struct Object*> visible_objects_;
    std::vector<ObjectId> visible_object_ids_;
    std::vector<CachedWorldVertex> draft_shape_geometry_;
    std::vector<CachedWorldVertex> selection_preview_geometry_;
    std::vector<Vec2d> selection_preview_points_;
    std::vector<Vec2d> draft_render_points_;
    std::uint64_t draft_generation_{};
    std::uint64_t draft_revision_{};
    std::uint32_t draft_detail_{};
    std::size_t draft_point_count_{};
    std::size_t draft_render_stride_{1U};
    std::size_t draft_end_cap_offset_{};
    bool draft_cache_active_{};
    bool draft_cache_is_stroke_{};
    std::vector<TextVertex> text_vertices_;
    std::vector<std::uint16_t> text_indices_;
    std::vector<TextBatch> text_batches_;
    std::optional<std::array<double, 4>> text_clip_;
    // Screen-space x for each byte-offset boundary of the filename glyphs
    // visible while editing, in ascending offset order. Rebuilt every frame
    // the filename is being edited and cleared otherwise, so pointer hit
    // testing can resolve a click to a caret position.
    std::vector<std::pair<std::size_t, double>> filename_caret_stops_;
    std::unordered_map<std::string, CachedText> text_cache_;
    std::unordered_map<std::string, double> text_width_cache_;
    std::unordered_map<std::string, std::string> fitted_text_cache_;
    std::unordered_map<const BoardPreview*, ThumbnailTexture>
        thumbnail_textures_;
    std::map<AssetId, CachedImage> image_textures_;
    ImageDecodeCache image_decode_cache_;
    std::size_t image_texture_bytes_{};
    std::size_t image_texture_evictions_{};
    std::vector<std::uint8_t> image_upload_pixels_;
    std::vector<ImageUpload> image_uploads_;
    std::vector<std::uint8_t> thumbnail_upload_pixels_;
    std::vector<ThumbnailUpload> thumbnail_uploads_;
    double text_viewport_width_{1.0};
    double text_viewport_height_{1.0};
    double text_geometry_scale_{1.0};
    double text_raster_size_{};
    RendererStats stats_;
    std::size_t geometry_cache_bytes_{};
    std::size_t total_evictions_{};
    std::uint64_t scene_signature_{};
    std::size_t scene_visible_objects_{};
    std::size_t scene_rendered_objects_{};
    bool scene_valid_{};
    bool scene_upload_pending_{};
    bool scene_active_this_frame_{};
    Vec2d draft_gpu_camera_position_;
    std::size_t draft_upload_first_vertex_{};
    bool draft_upload_pending_{};
    bool draft_active_this_frame_{};
    bool home_cache_trimmed_{};
    std::size_t scene_vertex_capacity_{};
    std::size_t draft_vertex_capacity_{};
    std::size_t overlay_vertex_capacity_{};
    std::size_t image_vertex_capacity_{};
    std::size_t transfer_capacity_bytes_{};
};

} // namespace sawer
