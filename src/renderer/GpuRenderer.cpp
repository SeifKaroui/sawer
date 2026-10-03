#include "renderer/GpuRenderer.hpp"
#include "renderer/BackgroundGrid.hpp"
#include "renderer/StrokeDetail.hpp"
#include "renderer/BackgroundParameters.hpp"
#include "renderer/BoardTheme.hpp"

#include "canvas/Camera.hpp"
#include "canvas/Selection.hpp"
#include "core/BuildInfo.hpp"
#include "core/Log.hpp"
#include "core/ThirdPartyNotices.hpp"
#include "document/Document.hpp"
#include "document/Object.hpp"
#include "geometry/StrokeProcessing.hpp"
#include "ui/HomeView.hpp"
#include "ui/Toolbar.hpp"
#include "ui/UnsavedDialog.hpp"
#include "fonts/EmbeddedFont.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "shaders/generated/colored_triangle.frag.dxil.h"
#include "shaders/generated/colored_triangle.frag.spv.h"
#include "shaders/generated/colored_triangle.vert.dxil.h"
#include "shaders/generated/colored_triangle.vert.spv.h"
#include "shaders/generated/image.frag.dxil.h"
#include "shaders/generated/image.frag.spv.h"
#include "shaders/generated/background.vert.dxil.h"
#include "shaders/generated/background.vert.spv.h"
#include "shaders/generated/background.frag.dxil.h"
#include "shaders/generated/background.frag.spv.h"
#include "shaders/generated/retained.vert.dxil.h"
#include "shaders/generated/retained.vert.spv.h"
#include "shaders/generated/retained.frag.dxil.h"
#include "shaders/generated/retained.frag.spv.h"
#include "shaders/generated/image.vert.dxil.h"
#include "shaders/generated/image.vert.spv.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace sawer {
namespace {

constexpr char text_style_key(const TextStyle style) noexcept
{
    switch (style) {
    case TextStyle::secondary: return 'S';
    case TextStyle::bold: return 'B';
    case TextStyle::title: return 'T';
    case TextStyle::document: return 'D';
    case TextStyle::caption: return 'C';
    default: return 'R';
    }
}

struct RenderColor final {
    float red;
    float green;
    float blue;
};

constexpr RenderColor rgb(const std::uint32_t value) noexcept
{
    return {
        static_cast<float>((value >> 16U) & 0xFFU) / 255.0F,
        static_cast<float>((value >> 8U) & 0xFFU) / 255.0F,
        static_cast<float>(value & 0xFFU) / 255.0F,
    };
}

struct InterfacePalette final {
    RenderColor background;
    RenderColor surface;
    RenderColor control_surface;
    RenderColor hover_surface;
    RenderColor border;
    RenderColor text;
    RenderColor muted;
    RenderColor primary;
    RenderColor primary_hover;
    RenderColor accent_soft;
    RenderColor focus;
    RenderColor preview_surface;
    RenderColor preview_dots;
    RenderColor on_primary;
};

constexpr InterfacePalette interface_palette(const bool light) noexcept
{
    if (light) {
        return {
            .background = rgb(0xF7F8FAU),
            .surface = rgb(0xFFFFFFU),
            .control_surface = rgb(0xE8ECF2U),
            .hover_surface = rgb(0xF0F2F5U),
            .border = rgb(0xD8DDE4U),
            .text = rgb(0x18202AU),
            .muted = rgb(0x657180U),
            .primary = rgb(0x2563EBU),
            .primary_hover = rgb(0x1D4ED8U),
            .accent_soft = rgb(0xEAF1FFU),
            .focus = rgb(0x2563EBU),
            .preview_surface = rgb(0xF8FAFCU),
            .preview_dots = rgb(0xCBD5E1U),
            .on_primary = rgb(0xFFFFFFU),
        };
    }
    return {
        .background = rgb(0x111315U),
        .surface = rgb(0x181B1FU),
        .control_surface = rgb(0x303841U),
        .hover_surface = rgb(0x21262CU),
        .border = rgb(0x303740U),
        .text = rgb(0xF2F4F7U),
        .muted = rgb(0xA0A9B4U),
        .primary = rgb(0x1D4ED8U),
        .primary_hover = rgb(0x2563EBU),
        .accent_soft = rgb(0x102845U),
        .focus = rgb(0x60A5FAU),
        .preview_surface = rgb(0x15181CU),
        .preview_dots = rgb(0x3A424CU),
        .on_primary = rgb(0xFFFFFFU),
    };
}

constexpr std::array<float, 4> text_color(
    const RenderColor color) noexcept
{
    return {color.red, color.green, color.blue, 1.0F};
}

static_assert(sizeof(GeometryVertex) == 12U);
static_assert(sizeof(CachedWorldVertex) == 24U);
constexpr Uint32 maximum_vertex_count = 1'048'576U;
constexpr std::size_t scene_batch_vertex_count =
    maximum_vertex_count - maximum_vertex_count % 3U;
constexpr std::size_t stroke_page_vertex_count = 65'535U;
constexpr std::size_t maximum_cached_stroke_vertices = scene_batch_vertex_count;
constexpr std::size_t initial_scene_vertex_capacity = 524'288U;
constexpr std::size_t initial_draft_vertex_capacity = 524'288U;
constexpr std::size_t initial_overlay_vertex_capacity = 65'536U;
// Optional board/thumbnail geometry must leave enough of the shared overlay
// buffer for selection affordances and file/tool controls.
constexpr std::size_t overlay_ui_vertex_reserve = 65'536U;
constexpr std::size_t maximum_overlay_content_vertices =
    maximum_vertex_count - overlay_ui_vertex_reserve;

class CommandBufferGuard final {
public:
    explicit CommandBufferGuard(
        SDL_GPUCommandBuffer* command_buffer) noexcept
        : command_buffer_{command_buffer}
    {
    }

    CommandBufferGuard(const CommandBufferGuard&) = delete;
    CommandBufferGuard& operator=(const CommandBufferGuard&) = delete;

    ~CommandBufferGuard() noexcept
    {
        if (command_buffer_ == nullptr) {
            return;
        }
        if (swapchain_acquired_) {
            // SDL forbids cancelling after a swapchain image has been
            // acquired. Submitting an otherwise empty/partial buffer safely
            // releases that image while an exception is unwinding.
            static_cast<void>(
                SDL_SubmitGPUCommandBuffer(command_buffer_));
        } else {
            static_cast<void>(
                SDL_CancelGPUCommandBuffer(command_buffer_));
        }
    }

    void mark_swapchain_acquired() noexcept
    {
        swapchain_acquired_ = true;
    }

    // Only used after release(), when a submission consumed the old buffer.
    void reset(SDL_GPUCommandBuffer* command_buffer) noexcept
    {
        command_buffer_ = command_buffer;
        swapchain_acquired_ = false;
    }

    [[nodiscard]] SDL_GPUCommandBuffer* release() noexcept
    {
        SDL_GPUCommandBuffer* const result = command_buffer_;
        command_buffer_ = nullptr;
        return result;
    }

private:
    SDL_GPUCommandBuffer* command_buffer_ = nullptr;
    bool swapchain_acquired_ = false;
};

[[noreturn]] void throw_sdl(std::string_view operation);

void submit_and_wait(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer)
{
    SDL_GPUFence* const fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
    if (fence == nullptr) throw_sdl("GPU batch submission");
    const bool complete = SDL_WaitForGPUFences(device, true, &fence, 1U);
    SDL_ReleaseGPUFence(device, fence);
    if (!complete) throw_sdl("GPU batch completion");
}

constexpr std::size_t initial_transfer_capacity_bytes =
    8U * 1024U * 1024U;
constexpr std::size_t maximum_transfer_capacity_bytes =
    384U * 1024U * 1024U;
constexpr Uint32 maximum_text_vertices = 65'536U;
constexpr Uint32 maximum_text_indices = 98'304U;
constexpr std::size_t maximum_cached_texts = 512U;
constexpr double text_raster_size_quantum = 0.25;
constexpr std::size_t geometry_cache_budget = 256U * 1024U * 1024U;
constexpr std::size_t maximum_cache_entries = 50'000U;
constexpr std::size_t image_texture_budget = 256U * 1024U * 1024U;
constexpr std::uint32_t image_tile_extent = 2'048U;
constexpr double pi = 3.14159265358979323846;
constexpr RenderColor border_color{0.39F, 0.43F, 0.49F};

bool is_settings_control(
    const Toolbar& toolbar, const UiControl* const control) noexcept
{
    return control != nullptr
        && toolbar.is_settings_control(control->action);
}

constexpr bool is_tool_action(const UiAction action) noexcept
{
    return action >= UiAction::select && action <= UiAction::ellipse;
}

constexpr bool uses_soft_property_selection(const UiAction action) noexcept
{
    return action == UiAction::edit_stroke_custom
        || action == UiAction::properties_menu
        || action == UiAction::color_target_stroke
        || action == UiAction::color_target_fill
        || action == UiAction::fill_none
        || (action >= UiAction::width_thin
            && action <= UiAction::width_increase)
        || (action >= UiAction::roundness_square
            && action <= UiAction::roundness_full);
}

std::optional<UiRect> tooltip_bounds(
    const Toolbar& toolbar, const UiControl* const control,
    const double text_width,
    const double text_height = 0.0) noexcept
{
    if (control == nullptr || control->tooltip.empty()) {
        return std::nullopt;
    }
    const double scale = toolbar.scale();
    const double margin = 6.0 * scale;
    const double spacing = 12.0 * scale;
    const double width = std::min(
        toolbar.viewport_width() - margin * 2.0,
        text_width + 24.0 * scale);
    const double height = std::max(30.0 * scale, text_height + 12.0 * scale);

    if (toolbar.is_property_control(control->action)) {
        // Keep help beside the entire options panel so adjacent controls stay
        // visible. Compact windows fall back to the roomier vertical side.
        const UiRect panel = toolbar.properties_bounds();
        const double x = panel.x + panel.width + spacing;
        const double y = std::clamp(
            control->bounds.y + (control->bounds.height - height) * 0.5,
            margin,
            std::max(margin, toolbar.viewport_height() - height - margin));
        if (x + width <= toolbar.viewport_width() - margin) {
            return UiRect{x, y, width, height};
        }
        const double fallback_x = std::clamp(control->bounds.x,
            margin, std::max(margin, toolbar.viewport_width() - width - margin));
        const double below = control->bounds.y + control->bounds.height + spacing;
        const double fallback_y = below + height <= toolbar.viewport_height() - margin
            ? below : std::max(margin, control->bounds.y - height - spacing);
        return UiRect{fallback_x, fallback_y, width, height};
    }

    if (is_settings_control(toolbar, control)) {
        const double y = std::clamp(
            control->bounds.y + control->bounds.height * 0.5 - height * 0.5,
            margin,
            std::max(margin, toolbar.viewport_height() - height - margin));
        const double right =
            control->bounds.x + control->bounds.width + spacing;
        if (right + width <= toolbar.viewport_width() - margin) {
            return UiRect{right, y, width, height};
        }
        const double left = control->bounds.x - width - spacing;
        if (left >= margin) {
            return UiRect{left, y, width, height};
        }

        // Narrow windows may not have enough horizontal space. Keep the
        // callout attached to the item and choose the roomier vertical side.
        const double x = std::clamp(
            control->bounds.x + control->bounds.width * 0.5 - width * 0.5,
            margin,
            std::max(margin, toolbar.viewport_width() - width - margin));
        const double below =
            control->bounds.y + control->bounds.height + spacing;
        if (below + height <= toolbar.viewport_height() - margin) {
            return UiRect{x, below, width, height};
        }
        return UiRect{
            x, std::max(margin, control->bounds.y - height - spacing),
            width, height};
    }

    const double x = std::clamp(
        control->bounds.x + control->bounds.width * 0.5 - width * 0.5,
        margin,
        std::max(margin, toolbar.viewport_width() - width - margin));
    const bool bottom =
        control->bounds.y > toolbar.viewport_height() * 0.5;
    const double y = bottom
        ? control->bounds.y - height - 10.0 * scale
        : control->bounds.y + control->bounds.height + 10.0 * scale;
    return UiRect{x, y, width, height};
}

[[noreturn]] void throw_sdl(const std::string_view operation)
{
    throw std::runtime_error{
        std::string{operation} + " failed: " + SDL_GetError()};
}

constexpr const char* requested_driver() noexcept
{
#if defined(_WIN32)
    return "direct3d12";
#elif defined(__linux__)
    return "vulkan";
#else
    return nullptr;
#endif
}

constexpr SDL_GPUSampleCount gpu_sample_count(
    const int samples) noexcept
{
    switch (samples) {
    case 2: return SDL_GPU_SAMPLECOUNT_2;
    case 4: return SDL_GPU_SAMPLECOUNT_4;
    case 8: return SDL_GPU_SAMPLECOUNT_8;
    default: return SDL_GPU_SAMPLECOUNT_1;
    }
}

SDL_GPUShader* create_shader(
    SDL_GPUDevice* const device,
    const bool vertex_shader)
{
    const auto supported_formats = SDL_GetGPUShaderFormats(device);

    SDL_GPUShaderCreateInfo info{};
    info.entrypoint = "main";
    info.stage = vertex_shader
        ? SDL_GPU_SHADERSTAGE_VERTEX
        : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = vertex_shader ? 1U : 0U;

    if ((supported_formats & SDL_GPU_SHADERFORMAT_DXIL) != 0U) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = vertex_shader
            ? colored_triangle_vert_dxil
            : colored_triangle_frag_dxil;
        info.code_size = vertex_shader
            ? colored_triangle_vert_dxil_len
            : colored_triangle_frag_dxil_len;
    } else if ((supported_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0U) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = vertex_shader
            ? colored_triangle_vert_spv
            : colored_triangle_frag_spv;
        info.code_size = vertex_shader
            ? colored_triangle_vert_spv_len
            : colored_triangle_frag_spv_len;
    } else {
        throw std::runtime_error{"GPU driver exposes no supported shader format"};
    }

    auto* const shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) {
        throw_sdl(vertex_shader ? "Vertex shader creation" : "Fragment shader creation");
    }
    return shader;
}

SDL_GPUShader* create_text_shader(
    SDL_GPUDevice* const device,
    const bool vertex_shader)
{
    SDL_GPUShaderCreateInfo info{};
    info.stage = vertex_shader
        ? SDL_GPU_SHADERSTAGE_VERTEX
        : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = vertex_shader ? 1U : 0U;
    info.num_samplers = vertex_shader ? 0U : 1U;
    const auto supported_formats = SDL_GetGPUShaderFormats(device);
    if ((supported_formats & SDL_GPU_SHADERFORMAT_DXIL) != 0U) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = vertex_shader ? shader_vert_dxil : shader_frag_dxil;
        info.code_size = vertex_shader
            ? shader_vert_dxil_len
            : shader_frag_dxil_len;
        info.entrypoint = vertex_shader ? "VSMain" : "PSMain";
    } else if ((supported_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0U) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = vertex_shader ? shader_vert_spv : shader_frag_spv;
        info.code_size = vertex_shader
            ? shader_vert_spv_len
            : shader_frag_spv_len;
        info.entrypoint = "main";
    } else {
        throw std::runtime_error{"GPU driver exposes no text shader format"};
    }
    SDL_GPUShader* const shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) {
        throw_sdl(vertex_shader
            ? "Text vertex shader creation"
            : "Text fragment shader creation");
    }
    return shader;
}

SDL_GPUShader* create_embedded_shader(SDL_GPUDevice* device, const bool vertex,
    const std::span<const unsigned char> dxil, const std::span<const unsigned char> spirv,
    const Uint32 uniforms)
{
    SDL_GPUShaderCreateInfo info{};
    info.entrypoint = "main";
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = uniforms;
    const bool use_dxil = (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_DXIL) != 0U;
    const auto bytes = use_dxil ? dxil : spirv;
    info.format = use_dxil ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.code = bytes.data();
    info.code_size = bytes.size();
    auto* shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) throw_sdl("Embedded shader creation");
    return shader;
}

SDL_GPUGraphicsPipeline* create_auxiliary_pipeline(SDL_GPUDevice* device,
    SDL_Window* window, SDL_GPUShader* vertex, SDL_GPUShader* fragment,
    const int samples, const bool mesh)
{
    const SDL_GPUVertexBufferDescription buffer{0U, sizeof(GeometryVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0U};
    const std::array<SDL_GPUVertexAttribute, 2> attributes{{
        {0U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0U},
        {1U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 8U}}};
    SDL_GPUColorTargetDescription target{};
    target.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    if (mesh) {
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1U;
        info.vertex_input_state.vertex_attributes = attributes.data();
        info.vertex_input_state.num_vertex_attributes = 2U;
    }
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    info.multisample_state.sample_count = gpu_sample_count(samples);
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1U;
    auto* pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (pipeline == nullptr) throw_sdl("Auxiliary pipeline creation");
    return pipeline;
}

SDL_GPUShader* create_image_shader(
    SDL_GPUDevice* const device,
    const bool vertex_shader)
{
    SDL_GPUShaderCreateInfo info{};
    info.stage = vertex_shader
        ? SDL_GPU_SHADERSTAGE_VERTEX
        : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = vertex_shader ? 1U : 0U;
    info.num_samplers = vertex_shader ? 0U : 1U;
    const auto formats = SDL_GetGPUShaderFormats(device);
    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0U) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = vertex_shader ? image_vert_dxil : image_frag_dxil;
        info.code_size = vertex_shader
            ? image_vert_dxil_len : image_frag_dxil_len;
        info.entrypoint = vertex_shader ? "VSMain" : "PSMain";
    } else if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0U) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = vertex_shader ? image_vert_spv : image_frag_spv;
        info.code_size = vertex_shader
            ? image_vert_spv_len : image_frag_spv_len;
        info.entrypoint = "main";
    } else {
        throw std::runtime_error{"GPU driver exposes no image shader format"};
    }
    SDL_GPUShader* const shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) {
        throw_sdl(vertex_shader
            ? "Image vertex shader creation"
            : "Image fragment shader creation");
    }
    return shader;
}

std::string property_or_unknown(
    const SDL_PropertiesID properties,
    const char* const name)
{
    const char* const value = SDL_GetStringProperty(properties, name, nullptr);
    return value != nullptr ? value : "unknown";
}

std::uint8_t packed_channel(const float value) noexcept
{
    return static_cast<std::uint8_t>(
        std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

std::array<std::uint8_t, 4> packed_color(
    const RenderColor color) noexcept
{
    return {
        packed_channel(color.red),
        packed_channel(color.green),
        packed_channel(color.blue),
        255U,
    };
}

std::array<std::uint8_t, 4> packed_color(
    const std::array<float, 4> color) noexcept
{
    return {
        packed_channel(color[0]),
        packed_channel(color[1]),
        packed_channel(color[2]),
        packed_channel(color[3]),
    };
}

void append_vertex(
    std::vector<GeometryVertex>& output,
    const double x,
    const double y,
    const Vec2d camera_position,
    const RenderColor color)
{
    output.push_back({
        {
            static_cast<float>(x - camera_position.x),
            static_cast<float>(y - camera_position.y),
        },
        packed_color(color),
    });
}

void append_quad(
    std::vector<GeometryVertex>& output,
    const double min_x,
    const double min_y,
    const double max_x,
    const double max_y,
    const Vec2d camera_position,
    const RenderColor color)
{
    append_vertex(output, min_x, min_y, camera_position, color);
    append_vertex(output, max_x, min_y, camera_position, color);
    append_vertex(output, max_x, max_y, camera_position, color);
    append_vertex(output, min_x, min_y, camera_position, color);
    append_vertex(output, max_x, max_y, camera_position, color);
    append_vertex(output, min_x, max_y, camera_position, color);
}

void append_screen_quad(
    std::vector<GeometryVertex>& output,
    const UiRect rect,
    const Camera& camera,
    const RenderColor color)
{
    const Vec2d first = camera.screen_to_world({rect.x, rect.y});
    const Vec2d second = camera.screen_to_world(
        {rect.x + rect.width, rect.y + rect.height});
    append_quad(
        output,
        first.x,
        first.y,
        second.x,
        second.y,
        camera.position(),
        color);
}

RenderColor mix_color(
    const RenderColor first,
    const RenderColor second,
    const double amount)
{
    const float value = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    return {
        first.red + (second.red - first.red) * value,
        first.green + (second.green - first.green) * value,
        first.blue + (second.blue - first.blue) * value,
    };
}

InterfacePalette mix_palette(
    const InterfacePalette& first,
    const InterfacePalette& second,
    const double amount)
{
    return {
        .background = mix_color(first.background, second.background, amount),
        .surface = mix_color(first.surface, second.surface, amount),
        .control_surface =
            mix_color(
                first.control_surface, second.control_surface, amount),
        .hover_surface =
            mix_color(first.hover_surface, second.hover_surface, amount),
        .border = mix_color(first.border, second.border, amount),
        .text = mix_color(first.text, second.text, amount),
        .muted = mix_color(first.muted, second.muted, amount),
        .primary = mix_color(first.primary, second.primary, amount),
        .primary_hover =
            mix_color(first.primary_hover, second.primary_hover, amount),
        .accent_soft =
            mix_color(first.accent_soft, second.accent_soft, amount),
        .focus = mix_color(first.focus, second.focus, amount),
        .preview_surface =
            mix_color(first.preview_surface, second.preview_surface, amount),
        .preview_dots =
            mix_color(first.preview_dots, second.preview_dots, amount),
        .on_primary =
            mix_color(first.on_primary, second.on_primary, amount),
    };
}

double smooth_theme_transition(const HomeView& home) noexcept
{
    const double amount = std::clamp(home.theme_transition(), 0.0, 1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

InterfacePalette home_palette(const HomeView& home)
{
    const auto gallery_palette = [](const bool light) {
        InterfacePalette palette = interface_palette(light);
        palette.background = light ? rgb(0xEEF0F3U) : rgb(0x0F1114U);
        palette.surface = light ? rgb(0xFFFFFFU) : rgb(0x202429U);
        palette.preview_surface = light ? rgb(0xFAFBFCU) : rgb(0x1B1F24U);
        return palette;
    };
    return mix_palette(
        gallery_palette(home.previous_theme() == Theme::light),
        gallery_palette(home.theme() == Theme::light),
        smooth_theme_transition(home));
}

double smooth_theme_transition(const Toolbar& toolbar) noexcept
{
    const double amount = std::clamp(
        toolbar.theme_transition(), 0.0, 1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

InterfacePalette toolbar_palette(const Toolbar& toolbar)
{
    return mix_palette(
        interface_palette(toolbar.previous_theme() == Theme::light),
        interface_palette(toolbar.theme() == Theme::light),
        smooth_theme_transition(toolbar));
}

RenderColor hsv_render_color(
    const double hue, const double saturation, const double value)
{
    const double wrapped = hue - std::floor(hue);
    const double scaled = wrapped * 6.0;
    const int sector = static_cast<int>(std::floor(scaled)) % 6;
    const double fraction = scaled - std::floor(scaled);
    const double p = value * (1.0 - saturation);
    const double q = value * (1.0 - fraction * saturation);
    const double t = value * (1.0 - (1.0 - fraction) * saturation);
    double red = value;
    double green = t;
    double blue = p;
    switch (sector) {
    case 1: red = q; green = value; blue = p; break;
    case 2: red = p; green = value; blue = t; break;
    case 3: red = p; green = q; blue = value; break;
    case 4: red = t; green = p; blue = value; break;
    case 5: red = value; green = p; blue = q; break;
    default: break;
    }
    return {
        static_cast<float>(red),
        static_cast<float>(green),
        static_cast<float>(blue),
    };
}

void append_screen_triangle(
    std::vector<GeometryVertex>& output,
    const Vec2d first,
    const Vec2d second,
    const Vec2d third,
    const Camera& camera,
    const RenderColor color)
{
    const Vec2d camera_position = camera.position();
    const Vec2d world_first = camera.screen_to_world(first);
    const Vec2d world_second = camera.screen_to_world(second);
    const Vec2d world_third = camera.screen_to_world(third);
    append_vertex(output, world_first.x, world_first.y, camera_position, color);
    append_vertex(output, world_second.x, world_second.y, camera_position, color);
    append_vertex(output, world_third.x, world_third.y, camera_position, color);
}

void append_screen_circle(
    std::vector<GeometryVertex>& output,
    const Vec2d center,
    const double radius,
    const Camera& camera,
    const RenderColor color,
    const std::uint32_t segments = 20U)
{
    for (std::uint32_t index = 0U; index < segments; ++index) {
        const double first_angle = 2.0 * pi * static_cast<double>(index)
            / static_cast<double>(segments);
        const double second_angle = 2.0 * pi * static_cast<double>(index + 1U)
            / static_cast<double>(segments);
        append_screen_triangle(
            output,
            center,
            {center.x + std::cos(first_angle) * radius,
             center.y + std::sin(first_angle) * radius},
            {center.x + std::cos(second_angle) * radius,
             center.y + std::sin(second_angle) * radius},
            camera,
            color);
    }
}

void append_screen_line(
    std::vector<GeometryVertex>& output,
    const Vec2d first,
    const Vec2d second,
    const double width,
    const Camera& camera,
    const RenderColor color)
{
    const double dx = second.x - first.x;
    const double dy = second.y - first.y;
    const double length = std::hypot(dx, dy);
    if (length < 1.0e-6) return;
    const Vec2d normal{-dy / length * width * 0.5, dx / length * width * 0.5};
    append_screen_triangle(
        output,
        {first.x + normal.x, first.y + normal.y},
        {second.x + normal.x, second.y + normal.y},
        {second.x - normal.x, second.y - normal.y},
        camera,
        color);
    append_screen_triangle(
        output,
        {first.x + normal.x, first.y + normal.y},
        {second.x - normal.x, second.y - normal.y},
        {first.x - normal.x, first.y - normal.y},
        camera,
        color);
    append_screen_circle(output, first, width * 0.5, camera, color, 10U);
    append_screen_circle(output, second, width * 0.5, camera, color, 10U);
}

void append_screen_rounded_rect(
    std::vector<GeometryVertex>& output,
    const UiRect rect,
    const double requested_radius,
    const Camera& camera,
    const RenderColor color)
{
    // Responsive layouts can briefly produce degenerate inner rectangles
    // while the native window is between resize ticks. They have no visible
    // area and must not be passed to clamp with a negative radius ceiling.
    if (rect.width <= 0.0 || rect.height <= 0.0) {
        return;
    }
    const double radius = std::clamp(
        requested_radius, 0.0, std::min(rect.width, rect.height) * 0.5);
    append_screen_quad(
        output,
        {rect.x + radius, rect.y, rect.width - radius * 2.0, rect.height},
        camera,
        color);
    append_screen_quad(
        output,
        {rect.x, rect.y + radius, radius, rect.height - radius * 2.0},
        camera,
        color);
    append_screen_quad(
        output,
        {rect.x + rect.width - radius,
         rect.y + radius,
         radius,
         rect.height - radius * 2.0},
        camera,
        color);
    constexpr std::uint32_t corner_segments = 14U;
    const std::array<Vec2d, 4> centers{{
        {rect.x + radius, rect.y + radius},
        {rect.x + rect.width - radius, rect.y + radius},
        {rect.x + rect.width - radius, rect.y + rect.height - radius},
        {rect.x + radius, rect.y + rect.height - radius},
    }};
    for (std::size_t corner = 0U; corner < centers.size(); ++corner) {
        const double start = pi + static_cast<double>(corner) * pi * 0.5;
        for (std::uint32_t segment = 0U; segment < corner_segments; ++segment) {
            const double first_angle = start
                + static_cast<double>(segment) * pi * 0.5
                    / static_cast<double>(corner_segments);
            const double second_angle = start
                + static_cast<double>(segment + 1U) * pi * 0.5
                    / static_cast<double>(corner_segments);
            append_screen_triangle(
                output,
                centers[corner],
                {centers[corner].x + std::cos(first_angle) * radius,
                 centers[corner].y + std::sin(first_angle) * radius},
                {centers[corner].x + std::cos(second_angle) * radius,
                 centers[corner].y + std::sin(second_angle) * radius},
                camera,
                color);
        }
    }
}

// Closes the current alpha span and starts a new one at the given opacity.
// Contiguous requests at the same opacity collapse into one span.
void set_geometry_alpha(
    std::vector<GeometrySpan>& spans,
    const std::vector<GeometryVertex>& output,
    const double alpha)
{
    const auto vertex =
        static_cast<std::uint32_t>(output.size());
    const auto value = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    if (!spans.empty() && spans.back().first_vertex == vertex) {
        spans.back().alpha = value;
        return;
    }
    if (!spans.empty() && spans.back().alpha == value) {
        return;
    }
    spans.push_back(GeometrySpan{vertex, value,
        spans.empty() ? std::nullopt : spans.back().clip});
}

void set_geometry_clip(
    std::vector<GeometrySpan>& spans,
    const std::vector<GeometryVertex>& output,
    const std::optional<std::array<double, 4>>& clip)
{
    const auto vertex = static_cast<std::uint32_t>(output.size());
    if (!spans.empty() && spans.back().first_vertex == vertex) {
        spans.back().clip = clip;
    } else if (spans.empty() || spans.back().clip != clip) {
        spans.push_back(GeometrySpan{vertex,
            spans.empty() ? 1.0F : spans.back().alpha, clip});
    }
}

constexpr bool can_append_vertices(
    const std::size_t current,
    const std::size_t additional,
    const std::size_t limit) noexcept
{
    return current <= limit && additional <= limit - current;
}

void trim_geometry_to_vertex_budget(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans)
{
    constexpr std::size_t triangle_aligned_vertex_limit =
        maximum_vertex_count - maximum_vertex_count % 3U;
    if (output.size() <= triangle_aligned_vertex_limit) {
        return;
    }

    output.resize(triangle_aligned_vertex_limit);
    const auto first_out_of_range = std::lower_bound(
        spans.begin(),
        spans.end(),
        static_cast<std::uint32_t>(output.size()),
        [](const GeometrySpan& span, const std::uint32_t vertex) {
            return span.first_vertex < vertex;
        });
    spans.erase(first_out_of_range, spans.end());
    if (spans.empty()) {
        spans.push_back(GeometrySpan{0U, 1.0F});
    }
}

void append_screen_triangle_gradient(
    std::vector<GeometryVertex>& output,
    const Vec2d first,
    const Vec2d second,
    const Vec2d third,
    const Camera& camera,
    const RenderColor first_color,
    const RenderColor second_color,
    const RenderColor third_color)
{
    const Vec2d camera_position = camera.position();
    const Vec2d world_first = camera.screen_to_world(first);
    const Vec2d world_second = camera.screen_to_world(second);
    const Vec2d world_third = camera.screen_to_world(third);
    append_vertex(
        output, world_first.x, world_first.y, camera_position, first_color);
    append_vertex(
        output, world_second.x, world_second.y, camera_position, second_color);
    append_vertex(
        output, world_third.x, world_third.y, camera_position, third_color);
}

void append_screen_quad_gradient(
    std::vector<GeometryVertex>& output,
    const UiRect rect,
    const Camera& camera,
    const RenderColor top,
    const RenderColor bottom)
{
    if (rect.width <= 0.0 || rect.height <= 0.0) {
        return;
    }
    append_screen_triangle_gradient(
        output,
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        camera, top, top, bottom);
    append_screen_triangle_gradient(
        output,
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x, rect.y + rect.height},
        camera, top, bottom, bottom);
}

void append_screen_quad_gradient_horizontal(
    std::vector<GeometryVertex>& output,
    const UiRect rect,
    const Camera& camera,
    const RenderColor left,
    const RenderColor right)
{
    if (rect.width <= 0.0 || rect.height <= 0.0) {
        return;
    }
    append_screen_triangle_gradient(
        output,
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        camera, left, right, right);
    append_screen_triangle_gradient(
        output,
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x, rect.y + rect.height},
        camera, left, right, left);
}

void append_screen_gradient_cell(
    std::vector<GeometryVertex>& output,
    const UiRect rect,
    const Camera& camera,
    const RenderColor top_left,
    const RenderColor top_right,
    const RenderColor bottom_right,
    const RenderColor bottom_left)
{
    if (rect.width <= 0.0 || rect.height <= 0.0) {
        return;
    }
    append_screen_triangle_gradient(
        output,
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        camera, top_left, top_right, bottom_right);
    append_screen_triangle_gradient(
        output,
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x, rect.y + rect.height},
        camera, top_left, bottom_right, bottom_left);
}

// Vertical-gradient rounded rectangle. Because the gradient is a linear
// function of y, per-vertex colors interpolate to an exact smooth ramp no
// matter how the shape is triangulated.
void append_screen_rounded_rect_gradient(
    std::vector<GeometryVertex>& output,
    const UiRect rect,
    const double requested_radius,
    const Camera& camera,
    const RenderColor top,
    const RenderColor bottom)
{
    if (rect.width <= 0.0 || rect.height <= 0.0) {
        return;
    }
    const auto at = [&](const double y) {
        return mix_color(
            top, bottom, std::clamp((y - rect.y) / rect.height, 0.0, 1.0));
    };
    const double radius = std::clamp(
        requested_radius, 0.0, std::min(rect.width, rect.height) * 0.5);
    append_screen_quad_gradient(
        output,
        {rect.x + radius, rect.y, rect.width - radius * 2.0, rect.height},
        camera, top, bottom);
    append_screen_quad_gradient(
        output,
        {rect.x, rect.y + radius, radius, rect.height - radius * 2.0},
        camera, at(rect.y + radius), at(rect.y + rect.height - radius));
    append_screen_quad_gradient(
        output,
        {rect.x + rect.width - radius,
         rect.y + radius,
         radius,
         rect.height - radius * 2.0},
        camera, at(rect.y + radius), at(rect.y + rect.height - radius));
    constexpr std::uint32_t corner_segments = 14U;
    const std::array<Vec2d, 4> centers{{
        {rect.x + radius, rect.y + radius},
        {rect.x + rect.width - radius, rect.y + radius},
        {rect.x + rect.width - radius, rect.y + rect.height - radius},
        {rect.x + radius, rect.y + rect.height - radius},
    }};
    for (std::size_t corner = 0U; corner < centers.size(); ++corner) {
        const double start = pi + static_cast<double>(corner) * pi * 0.5;
        for (std::uint32_t segment = 0U; segment < corner_segments; ++segment) {
            const double first_angle = start
                + static_cast<double>(segment) * pi * 0.5
                    / static_cast<double>(corner_segments);
            const double second_angle = start
                + static_cast<double>(segment + 1U) * pi * 0.5
                    / static_cast<double>(corner_segments);
            const Vec2d first{
                centers[corner].x + std::cos(first_angle) * radius,
                centers[corner].y + std::sin(first_angle) * radius};
            const Vec2d second{
                centers[corner].x + std::cos(second_angle) * radius,
                centers[corner].y + std::sin(second_angle) * radius};
            append_screen_triangle_gradient(
                output, centers[corner], first, second, camera,
                at(centers[corner].y), at(first.y), at(second.y));
        }
    }
}

// Layered translucent rounded rectangles approximating a blurred drop
// shadow. `strength` scales opacity; `lift` scales vertical offset and
// spread so hovered elements appear to rise. Restores full opacity for the
// geometry that follows.
void append_soft_shadow(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans,
    const UiRect rect,
    const double radius,
    const Camera& camera,
    const RenderColor color,
    const double scale,
    const double strength,
    const double lift = 1.0)
{
    // Tight, directional drop shadow: every layer is offset downward at
    // least as far as it spreads, so nothing halos above or around the
    // element — only a soft fringe beneath it, like a CSS "0 4px 12px"
    // elevation.
    constexpr std::array<double, 4> spreads{{0.0, 1.2, 2.8, 5.0}};
    constexpr std::array<double, 4> offsets{{1.2, 2.2, 3.6, 5.6}};
    constexpr std::array<double, 4> alphas{{0.085, 0.060, 0.042, 0.028}};
    for (std::size_t layer = 0U; layer < spreads.size(); ++layer) {
        const double spread = spreads[layer] * scale;
        const double offset =
            std::max(offsets[layer] * lift, spreads[layer]) * scale;
        set_geometry_alpha(spans, output, alphas[layer] * strength);
        append_screen_rounded_rect(
            output,
            {rect.x - spread,
             rect.y - spread + offset,
             rect.width + spread * 2.0,
             rect.height + spread * 2.0},
            radius + spread, camera, color);
    }
    set_geometry_alpha(spans, output, 1.0);
}

void append_icon(
    std::vector<GeometryVertex>& output,
    const UiControl& control,
    const UiRect bounds,
    const Camera& camera,
    const RenderColor color,
    const double scale)
{
    const Vec2d center{
        bounds.x + bounds.width * 0.5,
        bounds.y + bounds.height * 0.5,
    };
    const double unit = scale;
    // Sawer's standard icons follow a 24-unit optical grid with the same
    // rounded, centered outline treatment throughout. The slightly lighter
    // than 2-unit stroke stays crisp at 1x while avoiding visual heaviness in
    // dense toolbar groups.
    const double stroke = 1.75 * unit;
    const auto line = [&](const double x1, const double y1,
                          const double x2, const double y2,
                          const double width = 0.0) {
        append_screen_line(
            output,
            {center.x + x1 * unit, center.y + y1 * unit},
            {center.x + x2 * unit, center.y + y2 * unit},
            width > 0.0 ? width : stroke,
            camera,
            color);
    };
    const auto rect = [&](const double x, const double y,
                          const double width, const double height) {
        line(x, y, x + width, y);
        line(x + width, y, x + width, y + height);
        line(x + width, y + height, x, y + height);
        line(x, y + height, x, y);
    };
    const auto arc = [&](const double cx, const double cy,
                         const double radius_x, const double radius_y,
                         const double start, const double end,
                         const std::uint32_t segments,
                         const double width = 0.0) {
        for (std::uint32_t index = 0U; index < segments; ++index) {
            const double amount = static_cast<double>(index)
                / static_cast<double>(segments);
            const double next_amount = static_cast<double>(index + 1U)
                / static_cast<double>(segments);
            const double first = start + (end - start) * amount;
            const double second = start + (end - start) * next_amount;
            line(
                cx + std::cos(first) * radius_x,
                cy + std::sin(first) * radius_y,
                cx + std::cos(second) * radius_x,
                cy + std::sin(second) * radius_y,
                width);
        }
    };
    const auto ring_at = [&](const double x, const double y,
                             const double radius_x, const double radius_y,
                             const double width = 0.0) {
        arc(x, y, radius_x, radius_y, 0.0, 2.0 * pi, 28U, width);
    };
    const auto ring = [&](const double radius_x, const double radius_y,
                          const double width = 0.0) {
        ring_at(0.0, 0.0, radius_x, radius_y, width);
    };
    const auto rounded_box = [&](const double x, const double y,
                                 const double width, const double height,
                                 const double radius) {
        line(x + radius, y, x + width - radius, y);
        line(x + width, y + radius, x + width, y + height - radius);
        line(x + width - radius, y + height, x + radius, y + height);
        line(x, y + height - radius, x, y + radius);
        arc(x + radius, y + radius, radius, radius, pi, 1.5 * pi, 5U);
        arc(x + width - radius, y + radius, radius, radius,
            1.5 * pi, 2.0 * pi, 5U);
        arc(x + width - radius, y + height - radius, radius, radius,
            0.0, 0.5 * pi, 5U);
        arc(x + radius, y + height - radius, radius, radius,
            0.5 * pi, pi, 5U);
    };
    const auto svg_line = [&](const double x1, const double y1,
                              const double x2, const double y2,
                              const double width = 0.0) {
        line(
            x1 - 12.0, y1 - 12.0,
            x2 - 12.0, y2 - 12.0,
            width);
    };
    const auto svg_arc = [&](const double x1, const double y1,
                             const double x2, const double y2,
                             double radius_x, double radius_y,
                             const bool large_arc, const bool sweep) {
        const double midpoint_x = (x1 - x2) * 0.5;
        const double midpoint_y = (y1 - y2) * 0.5;
        const double radius_scale =
            midpoint_x * midpoint_x / (radius_x * radius_x)
            + midpoint_y * midpoint_y / (radius_y * radius_y);
        if (radius_scale > 1.0) {
            const double expansion = std::sqrt(radius_scale);
            radius_x *= expansion;
            radius_y *= expansion;
        }

        const double numerator = std::max(
            0.0,
            radius_x * radius_x * radius_y * radius_y
                - radius_x * radius_x * midpoint_y * midpoint_y
                - radius_y * radius_y * midpoint_x * midpoint_x);
        const double denominator =
            radius_x * radius_x * midpoint_y * midpoint_y
            + radius_y * radius_y * midpoint_x * midpoint_x;
        const double direction = large_arc == sweep ? -1.0 : 1.0;
        const double coefficient = denominator > 1.0e-12
            ? direction * std::sqrt(numerator / denominator)
            : 0.0;
        const double local_center_x =
            coefficient * radius_x * midpoint_y / radius_y;
        const double local_center_y =
            coefficient * -radius_y * midpoint_x / radius_x;
        const double arc_center_x =
            (x1 + x2) * 0.5 + local_center_x;
        const double arc_center_y =
            (y1 + y2) * 0.5 + local_center_y;

        const double start = std::atan2(
            (midpoint_y - local_center_y) / radius_y,
            (midpoint_x - local_center_x) / radius_x);
        const double end = std::atan2(
            (-midpoint_y - local_center_y) / radius_y,
            (-midpoint_x - local_center_x) / radius_x);
        double extent = end - start;
        if (!sweep && extent > 0.0) {
            extent -= 2.0 * pi;
        } else if (sweep && extent < 0.0) {
            extent += 2.0 * pi;
        }
        const std::uint32_t segments = std::max(
            3U,
            static_cast<std::uint32_t>(
                std::ceil(std::abs(extent) / (pi / 8.0))));
        for (std::uint32_t index = 0U; index < segments; ++index) {
            const double first = start + extent
                * static_cast<double>(index)
                / static_cast<double>(segments);
            const double second = start + extent
                * static_cast<double>(index + 1U)
                / static_cast<double>(segments);
            svg_line(
                arc_center_x + std::cos(first) * radius_x,
                arc_center_y + std::sin(first) * radius_y,
                arc_center_x + std::cos(second) * radius_x,
                arc_center_y + std::sin(second) * radius_y);
        }
    };
    const auto svg_cubic = [&](const double x0, const double y0,
                               const double x1, const double y1,
                               const double x2, const double y2,
                               const double x3, const double y3) {
        constexpr std::uint32_t segments = 12U;
        double previous_x = x0;
        double previous_y = y0;
        for (std::uint32_t index = 1U; index <= segments; ++index) {
            const double amount = static_cast<double>(index)
                / static_cast<double>(segments);
            const double inverse = 1.0 - amount;
            const double next_x =
                inverse * inverse * inverse * x0
                + 3.0 * inverse * inverse * amount * x1
                + 3.0 * inverse * amount * amount * x2
                + amount * amount * amount * x3;
            const double next_y =
                inverse * inverse * inverse * y0
                + 3.0 * inverse * inverse * amount * y1
                + 3.0 * inverse * amount * amount * y2
                + amount * amount * amount * y3;
            svg_line(previous_x, previous_y, next_x, next_y);
            previous_x = next_x;
            previous_y = next_y;
        }
    };

    switch (control.icon) {
    case UiIcon::file_new:
        // Lucide "file-plus".
        svg_arc(6.0, 22.0, 4.0, 20.0, 2.0, 2.0, false, true);
        svg_line(4.0, 20.0, 4.0, 4.0);
        svg_arc(4.0, 4.0, 6.0, 2.0, 2.0, 2.0, false, true);
        svg_line(6.0, 2.0, 14.0, 2.0);
        svg_arc(14.0, 2.0, 15.704, 2.706, 2.4, 2.4, false, true);
        svg_line(15.704, 2.706, 19.292, 6.294);
        svg_arc(19.292, 6.294, 20.0, 8.0, 2.4, 2.4, false, true);
        svg_line(20.0, 8.0, 20.0, 20.0);
        svg_arc(20.0, 20.0, 18.0, 22.0, 2.0, 2.0, false, true);
        svg_line(18.0, 22.0, 6.0, 22.0);
        svg_line(14.0, 2.0, 14.0, 7.0);
        svg_arc(14.0, 7.0, 15.0, 8.0, 1.0, 1.0, false, true);
        svg_line(15.0, 8.0, 20.0, 8.0);
        svg_line(9.0, 15.0, 15.0, 15.0);
        svg_line(12.0, 18.0, 12.0, 12.0);
        break;
    case UiIcon::folder_open:
        // Lucide "folder". The single continuous silhouette stays legible at
        // Home's compact icon size and avoids the visually busy overlapping
        // flap used by the previous open-folder drawing.
        svg_arc(20.0, 20.0, 22.0, 18.0, 2.0, 2.0, false, false);
        svg_line(22.0, 18.0, 22.0, 8.0);
        svg_arc(22.0, 8.0, 20.0, 6.0, 2.0, 2.0, false, false);
        svg_line(20.0, 6.0, 12.1, 6.0);
        svg_arc(12.1, 6.0, 10.41, 5.1, 2.0, 2.0, false, true);
        svg_line(10.41, 5.1, 9.6, 3.9);
        svg_arc(9.6, 3.9, 7.93, 3.0, 2.0, 2.0, false, false);
        svg_line(7.93, 3.0, 4.0, 3.0);
        svg_arc(4.0, 3.0, 2.0, 5.0, 2.0, 2.0, false, false);
        svg_line(2.0, 5.0, 2.0, 18.0);
        svg_arc(2.0, 18.0, 4.0, 20.0, 2.0, 2.0, false, false);
        svg_line(4.0, 20.0, 20.0, 20.0);
        break;
    case UiIcon::home:
        // Lucide "house".
        svg_line(15.0, 21.0, 15.0, 13.0);
        svg_arc(15.0, 13.0, 14.0, 12.0, 1.0, 1.0, false, false);
        svg_line(14.0, 12.0, 10.0, 12.0);
        svg_arc(10.0, 12.0, 9.0, 13.0, 1.0, 1.0, false, false);
        svg_line(9.0, 13.0, 9.0, 21.0);
        svg_arc(3.0, 10.0, 3.709, 8.472, 2.0, 2.0, false, true);
        svg_line(3.709, 8.472, 10.709, 2.472);
        svg_arc(10.709, 2.472, 13.291, 2.472, 2.0, 2.0, false, true);
        svg_line(13.291, 2.472, 20.291, 8.472);
        svg_arc(20.291, 8.472, 21.0, 10.0, 2.0, 2.0, false, true);
        svg_line(21.0, 10.0, 21.0, 19.0);
        svg_arc(21.0, 19.0, 19.0, 21.0, 2.0, 2.0, false, true);
        svg_line(19.0, 21.0, 5.0, 21.0);
        svg_arc(5.0, 21.0, 3.0, 19.0, 2.0, 2.0, false, true);
        svg_line(3.0, 19.0, 3.0, 10.0);
        break;
    case UiIcon::save:
        // Lucide "save".
        svg_arc(15.2, 3.0, 16.6, 3.6, 2.0, 2.0, false, true);
        svg_line(16.6, 3.6, 20.4, 7.4);
        svg_arc(20.4, 7.4, 21.0, 8.8, 2.0, 2.0, false, true);
        svg_line(21.0, 8.8, 21.0, 19.0);
        svg_arc(21.0, 19.0, 19.0, 21.0, 2.0, 2.0, false, true);
        svg_line(19.0, 21.0, 5.0, 21.0);
        svg_arc(5.0, 21.0, 3.0, 19.0, 2.0, 2.0, false, true);
        svg_line(3.0, 19.0, 3.0, 5.0);
        svg_arc(3.0, 5.0, 5.0, 3.0, 2.0, 2.0, false, true);
        svg_line(5.0, 3.0, 15.2, 3.0);
        svg_line(17.0, 21.0, 17.0, 14.0);
        svg_arc(17.0, 14.0, 16.0, 13.0, 1.0, 1.0, false, false);
        svg_line(16.0, 13.0, 8.0, 13.0);
        svg_arc(8.0, 13.0, 7.0, 14.0, 1.0, 1.0, false, false);
        svg_line(7.0, 14.0, 7.0, 21.0);
        svg_line(7.0, 3.0, 7.0, 7.0);
        svg_arc(7.0, 7.0, 8.0, 8.0, 1.0, 1.0, false, true);
        svg_line(8.0, 8.0, 15.0, 8.0);
        break;
    case UiIcon::cursor:
        // Lucide "mouse-pointer-2".
        svg_arc(4.037, 4.688, 4.688, 4.037, 0.495, 0.495, false, true);
        svg_line(4.688, 4.037, 20.688, 10.537);
        svg_arc(20.688, 10.537, 20.625, 11.484, 0.5, 0.5, false, true);
        svg_line(20.625, 11.484, 14.501, 13.064);
        svg_arc(14.501, 13.064, 13.063, 14.499, 2.0, 2.0, false, false);
        svg_line(13.063, 14.499, 11.484, 20.625);
        svg_arc(11.484, 20.625, 10.537, 20.688, 0.5, 0.5, false, true);
        svg_line(10.537, 20.688, 4.037, 4.688);
        break;
    case UiIcon::hand:
        // Lucide "hand".
        svg_line(18.0, 11.0, 18.0, 6.0);
        svg_arc(18.0, 6.0, 16.0, 4.0, 2.0, 2.0, false, false);
        svg_arc(16.0, 4.0, 14.0, 6.0, 2.0, 2.0, false, false);
        svg_line(14.0, 10.0, 14.0, 4.0);
        svg_arc(14.0, 4.0, 12.0, 2.0, 2.0, 2.0, false, false);
        svg_arc(12.0, 2.0, 10.0, 4.0, 2.0, 2.0, false, false);
        svg_line(10.0, 4.0, 10.0, 6.0);
        svg_line(10.0, 10.5, 10.0, 6.0);
        svg_arc(10.0, 6.0, 8.0, 4.0, 2.0, 2.0, false, false);
        svg_arc(8.0, 4.0, 6.0, 6.0, 2.0, 2.0, false, false);
        svg_line(6.0, 6.0, 6.0, 14.0);
        svg_arc(18.0, 8.0, 22.0, 8.0, 2.0, 2.0, true, true);
        svg_line(22.0, 8.0, 22.0, 14.0);
        svg_arc(22.0, 14.0, 14.0, 22.0, 8.0, 8.0, false, true);
        svg_line(14.0, 22.0, 12.0, 22.0);
        svg_cubic(12.0, 22.0, 9.2, 22.0, 7.5, 21.14, 6.01, 19.66);
        svg_line(6.01, 19.66, 2.41, 16.06);
        svg_arc(2.41, 16.06, 5.24, 13.24, 2.0, 2.0, false, true);
        svg_line(5.24, 13.24, 7.0, 15.0);
        break;
    case UiIcon::pencil:
        // Lucide "pencil".
        svg_arc(21.174, 6.812, 17.188, 2.825, 1.0, 1.0, false, false);
        svg_line(17.188, 2.825, 3.842, 16.174);
        svg_arc(3.842, 16.174, 3.342, 17.004, 2.0, 2.0, false, false);
        svg_line(3.342, 17.004, 2.021, 21.356);
        svg_arc(2.021, 21.356, 2.644, 21.978, 0.5, 0.5, false, false);
        svg_line(2.644, 21.978, 6.997, 20.658);
        svg_arc(6.997, 20.658, 7.827, 20.161, 2.0, 2.0, false, false);
        svg_line(7.827, 20.161, 21.174, 6.812);
        svg_line(15.0, 5.0, 19.0, 9.0);
        break;
    case UiIcon::line: {
        double width = stroke;
        if (control.action >= UiAction::width_thin
            && control.action <= UiAction::width_heavy) {
            width = std::clamp(
                Toolbar::width_for(control.action) * 0.42 * unit,
                1.2 * unit,
                6.0 * unit);
            svg_line(4.0, 12.0, 20.0, 12.0, width);
            break;
        }
        // Lucide "slash".
        svg_line(22.0, 2.0, 2.0, 22.0, width);
        break;
    }
    case UiIcon::minus:
        // Lucide "minus".
        svg_line(5.0, 12.0, 19.0, 12.0);
        break;
    case UiIcon::plus:
        // Lucide "plus".
        svg_line(5.0, 12.0, 19.0, 12.0);
        svg_line(12.0, 5.0, 12.0, 19.0);
        break;
    case UiIcon::rectangle:
        if (control.action >= UiAction::roundness_square
            && control.action <= UiAction::roundness_full) {
            const double radius =
                Toolbar::roundness_for(control.action) * 12.0;
            if (radius <= 1.0e-9) {
                rect(-10.0, -6.0, 20.0, 12.0);
            } else {
                rounded_box(-10.0, -6.0, 20.0, 12.0, radius);
            }
        } else {
            // Lucide "rectangle-horizontal".
            rounded_box(-10.0, -6.0, 20.0, 12.0, 2.0);
        }
        break;
    case UiIcon::ellipse:
        // Lucide "ellipse".
        ring(10.0, 6.0);
        break;
    case UiIcon::ban:
        // Lucide "ban".
        ring(10.0, 10.0);
        svg_line(4.93, 4.93, 19.07, 19.07);
        break;
    case UiIcon::undo:
        // Lucide "undo-2".
        svg_line(9.0, 14.0, 4.0, 9.0);
        svg_line(4.0, 9.0, 9.0, 4.0);
        svg_line(4.0, 9.0, 14.5, 9.0);
        svg_arc(14.5, 9.0, 20.0, 14.5, 5.5, 5.5, false, true);
        svg_arc(20.0, 14.5, 14.5, 20.0, 5.5, 5.5, false, true);
        svg_line(14.5, 20.0, 11.0, 20.0);
        break;
    case UiIcon::redo:
        // Lucide "redo-2".
        svg_line(15.0, 14.0, 20.0, 9.0);
        svg_line(20.0, 9.0, 15.0, 4.0);
        svg_line(20.0, 9.0, 9.5, 9.0);
        svg_arc(9.5, 9.0, 4.0, 14.5, 5.5, 5.5, false, false);
        svg_arc(4.0, 14.5, 9.5, 20.0, 5.5, 5.5, false, false);
        svg_line(9.5, 20.0, 13.0, 20.0);
        break;
    case UiIcon::copy:
        // Lucide "copy".
        rounded_box(-4.0, -4.0, 14.0, 14.0, 2.0);
        svg_cubic(4.0, 16.0, 2.9, 16.0, 2.0, 15.1, 2.0, 14.0);
        svg_line(2.0, 14.0, 2.0, 4.0);
        svg_cubic(2.0, 4.0, 2.0, 2.9, 2.9, 2.0, 4.0, 2.0);
        svg_line(4.0, 2.0, 14.0, 2.0);
        svg_cubic(14.0, 2.0, 15.1, 2.0, 16.0, 2.9, 16.0, 4.0);
        break;
    case UiIcon::duplicate:
        // Lucide "copy-plus".
        svg_line(15.0, 12.0, 15.0, 18.0);
        svg_line(12.0, 15.0, 18.0, 15.0);
        rounded_box(-4.0, -4.0, 14.0, 14.0, 2.0);
        svg_cubic(4.0, 16.0, 2.9, 16.0, 2.0, 15.1, 2.0, 14.0);
        svg_line(2.0, 14.0, 2.0, 4.0);
        svg_cubic(2.0, 4.0, 2.0, 2.9, 2.9, 2.0, 4.0, 2.0);
        svg_line(4.0, 2.0, 14.0, 2.0);
        svg_cubic(14.0, 2.0, 15.1, 2.0, 16.0, 2.9, 16.0, 4.0);
        break;
    case UiIcon::cut:
        // Lucide "scissors".
        ring_at(-6.0, -6.0, 3.0, 3.0);
        svg_line(8.12, 8.12, 12.0, 12.0);
        svg_line(20.0, 4.0, 8.12, 15.88);
        ring_at(-6.0, 6.0, 3.0, 3.0);
        svg_line(14.8, 14.8, 20.0, 20.0);
        break;
    case UiIcon::paste:
        // Lucide "clipboard-paste".
        svg_line(11.0, 14.0, 21.0, 14.0);
        svg_line(16.0, 4.0, 18.0, 4.0);
        svg_arc(18.0, 4.0, 20.0, 6.0, 2.0, 2.0, false, true);
        svg_line(20.0, 6.0, 20.0, 7.344);
        svg_line(17.0, 18.0, 21.0, 14.0);
        svg_line(21.0, 14.0, 17.0, 10.0);
        svg_line(8.0, 4.0, 6.0, 4.0);
        svg_arc(6.0, 4.0, 4.0, 6.0, 2.0, 2.0, false, false);
        svg_line(4.0, 6.0, 4.0, 20.0);
        svg_arc(4.0, 20.0, 6.0, 22.0, 2.0, 2.0, false, false);
        svg_line(6.0, 22.0, 18.0, 22.0);
        svg_arc(
            18.0, 22.0, 19.793, 20.887, 2.0, 2.0, false, false);
        rounded_box(-4.0, -10.0, 8.0, 4.0, 1.0);
        break;
    case UiIcon::trash:
        // Lucide "trash-2".
        svg_line(10.0, 11.0, 10.0, 17.0);
        svg_line(14.0, 11.0, 14.0, 17.0);
        svg_line(19.0, 6.0, 19.0, 20.0);
        svg_arc(19.0, 20.0, 17.0, 22.0, 2.0, 2.0, false, true);
        svg_line(17.0, 22.0, 7.0, 22.0);
        svg_arc(7.0, 22.0, 5.0, 20.0, 2.0, 2.0, false, true);
        svg_line(5.0, 20.0, 5.0, 6.0);
        svg_line(3.0, 6.0, 21.0, 6.0);
        svg_line(8.0, 6.0, 8.0, 4.0);
        svg_arc(8.0, 4.0, 10.0, 2.0, 2.0, 2.0, false, true);
        svg_line(10.0, 2.0, 14.0, 2.0);
        svg_arc(14.0, 2.0, 16.0, 4.0, 2.0, 2.0, false, true);
        svg_line(16.0, 4.0, 16.0, 6.0);
        break;
    case UiIcon::zoom_out:
    case UiIcon::zoom_in: {
        // Lucide "zoom-out" / "zoom-in".
        ring_at(-1.0, -1.0, 8.0, 8.0);
        svg_line(21.0, 21.0, 16.65, 16.65);
        svg_line(8.0, 11.0, 14.0, 11.0);
        if (control.icon == UiIcon::zoom_in) {
            svg_line(11.0, 8.0, 11.0, 14.0);
        }
        break;
    }
    case UiIcon::zoom_reset:
        break;
    case UiIcon::settings:
        // Lucide "sliders-horizontal".
        svg_line(3.0, 6.0, 7.0, 6.0);
        svg_line(13.0, 6.0, 21.0, 6.0);
        svg_line(3.0, 18.0, 11.0, 18.0);
        svg_line(17.0, 18.0, 21.0, 18.0);
        svg_line(10.0, 3.0, 10.0, 9.0);
        svg_line(14.0, 15.0, 14.0, 21.0);
        break;
    case UiIcon::close:
        // Lucide "x".
        svg_line(6.0, 6.0, 18.0, 18.0);
        svg_line(6.0, 18.0, 18.0, 6.0);
        break;
    case UiIcon::check:
        // Lucide "check".
        svg_line(20.0, 6.0, 9.0, 17.0);
        svg_line(9.0, 17.0, 4.0, 12.0);
        break;
    case UiIcon::chevron_left:
        // Lucide "chevron-left".
        svg_line(15.0, 18.0, 9.0, 12.0);
        svg_line(9.0, 12.0, 15.0, 6.0);
        break;
    case UiIcon::chevron_right:
        // Lucide "chevron-right".
        svg_line(9.0, 18.0, 15.0, 12.0);
        svg_line(15.0, 12.0, 9.0, 6.0);
        break;
    case UiIcon::chevron_down:
        // Lucide "chevron-down".
        svg_line(6.0, 9.0, 12.0, 15.0);
        svg_line(12.0, 15.0, 18.0, 9.0);
        break;
    case UiIcon::rename:
        // Text cursor inside an input field, on the Lucide optical grid.
        svg_line(10.0, 3.0, 12.0, 5.0);
        svg_line(12.0, 5.0, 14.0, 3.0);
        svg_line(12.0, 5.0, 12.0, 19.0);
        svg_line(10.0, 21.0, 12.0, 19.0);
        svg_line(12.0, 19.0, 14.0, 21.0);
        svg_line(8.0, 6.0, 4.0, 6.0);
        svg_line(4.0, 6.0, 4.0, 18.0);
        svg_line(4.0, 18.0, 8.0, 18.0);
        svg_line(16.0, 6.0, 20.0, 6.0);
        svg_line(20.0, 6.0, 20.0, 18.0);
        svg_line(20.0, 18.0, 16.0, 18.0);
        break;
    case UiIcon::theme:
        // Lucide "sun".
        ring(4.0, 4.0);
        svg_line(12.0, 2.0, 12.0, 4.0);
        svg_line(12.0, 20.0, 12.0, 22.0);
        svg_line(4.93, 4.93, 6.34, 6.34);
        svg_line(17.66, 17.66, 19.07, 19.07);
        svg_line(2.0, 12.0, 4.0, 12.0);
        svg_line(20.0, 12.0, 22.0, 12.0);
        svg_line(6.34, 17.66, 4.93, 19.07);
        svg_line(19.07, 4.93, 17.66, 6.34);
        break;
    case UiIcon::moon:
        // Crescent on the same 24-unit optical grid as the outline icons.
        svg_cubic(20.8, 13.0, 20.3, 17.5, 16.5, 21.0, 12.0, 21.0);
        svg_cubic(12.0, 21.0, 7.0, 21.0, 3.0, 17.0, 3.0, 12.0);
        svg_cubic(3.0, 12.0, 3.0, 7.5, 6.4, 3.8, 11.0, 3.1);
        svg_cubic(11.0, 3.1, 9.1, 5.3, 9.0, 8.7, 11.1, 11.0);
        svg_cubic(11.1, 11.0, 13.4, 13.6, 17.3, 14.4, 20.8, 13.0);
        break;
    case UiIcon::background:
        // Lucide "grid-2x2".
        svg_line(12.0, 3.0, 12.0, 21.0);
        svg_line(3.0, 12.0, 21.0, 12.0);
        rounded_box(-9.0, -9.0, 18.0, 18.0, 2.0);
        break;
    case UiIcon::custom_color: {
        // Palette silhouette on the same optical grid as the outline icons.
        // This opens color editing; it does not sample pixels from the board.
        svg_cubic(12.0, 22.0, 6.48, 22.0, 2.0, 17.52, 2.0, 12.0);
        svg_cubic(2.0, 12.0, 2.0, 6.48, 6.48, 2.0, 12.0, 2.0);
        svg_cubic(12.0, 2.0, 17.52, 2.0, 22.0, 6.48, 22.0, 12.0);
        svg_cubic(22.0, 12.0, 22.0, 14.21, 20.21, 16.0, 18.0, 16.0);
        svg_line(18.0, 16.0, 16.0, 16.0);
        svg_cubic(16.0, 16.0, 14.90, 16.0, 14.0, 16.90, 14.0, 18.0);
        svg_cubic(14.0, 18.0, 14.0, 18.45, 14.3, 18.9, 14.3, 19.5);
        svg_cubic(14.3, 19.5, 14.3, 20.88, 13.38, 22.0, 12.0, 22.0);
        for (const Vec2d dot : std::array<Vec2d, 4>{{
                 {8.0, 8.0}, {13.0, 6.0}, {18.0, 10.0}, {6.0, 13.0}}}) {
            append_screen_circle(output,
                {center.x + (dot.x - 12.0) * unit,
                    center.y + (dot.y - 12.0) * unit},
                1.25 * unit, camera, color, 12U);
        }
        break;
    }
    case UiIcon::back:
        // Lucide "arrow-left".
        svg_line(12.0, 19.0, 5.0, 12.0);
        svg_line(5.0, 12.0, 12.0, 5.0);
        svg_line(19.0, 12.0, 5.0, 12.0);
        break;
    case UiIcon::info:
        // Lucide "info".
        ring(10.0, 10.0);
        svg_line(12.0, 16.0, 12.0, 12.0);
        svg_line(12.0, 8.0, 12.01, 8.0);
        break;
    case UiIcon::grid_solid:
    case UiIcon::grid_dot:
    case UiIcon::grid_square:
    case UiIcon::grid_graph:
    case UiIcon::grid_hybrid:
    case UiIcon::grid_diamond:
    case UiIcon::grid_wide_rule:
    case UiIcon::grid_triangle:
    case UiIcon::grid_narrow_rule: {
        // The grid tiles are larger than ordinary buttons, so these previews
        // are drawn at a bigger fixed extent to fill the tile.
        constexpr double h = 15.0;
        const double thin = 1.3 * unit;
        const auto vline = [&](const double gx) { line(gx, -h, gx, h, thin); };
        const auto hline = [&](const double gy) { line(-h, gy, h, gy, thin); };
        const auto dot = [&](const double gx, const double gy) {
            append_screen_circle(
                output,
                {center.x + gx * unit, center.y + gy * unit},
                1.4 * unit,
                camera,
                color,
                12U);
        };
        switch (control.icon) {
        case UiIcon::grid_solid:
            rect(-h, -h, 2.0 * h, 2.0 * h);
            break;
        case UiIcon::grid_dot:
            for (int row = -1; row <= 1; ++row) {
                for (int col = -1; col <= 1; ++col) {
                    dot(col * h * 0.66, row * h * 0.66);
                }
            }
            break;
        case UiIcon::grid_square:
            for (int step = -1; step <= 1; ++step) {
                vline(step * h * 0.66);
                hline(step * h * 0.66);
            }
            break;
        case UiIcon::grid_graph:
            for (int step = -3; step <= 3; ++step) {
                vline(step * h / 3.0);
                hline(step * h / 3.0);
            }
            break;
        case UiIcon::grid_hybrid:
            for (int step = -1; step <= 1; ++step) {
                hline(step * h * 0.66);
            }
            for (int row = -1; row <= 1; ++row) {
                for (int col = -1; col <= 1; ++col) {
                    dot(col * h * 0.66, row * h * 0.66);
                }
            }
            break;
        case UiIcon::grid_diamond:
            line(0.0, -h, h, 0.0, thin);
            line(h, 0.0, 0.0, h, thin);
            line(0.0, h, -h, 0.0, thin);
            line(-h, 0.0, 0.0, -h, thin);
            line(0.0, -h * 0.5, h * 0.5, 0.0, thin);
            line(h * 0.5, 0.0, 0.0, h * 0.5, thin);
            line(0.0, h * 0.5, -h * 0.5, 0.0, thin);
            line(-h * 0.5, 0.0, 0.0, -h * 0.5, thin);
            break;
        case UiIcon::grid_wide_rule:
            hline(-h * 0.5);
            hline(h * 0.16);
            hline(h * 0.82);
            break;
        case UiIcon::grid_narrow_rule:
            for (int step = -2; step <= 2; ++step) {
                hline(step * h * 0.42);
            }
            break;
        case UiIcon::grid_triangle:
            hline(-h);
            hline(h);
            line(-h, h, -h * 0.33, -h, thin);
            line(-h * 0.33, -h, h * 0.33, h, thin);
            line(h * 0.33, h, h, -h, thin);
            break;
        default:
            break;
        }
        break;
    }
    case UiIcon::none:
        break;
    }
}

RenderColor to_render_color(const sawer::Color color)
{
    return {
        static_cast<float>(color.red) / 255.0F,
        static_cast<float>(color.green) / 255.0F,
        static_cast<float>(color.blue) / 255.0F,
    };
}

struct CameraRelativeGeometry final {
    std::vector<GeometryVertex>& vertices;
    Vec2d camera_position;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return vertices.size();
    }

    void clear() noexcept
    {
        vertices.clear();
    }

    void resize(const std::size_t vertex_count)
    {
        vertices.resize(vertex_count);
    }
};

void append_world_vertex(
    std::vector<CachedWorldVertex>& output,
    const Vec2d point,
    const RenderColor color)
{
    output.push_back({
        point,
        packed_color(color),
    });
}

void append_world_vertex(
    CameraRelativeGeometry& output,
    const Vec2d point,
    const RenderColor color)
{
    output.vertices.push_back({
        {
            static_cast<float>(
                point.x - output.camera_position.x),
            static_cast<float>(
                point.y - output.camera_position.y),
        },
        packed_color(color),
    });
}

// Completed long strokes emit triangle-aligned pages instead of dropping
// samples to fit one mesh. Stopping the cache collector does not change data.
struct StrokePageStopped final {};
struct PagedStrokeGeometry final {
    std::vector<CachedWorldVertex>& page;
    const std::function<bool(std::span<const CachedWorldVertex>)>& consume;
    Aabb clip;

    void flush()
    {
        if (page.empty()) return;
        if (!consume(page)) throw StrokePageStopped{};
        page.clear();
    }
};

void append_world_triangle(
    PagedStrokeGeometry& output, const Vec2d first, const Vec2d second,
    const Vec2d third, const RenderColor color)
{
    const Aabb bounds{std::min({first.x, second.x, third.x}),
        std::min({first.y, second.y, third.y}),
        std::max({first.x, second.x, third.x}),
        std::max({first.y, second.y, third.y})};
    if (!bounds.intersects(output.clip)) return;
    if (output.page.size() == stroke_page_vertex_count) output.flush();
    const auto packed = packed_color(color);
    output.page.push_back({first, packed});
    output.page.push_back({second, packed});
    output.page.push_back({third, packed});
}

template <typename Output>
void append_world_triangle(
    Output& output,
    const Vec2d first,
    const Vec2d second,
    const Vec2d third,
    const RenderColor color)
{
    append_world_vertex(output, first, color);
    append_world_vertex(output, second, color);
    append_world_vertex(output, third, color);
}

template <typename Output>
void append_world_quad(
    Output& output,
    const Vec2d first,
    const Vec2d second,
    const Vec2d third,
    const Vec2d fourth,
    const RenderColor color)
{
    append_world_triangle(output, first, second, third, color);
    append_world_triangle(output, first, third, fourth, color);
}

template <typename Output>
void append_world_circle(
    Output& output,
    const Vec2d center,
    const double radius,
    const RenderColor color,
    const std::uint32_t segments = 12U)
{
    for (std::uint32_t index = 0U; index < segments; ++index) {
        const double first_angle =
            2.0 * pi * static_cast<double>(index)
            / static_cast<double>(segments);
        const double second_angle =
            2.0 * pi * static_cast<double>(index + 1U)
            / static_cast<double>(segments);
        append_world_triangle(
            output,
            center,
            {
                center.x + std::cos(first_angle) * radius,
                center.y + std::sin(first_angle) * radius,
            },
            {
                center.x + std::cos(second_angle) * radius,
                center.y + std::sin(second_angle) * radius,
            },
            color);
    }
}

template <typename Output>
void append_segment_quad(
    Output& output,
    const Vec2d start,
    const Vec2d end,
    const double width,
    const RenderColor color)
{
    const double delta_x = end.x - start.x;
    const double delta_y = end.y - start.y;
    const double length = std::hypot(delta_x, delta_y);
    if (length < 1.0e-12) {
        return;
    }
    const double normal_x = -delta_y / length * width * 0.5;
    const double normal_y = delta_x / length * width * 0.5;
    append_world_quad(
        output,
        {start.x + normal_x, start.y + normal_y},
        {end.x + normal_x, end.y + normal_y},
        {end.x - normal_x, end.y - normal_y},
        {start.x - normal_x, start.y - normal_y},
        color);
}

template <typename Output>
void append_round_fan(
    Output& output,
    const Vec2d center,
    const Vec2d first_offset,
    const double sweep,
    const RenderColor color,
    const std::uint32_t circle_segments)
{
    const double radius = std::hypot(first_offset.x, first_offset.y);
    if (radius <= 1.0e-12 || std::abs(sweep) <= 1.0e-12) {
        return;
    }
    const double start_angle =
        std::atan2(first_offset.y, first_offset.x);
    const auto steps = static_cast<std::uint32_t>(std::max(
        1.0,
        std::ceil(
            std::abs(sweep) / (2.0 * pi)
            * static_cast<double>(std::max(circle_segments, 4U)))));
    for (std::uint32_t step = 0U; step < steps; ++step) {
        const double first_angle = start_angle
            + sweep * static_cast<double>(step)
                / static_cast<double>(steps);
        const double second_angle = start_angle
            + sweep * static_cast<double>(step + 1U)
                / static_cast<double>(steps);
        append_world_triangle(
            output,
            center,
            {
                center.x + std::cos(first_angle) * radius,
                center.y + std::sin(first_angle) * radius,
            },
            {
                center.x + std::cos(second_angle) * radius,
                center.y + std::sin(second_angle) * radius,
            },
            color);
    }
}

struct StrokeSegmentFrame final {
    Vec2d direction;
    Vec2d normal;
};

std::optional<StrokeSegmentFrame> stroke_segment_frame(
    const Vec2d start,
    const Vec2d end) noexcept
{
    const double delta_x = end.x - start.x;
    const double delta_y = end.y - start.y;
    const double length = std::hypot(delta_x, delta_y);
    if (length <= 1.0e-12) {
        return std::nullopt;
    }
    const Vec2d direction{delta_x / length, delta_y / length};
    return StrokeSegmentFrame{
        direction,
        {-direction.y, direction.x},
    };
}

template <typename Output>
void append_round_join(
    Output& output,
    const Vec2d center,
    const StrokeSegmentFrame previous,
    const StrokeSegmentFrame next,
    const double radius,
    const RenderColor color,
    const std::uint32_t circle_segments)
{
    const double cross = previous.direction.x * next.direction.y
        - previous.direction.y * next.direction.x;
    const double dot = std::clamp(
        previous.direction.x * next.direction.x
            + previous.direction.y * next.direction.y,
        -1.0,
        1.0);
    const double turn = std::atan2(cross, dot);
    if (std::abs(turn) <= 1.0e-6) {
        return;
    }

    // Only the convex side of a join needs an arc. Segment quads already
    // overlap on the inside of the turn.
    const double side = turn > 0.0 ? -1.0 : 1.0;
    append_round_fan(
        output,
        center,
        {
            previous.normal.x * radius * side,
            previous.normal.y * radius * side,
        },
        turn,
        color,
        circle_segments);
}

template <typename Output>
void tessellate_polyline(
    Output& output,
    const std::vector<Vec2d>& points,
    const Style& style,
    const std::uint32_t circle_segments)
{
    if (points.empty()) {
        return;
    }
    const double width = std::max(style.stroke_width, 0.5);
    const RenderColor color = to_render_color(style.stroke);
    if (points.size() == 1U) {
        append_world_circle(
            output, points.front(), width * 0.5, color, circle_segments);
        return;
    }

    std::size_t first_segment = 1U;
    std::optional<StrokeSegmentFrame> frame;
    while (first_segment < points.size()) {
        frame = stroke_segment_frame(
            points[first_segment - 1U], points[first_segment]);
        if (frame.has_value()) {
            break;
        }
        ++first_segment;
    }
    if (!frame.has_value()) {
        append_world_circle(
            output, points.front(), width * 0.5, color, circle_segments);
        return;
    }

    const double radius = width * 0.5;
    append_round_fan(
        output,
        points[first_segment - 1U],
        {-frame->normal.x * radius, -frame->normal.y * radius},
        -pi,
        color,
        circle_segments);
    append_segment_quad(
        output,
        points[first_segment - 1U],
        points[first_segment],
        width,
        color);

    StrokeSegmentFrame previous = *frame;
    Vec2d endpoint = points[first_segment];
    for (std::size_t index = first_segment + 1U;
         index < points.size(); ++index) {
        const auto next = stroke_segment_frame(
            points[index - 1U], points[index]);
        if (!next.has_value()) {
            continue;
        }
        append_round_join(
            output,
            points[index - 1U],
            previous,
            *next,
            radius,
            color,
            circle_segments);
        append_segment_quad(
            output,
            points[index - 1U],
            points[index],
            width,
            color);
        previous = *next;
        endpoint = points[index];
    }
    append_round_fan(
        output,
        endpoint,
        {previous.normal.x * radius, previous.normal.y * radius},
        -pi,
        color,
        circle_segments);
}

std::size_t round_cap_value_count(
    const std::uint32_t circle_segments) noexcept
{
    const auto steps = static_cast<std::size_t>(std::ceil(
        static_cast<double>(std::max(circle_segments, 4U)) * 0.5));
    return steps * 3U;
}

std::size_t maximum_stroke_segments(
    const std::uint32_t circle_segments,
    const std::size_t vertex_budget) noexcept
{
    // Treat every retained segment as an isolated two-point run. This is more
    // conservative than a continuous polyline, but also covers indexed
    // strokes split into many visible runs without exceeding the GPU buffer.
    const std::size_t vertices_per_segment =
        6U + round_cap_value_count(circle_segments) * 2U;
    return std::max<std::size_t>(
        1U, vertex_budget / vertices_per_segment);
}

bool polyline_exceeds_vertex_budget(
    const std::vector<Vec2d>& points,
    const std::uint32_t circle_segments,
    const std::size_t vertex_budget) noexcept
{
    if (points.empty()) {
        return false;
    }
    const std::size_t cap_vertices =
        round_cap_value_count(circle_segments);
    if (points.size() == 1U) {
        return cap_vertices * 2U > vertex_budget;
    }

    std::size_t first_segment = 1U;
    std::optional<StrokeSegmentFrame> previous;
    while (first_segment < points.size()) {
        previous = stroke_segment_frame(
            points[first_segment - 1U], points[first_segment]);
        if (previous.has_value()) {
            break;
        }
        ++first_segment;
    }
    if (!previous.has_value()) {
        return cap_vertices * 2U > vertex_budget;
    }

    std::size_t vertices = cap_vertices + 6U;
    const auto add_exceeds = [&](const std::size_t addition) {
        if (addition > vertex_budget - std::min(vertices, vertex_budget)) {
            return true;
        }
        vertices += addition;
        return false;
    };
    for (std::size_t index = first_segment + 1U;
         index < points.size(); ++index) {
        const auto next = stroke_segment_frame(
            points[index - 1U], points[index]);
        if (!next.has_value()) {
            continue;
        }
        const double cross =
            previous->direction.x * next->direction.y
            - previous->direction.y * next->direction.x;
        const double dot = std::clamp(
            previous->direction.x * next->direction.x
                + previous->direction.y * next->direction.y,
            -1.0,
            1.0);
        const double turn = std::atan2(cross, dot);
        std::size_t join_vertices = 0U;
        if (std::abs(turn) > 1.0e-6) {
            const auto steps = static_cast<std::size_t>(std::max(
                1.0,
                std::ceil(
                    std::abs(turn) / (2.0 * pi)
                    * static_cast<double>(
                        std::max(circle_segments, 4U)))));
            join_vertices = steps * 3U;
        }
        if (add_exceeds(join_vertices + 6U)) {
            return true;
        }
        previous = next;
    }
    return add_exceeds(cap_vertices);
}

void decimate_polyline_to_vertex_budget(
    std::vector<Vec2d>& points,
    const std::uint32_t circle_segments,
    const std::size_t vertex_budget)
{
    const std::size_t segment_budget = maximum_stroke_segments(
        circle_segments, vertex_budget);
    if (points.size() <= segment_budget + 1U
        || !polyline_exceeds_vertex_budget(
            points, circle_segments, vertex_budget)) {
        return;
    }
    const std::size_t stride = std::max<std::size_t>(
        2U,
        (points.size() - 1U + segment_budget - 1U)
            / segment_budget);
    std::size_t destination = 0U;
    for (std::size_t source = 0U;
         source < points.size();
         source += stride) {
        points[destination++] = points[source];
    }
    if (destination == 0U
        || points[destination - 1U] != points.back()) {
        points[destination++] = points.back();
    }
    points.resize(destination);
}

template <typename Output>
bool extend_tessellated_polyline(
    Output& output,
    const std::vector<Vec2d>& points,
    const std::size_t previous_point_count,
    std::size_t& end_cap_offset,
    const Style& style,
    const std::uint32_t circle_segments)
{
    if (previous_point_count < 2U
        || previous_point_count >= points.size()
        || end_cap_offset > output.size()) {
        return false;
    }

    auto previous = stroke_segment_frame(
        points[previous_point_count - 2U],
        points[previous_point_count - 1U]);
    if (!previous.has_value()) {
        return false;
    }

    output.resize(end_cap_offset);
    const double width = std::max(style.stroke_width, 0.5);
    const double radius = width * 0.5;
    const RenderColor color = to_render_color(style.stroke);
    Vec2d endpoint = points[previous_point_count - 1U];
    for (std::size_t index = previous_point_count;
         index < points.size(); ++index) {
        const auto next = stroke_segment_frame(
            points[index - 1U], points[index]);
        if (!next.has_value()) {
            continue;
        }
        append_round_join(
            output,
            points[index - 1U],
            *previous,
            *next,
            radius,
            color,
            circle_segments);
        append_segment_quad(
            output,
            points[index - 1U],
            points[index],
            width,
            color);
        previous = next;
        endpoint = points[index];
    }
    end_cap_offset = output.size();
    append_round_fan(
        output,
        endpoint,
        {previous->normal.x * radius, previous->normal.y * radius},
        -pi,
        color,
        circle_segments);
    return true;
}

struct RoundedRectanglePoint final {
    Vec2d position;
    Vec2d outward_normal;
};

std::vector<RoundedRectanglePoint> rounded_rectangle_perimeter(
    const RectangleShape& rectangle,
    const std::uint32_t corner_segments)
{
    const double min_x = std::min(rectangle.first.x, rectangle.second.x);
    const double min_y = std::min(rectangle.first.y, rectangle.second.y);
    const double max_x = std::max(rectangle.first.x, rectangle.second.x);
    const double max_y = std::max(rectangle.first.y, rectangle.second.y);
    const double radius = std::clamp(rectangle.roundness, 0.0, 0.5)
        * std::min(max_x - min_x, max_y - min_y);
    if (radius <= 1.0e-12) {
        return {};
    }

    const std::uint32_t steps = std::max(corner_segments, 2U);
    const std::array<Vec2d, 4> centers{{
        {min_x + radius, min_y + radius},
        {max_x - radius, min_y + radius},
        {max_x - radius, max_y - radius},
        {min_x + radius, max_y - radius},
    }};
    constexpr std::array<double, 4> start_angles{{
        pi,
        1.5 * pi,
        0.0,
        0.5 * pi,
    }};

    std::vector<RoundedRectanglePoint> perimeter;
    perimeter.reserve(
        centers.size() * static_cast<std::size_t>(steps + 1U));
    for (std::size_t corner = 0U; corner < centers.size(); ++corner) {
        for (std::uint32_t step = 0U; step <= steps; ++step) {
            const double angle = start_angles[corner]
                + static_cast<double>(step) * pi * 0.5
                    / static_cast<double>(steps);
            const Vec2d normal{std::cos(angle), std::sin(angle)};
            perimeter.push_back({
                {
                    centers[corner].x + normal.x * radius,
                    centers[corner].y + normal.y * radius,
                },
                normal,
            });
        }
    }
    return perimeter;
}

void tessellate_rectangle(
    std::vector<CachedWorldVertex>& output,
    const RectangleShape& rectangle,
    const Style& style,
    const std::uint32_t detail)
{
    const double min_x = std::min(rectangle.first.x, rectangle.second.x);
    const double min_y = std::min(rectangle.first.y, rectangle.second.y);
    const double max_x = std::max(rectangle.first.x, rectangle.second.x);
    const double max_y = std::max(rectangle.first.y, rectangle.second.y);
    const auto perimeter = rounded_rectangle_perimeter(
        rectangle, std::max(detail / 4U, 2U));
    if (!perimeter.empty()) {
        const Vec2d center{
            (min_x + max_x) * 0.5,
            (min_y + max_y) * 0.5,
        };
        if (style.fill.has_value()) {
            const RenderColor fill = to_render_color(*style.fill);
            for (std::size_t index = 0U; index < perimeter.size(); ++index) {
                const std::size_t next = (index + 1U) % perimeter.size();
                append_world_triangle(
                    output,
                    center,
                    perimeter[index].position,
                    perimeter[next].position,
                    fill);
            }
        }

        const RenderColor stroke = to_render_color(style.stroke);
        const double half_width =
            std::max(style.stroke_width, 0.5) * 0.5;
        for (std::size_t index = 0U; index < perimeter.size(); ++index) {
            const std::size_t next = (index + 1U) % perimeter.size();
            const auto& first = perimeter[index];
            const auto& second = perimeter[next];
            append_world_quad(
                output,
                {
                    first.position.x
                        + first.outward_normal.x * half_width,
                    first.position.y
                        + first.outward_normal.y * half_width,
                },
                {
                    second.position.x
                        + second.outward_normal.x * half_width,
                    second.position.y
                        + second.outward_normal.y * half_width,
                },
                {
                    second.position.x
                        - second.outward_normal.x * half_width,
                    second.position.y
                        - second.outward_normal.y * half_width,
                },
                {
                    first.position.x
                        - first.outward_normal.x * half_width,
                    first.position.y
                        - first.outward_normal.y * half_width,
                },
                stroke);
        }
        return;
    }

    if (style.fill.has_value()) {
        append_world_quad(
            output,
            {min_x, min_y},
            {max_x, min_y},
            {max_x, max_y},
            {min_x, max_y},
            to_render_color(*style.fill));
    }

    const RenderColor color = to_render_color(style.stroke);
    const double width = std::max(style.stroke_width, 0.5);
    append_segment_quad(output, {min_x, min_y}, {max_x, min_y}, width, color);
    append_segment_quad(output, {max_x, min_y}, {max_x, max_y}, width, color);
    append_segment_quad(output, {max_x, max_y}, {min_x, max_y}, width, color);
    append_segment_quad(output, {min_x, max_y}, {min_x, min_y}, width, color);
}

std::uint32_t ellipse_detail(const Ellipse& ellipse, const double zoom)
{
    const double radius_x = std::abs(ellipse.second.x - ellipse.first.x) * 0.5;
    const double radius_y = std::abs(ellipse.second.y - ellipse.first.y) * 0.5;
    const double screen_radius = std::max(radius_x, radius_y) * zoom;
    return static_cast<std::uint32_t>(
        std::clamp(std::ceil(screen_radius * 0.4), 24.0, 128.0));
}

void tessellate_ellipse(
    std::vector<CachedWorldVertex>& output,
    const Ellipse& ellipse,
    const Style& style,
    const std::uint32_t segments)
{
    const Vec2d center{
        (ellipse.first.x + ellipse.second.x) * 0.5,
        (ellipse.first.y + ellipse.second.y) * 0.5,
    };
    const double radius_x = std::abs(ellipse.second.x - ellipse.first.x) * 0.5;
    const double radius_y = std::abs(ellipse.second.y - ellipse.first.y) * 0.5;
    const double half_width = std::max(style.stroke_width, 0.5) * 0.5;
    const RenderColor stroke = to_render_color(style.stroke);
    constexpr double epsilon = 1.0e-12;

    if (radius_x <= epsilon || radius_y <= epsilon) {
        if (radius_x <= epsilon && radius_y <= epsilon) {
            append_world_circle(
                output, center, half_width, stroke, segments);
            return;
        }
        const Vec2d first = radius_x > radius_y
            ? Vec2d{center.x - radius_x, center.y}
            : Vec2d{center.x, center.y - radius_y};
        const Vec2d second = radius_x > radius_y
            ? Vec2d{center.x + radius_x, center.y}
            : Vec2d{center.x, center.y + radius_y};
        tessellate_polyline(output, {first, second}, style, segments);
        return;
    }

    struct RingPoint final {
        Vec2d outer;
        Vec2d inner;
    };
    const auto ring_point = [&](const double angle) {
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        const Vec2d centerline{
            center.x + cosine * radius_x,
            center.y + sine * radius_y,
        };
        // The ellipse gradient gives the true outward normal. Sharing these
        // offset points between adjacent triangles produces one continuous
        // ring instead of disconnected segment quads that crack at joins.
        const double gradient_x = cosine / radius_x;
        const double gradient_y = sine / radius_y;
        const double gradient_length =
            std::hypot(gradient_x, gradient_y);
        const Vec2d normal{
            gradient_x / gradient_length,
            gradient_y / gradient_length,
        };
        return RingPoint{
            {
                centerline.x + normal.x * half_width,
                centerline.y + normal.y * half_width,
            },
            {
                centerline.x - normal.x * half_width,
                centerline.y - normal.y * half_width,
            },
        };
    };
    const bool stroke_closes_center =
        half_width >= std::min(radius_x, radius_y);
    const std::optional<RenderColor> fill = style.fill.has_value()
        ? std::optional<RenderColor>{to_render_color(*style.fill)}
        : std::nullopt;

    std::vector<RingPoint> ring;
    ring.reserve(segments);
    for (std::uint32_t index = 0U; index < segments; ++index) {
        const double angle =
            2.0 * pi * static_cast<double>(index)
            / static_cast<double>(segments);
        ring.push_back(ring_point(angle));
    }

    // Fill first and stop exactly at the outline's inner boundary. Emitting a
    // fill triangle before each individual stroke segment allowed the next
    // fill triangle to overdraw half of the preceding outline at their join.
    if (fill.has_value() && !stroke_closes_center) {
        for (std::size_t index = 0U; index < ring.size(); ++index) {
            const std::size_t next = (index + 1U) % ring.size();
            append_world_triangle(
                output,
                center,
                ring[index].inner,
                ring[next].inner,
                *fill);
        }
    }

    // Submit the complete outline after the fill so no later geometry can
    // carve radial wedges through the stroke.
    for (std::size_t index = 0U; index < ring.size(); ++index) {
        const std::size_t next = (index + 1U) % ring.size();
        if (stroke_closes_center) {
            append_world_triangle(
                output, center, ring[index].outer, ring[next].outer, stroke);
        } else {
            append_world_quad(
                output,
                ring[index].outer,
                ring[next].outer,
                ring[next].inner,
                ring[index].inner,
                stroke);
        }
    }
}

std::uint32_t geometry_detail(
    const ObjectGeometry& geometry,
    const Style& style,
    const double zoom)
{
    if (const auto* const ellipse = std::get_if<Ellipse>(&geometry)) {
        return ellipse_detail(*ellipse, zoom);
    }
    if (const auto* const rectangle =
            std::get_if<RectangleShape>(&geometry);
        rectangle != nullptr && rectangle->roundness > 0.0) {
        const double radius =
            std::clamp(rectangle->roundness, 0.0, 0.5)
            * std::min(
                std::abs(rectangle->second.x - rectangle->first.x),
                std::abs(rectangle->second.y - rectangle->first.y));
        const double screen_radius = radius * zoom;
        return static_cast<std::uint32_t>(
            std::clamp(std::ceil(screen_radius * 0.4), 12.0, 64.0));
    }
    const double screen_width = style.stroke_width * zoom;
    if (screen_width < 1.5) return 8U;
    if (screen_width < 4.0) return 14U;
    return 20U;
}

void tessellate_geometry(
    std::vector<CachedWorldVertex>& output,
    const ObjectGeometry& geometry,
    const Style& style,
    const std::uint32_t detail)
{
    output.clear();
    std::visit(
        [&](const auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, Line>) {
                tessellate_polyline(
                    output, {shape.start, shape.end}, style, detail);
            } else if constexpr (std::is_same_v<Shape, Stroke>) {
                tessellate_polyline(output, shape.points, style, detail);
            } else if constexpr (std::is_same_v<Shape, RectangleShape>) {
                tessellate_rectangle(output, shape, style, detail);
            } else if constexpr (std::is_same_v<Shape, Ellipse>) {
                tessellate_ellipse(output, shape, style, detail);
            }
        },
        geometry);
}

void tessellate_transformed_geometry(
    std::vector<CachedWorldVertex>& output,
    std::vector<Vec2d>& point_scratch,
    const ObjectGeometry& geometry,
    const Style& style,
    const double zoom,
    const SelectionTransform& transform)
{
    output.clear();
    std::visit(
        [&](const auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, Line>) {
                const Line transformed{
                    transform.apply(shape.start),
                    transform.apply(shape.end),
                };
                tessellate_polyline(
                    output,
                    {transformed.start, transformed.end},
                    style,
                    geometry_detail(transformed, style, zoom));
            } else if constexpr (std::is_same_v<Shape, Stroke>) {
                point_scratch.resize(shape.points.size());
                std::transform(
                    shape.points.begin(),
                    shape.points.end(),
                    point_scratch.begin(),
                    [&](const Vec2d point) {
                        return transform.apply(point);
                    });
                decimate_polyline_to_vertex_budget(
                    point_scratch,
                    geometry_detail(geometry, style, zoom),
                    maximum_overlay_content_vertices);
                tessellate_polyline(
                    output,
                    point_scratch,
                    style,
                    geometry_detail(geometry, style, zoom));
            } else if constexpr (
                std::is_same_v<Shape, RectangleShape>) {
                const RectangleShape transformed{
                    transform.apply(shape.first),
                    transform.apply(shape.second),
                    shape.roundness,
                };
                tessellate_rectangle(
                    output,
                    transformed,
                    style,
                    geometry_detail(transformed, style, zoom));
            } else if constexpr (std::is_same_v<Shape, Ellipse>) {
                const Ellipse transformed{
                    transform.apply(shape.first),
                    transform.apply(shape.second),
                };
                tessellate_ellipse(
                    output,
                    transformed,
                    style,
                    geometry_detail(transformed, style, zoom));
            }
        },
        geometry);
}

void tessellate_geometry_bounded(
    std::vector<CachedWorldVertex>& output,
    std::vector<Vec2d>& point_scratch,
    const ObjectGeometry& geometry,
    const Style& style,
    const std::uint32_t detail,
    const std::size_t vertex_budget)
{
    if (const auto* const stroke = std::get_if<Stroke>(&geometry)) {
        point_scratch = stroke->points;
        decimate_polyline_to_vertex_budget(
            point_scratch, detail, vertex_budget);
        output.clear();
        tessellate_polyline(output, point_scratch, style, detail);
        return;
    }
    tessellate_geometry(output, geometry, style, detail);
}

Aabb transformed_object_bounds(
    const Object& object,
    const SelectionTransform& transform) noexcept
{
    const double padding =
        std::max(object.style.stroke_width, 0.0) * 0.5;
    Aabb geometry_bounds{
        object.bounds.min_x + padding,
        object.bounds.min_y + padding,
        object.bounds.max_x - padding,
        object.bounds.max_y - padding,
    };
    Aabb transformed = transform.apply(geometry_bounds);
    transformed.min_x -= padding;
    transformed.min_y -= padding;
    transformed.max_x += padding;
    transformed.max_y += padding;
    return transformed;
}

void append_cached_geometry(
    std::vector<GeometryVertex>& output,
    const std::vector<CachedWorldVertex>& cached,
    const Vec2d camera_position,
    const SelectionTransform* const transform = nullptr)
{
    for (const CachedWorldVertex& vertex : cached) {
        Vec2d position = vertex.position;
        if (transform != nullptr) {
            position = transform->apply(position);
        }
        output.push_back({
            {
                static_cast<float>(
                    position.x - camera_position.x),
                static_cast<float>(
                    position.y - camera_position.y),
            },
            vertex.color,
        });
    }
}

// Draws the board's background pattern (dots, squares, rules, diamonds, ...)
// into the world-space geometry buffer. Line colors are derived from the
// chosen background color's luminance so any swatch stays legible.
[[maybe_unused]] void append_background_pattern(
    std::vector<GeometryVertex>& output,
    const Vec2d camera_position,
    const BackgroundStyle style,
    const RenderColor background,
    const std::optional<Color> custom_grid_color,
    const double min_x,
    const double min_y,
    const double max_x,
    const double max_y,
    const Vec2d viewport,
    const double step,
    const double zoom)
{
    if (style == BackgroundStyle::solid) {
        return;
    }

    const auto grid_plan = plan_background_grid(
        static_cast<BackgroundGridPattern>(style),
        viewport.x,
        viewport.y,
        zoom,
        step);
    const double actual_step = grid_plan.world_spacing;

    const float luminance = 0.299F * background.red
        + 0.587F * background.green + 0.114F * background.blue;
    const RenderColor ink = custom_grid_color.has_value()
        ? to_render_color(*custom_grid_color)
        : (luminance > 0.5F
            ? RenderColor{0.0F, 0.0F, 0.0F}
            : RenderColor{1.0F, 1.0F, 1.0F});
    const float grid_density = static_cast<float>(grid_plan.density);
    const RenderColor minor = mix_color(
        background, ink,
        (custom_grid_color.has_value() ? 0.65F : 0.15F) * grid_density);
    const RenderColor major = mix_color(
        background, ink,
        (custom_grid_color.has_value() ? 0.84F : 0.32F) * grid_density);
    const RenderColor axis = mix_color(
        background, ink,
        (custom_grid_color.has_value() ? 1.0F : 0.48F) * grid_density);
    const RenderColor dots = mix_color(
        background, ink,
        (custom_grid_color.has_value() ? 0.78F : 0.26F) * grid_density);

    // Keep mark sizes in world units even if distant detail is skipped.
    const double w_minor = step * 0.018;
    const double w_major = step * 0.032;
    const double w_axis = step * 0.05;
    constexpr double eps = 1.0e-9;

    const auto vertical_lines = [&](const double gstep, const bool majors) {
        const double first = std::ceil(min_x / gstep) * gstep;
        for (double x = first; x <= max_x + eps; x += gstep) {
            const auto idx = static_cast<std::int64_t>(std::llround(x / gstep));
            const bool is_axis = std::abs(x) < eps;
            const bool is_major = majors && idx % 5 == 0;
            const RenderColor col = is_axis ? axis : (is_major ? major : minor);
            const double w = is_axis ? w_axis : (is_major ? w_major : w_minor);
            append_quad(
                output, x - w * 0.5, min_y, x + w * 0.5, max_y,
                camera_position, col);
        }
    };
    const auto horizontal_lines = [&](const double gstep,
                                      const bool majors,
                                      const bool emphasize_axis) {
        const double first = std::ceil(min_y / gstep) * gstep;
        for (double y = first; y <= max_y + eps; y += gstep) {
            const auto idx = static_cast<std::int64_t>(std::llround(y / gstep));
            const bool is_axis = emphasize_axis && std::abs(y) < eps;
            const bool is_major = majors && idx % 5 == 0;
            const RenderColor col = is_axis ? axis : (is_major ? major : minor);
            const double w = is_axis ? w_axis : (is_major ? w_major : w_minor);
            append_quad(
                output, min_x, y - w * 0.5, max_x, y + w * 0.5,
                camera_position, col);
        }
    };
    const auto dot_lattice = [&](const double gstep) {
        const double first_x = std::ceil(min_x / gstep) * gstep;
        const double first_y = std::ceil(min_y / gstep) * gstep;
        const double r = step * 0.045;
        // Use less geometry for dots that shrink to a few screen pixels.
        const double screen_r = r * zoom;
        const std::uint32_t segments = screen_r < 2.0
            ? 0U
            : (screen_r < 6.0 ? 10U : 18U);
        for (double x = first_x; x <= max_x + eps; x += gstep) {
            for (double y = first_y; y <= max_y + eps; y += gstep) {
                if (segments == 0U) {
                    append_quad(
                        output, x - r, y - r, x + r, y + r,
                        camera_position, dots);
                    continue;
                }
                for (std::uint32_t i = 0U; i < segments; ++i) {
                    const double a = 2.0 * pi * i / segments;
                    const double b = 2.0 * pi * (i + 1U) / segments;
                    append_vertex(output, x, y, camera_position, dots);
                    append_vertex(
                        output, x + std::cos(a) * r, y + std::sin(a) * r,
                        camera_position, dots);
                    append_vertex(
                        output, x + std::cos(b) * r, y + std::sin(b) * r,
                        camera_position, dots);
                }
            }
        }
    };
    const auto thick_segment = [&](Vec2d a, Vec2d b,
                                   const double width, const RenderColor col) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length = std::hypot(dx, dy);
        if (length < 1.0e-12) return;
        const double nx = -dy / length * width * 0.5;
        const double ny = dx / length * width * 0.5;
        append_vertex(output, a.x + nx, a.y + ny, camera_position, col);
        append_vertex(output, b.x + nx, b.y + ny, camera_position, col);
        append_vertex(output, b.x - nx, b.y - ny, camera_position, col);
        append_vertex(output, a.x + nx, a.y + ny, camera_position, col);
        append_vertex(output, b.x - nx, b.y - ny, camera_position, col);
        append_vertex(output, a.x - nx, a.y - ny, camera_position, col);
    };
    // Liang-Barsky clip of segment a-b to the visible box.
    const auto clip_segment = [&](Vec2d& a, Vec2d& b) -> bool {
        double t0 = 0.0;
        double t1 = 1.0;
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const auto edge = [&](const double p, const double q) -> bool {
            if (std::abs(p) < 1.0e-12) return q >= 0.0;
            const double r = q / p;
            if (p < 0.0) {
                if (r > t1) return false;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return false;
                if (r < t1) t1 = r;
            }
            return true;
        };
        if (edge(-dx, a.x - min_x) && edge(dx, max_x - a.x)
            && edge(-dy, a.y - min_y) && edge(dy, max_y - a.y)) {
            const Vec2d na{a.x + t0 * dx, a.y + t0 * dy};
            const Vec2d nb{a.x + t1 * dx, a.y + t1 * dy};
            a = na;
            b = nb;
            return t1 > t0;
        }
        return false;
    };
    const auto diagonal_family = [&](const double slope, const double spacing) {
        const std::array<double, 4> corners{{
            min_y - slope * min_x,
            min_y - slope * max_x,
            max_y - slope * min_x,
            max_y - slope * max_x,
        }};
        const double c_lo = *std::min_element(corners.begin(), corners.end());
        const double c_hi = *std::max_element(corners.begin(), corners.end());
        const double first = std::ceil(c_lo / spacing) * spacing;
        for (double c = first; c <= c_hi + eps; c += spacing) {
            Vec2d a{min_x, slope * min_x + c};
            Vec2d b{max_x, slope * max_x + c};
            if (clip_segment(a, b)) {
                thick_segment(a, b, w_minor, minor);
            }
        }
    };

    switch (style) {
    case BackgroundStyle::dot:
        dot_lattice(actual_step);
        break;
    case BackgroundStyle::square:
        vertical_lines(actual_step, true);
        horizontal_lines(actual_step, true, true);
        break;
    case BackgroundStyle::graph:
        vertical_lines(actual_step / 5.0, true);
        horizontal_lines(actual_step / 5.0, true, true);
        break;
    case BackgroundStyle::hybrid:
        vertical_lines(actual_step, false);
        horizontal_lines(actual_step, false, true);
        dot_lattice(actual_step);
        break;
    case BackgroundStyle::diamond:
        diagonal_family(1.0, actual_step);
        diagonal_family(-1.0, actual_step);
        break;
    case BackgroundStyle::wide_rule:
        horizontal_lines(actual_step, false, false);
        break;
    case BackgroundStyle::narrow_rule:
        horizontal_lines(actual_step * 0.5, false, false);
        break;
    case BackgroundStyle::triangle:
        horizontal_lines(actual_step, false, true);
        diagonal_family(1.7320508, actual_step * 2.0);
        diagonal_family(-1.7320508, actual_step * 2.0);
        break;
    case BackgroundStyle::solid:
    case BackgroundStyle::count:
        break;
    }
}

// Left square reserved for a button's icon, shared by geometry and text so the
// label starts clear of the icon.
double home_button_icon_size(const double scale) noexcept
{
    return 20.0 * scale;
}

double home_button_label_x(const UiRect& bounds, const double scale) noexcept
{
    return bounds.x + 12.0 * scale + home_button_icon_size(scale) + 8.0 * scale;
}

void append_home_geometry(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans,
    const Camera& camera,
    const HomeView& home)
{
    const bool light = home.theme() == Theme::light;
    const bool previous_light = home.previous_theme() == Theme::light;
    const double theme_amount = smooth_theme_transition(home);
    const double scale = home.scale();
    const double reveal = home.reveal();
    const InterfacePalette palette = home_palette(home);
    const RenderColor backdrop = palette.background;
    const RenderColor card_surface = palette.surface;
    const RenderColor preview_surface = palette.preview_surface;
    const RenderColor card_border = palette.border;
    const RenderColor hovered = palette.hover_surface;
    const RenderColor accent = palette.primary;
    const RenderColor accent_hover = palette.primary_hover;
    const RenderColor text = palette.text;
    const RenderColor card_shadow = mix_color(
        previous_light ? rgb(0x273244U) : rgb(0x000000U),
        light ? rgb(0x273244U) : rgb(0x000000U), theme_amount);
    const RenderColor danger = mix_color(
        previous_light
            ? RenderColor{0.72F, 0.18F, 0.24F}
            : RenderColor{1.0F, 0.48F, 0.54F},
        light
            ? RenderColor{0.72F, 0.18F, 0.24F}
            : RenderColor{1.0F, 0.48F, 0.54F},
        theme_amount);

    // A single neutral plane keeps the screen flat and platform-neutral.
    append_screen_quad(
        output,
        {0.0, 0.0, home.viewport_width(), home.viewport_height()},
        camera, backdrop);
    const double panel_shift = (1.0 - reveal) * 16.0 * scale;
    const double header_shift = -(1.0 - reveal) * 8.0 * scale;

    // The page title and file actions share one flat header row.
    if (home.header_bounds().width > 1.0) {
        UiRect bounds = home.header_bounds();
        bounds.y += header_shift;
        append_screen_quad(
            output,
            {
                bounds.x,
                bounds.y + bounds.height - std::max(scale, 1.0),
                bounds.width,
                std::max(scale, 1.0),
            },
            camera, card_border);
    }

    if (home.status_bounds().width > 1.0) {
        const bool is_error = !home.error_message().empty();
        UiRect bounds = home.status_bounds();
        bounds.y += panel_shift;
        append_screen_rounded_rect(
            output, bounds, 7.0 * scale, camera,
            is_error
                ? mix_color(
                    card_border,
                    danger,
                    (previous_light ? 0.48 : 0.62)
                        + ((light ? 0.48 : 0.62)
                            - (previous_light ? 0.48 : 0.62))
                            * theme_amount)
                : mix_color(card_border, accent, 0.42));
        UiRect inside = bounds;
        inside.x += 1.0 * scale;
        inside.y += 1.0 * scale;
        inside.width -= 2.0 * scale;
        inside.height -= 2.0 * scale;
        append_screen_rounded_rect(
            output, inside, 6.0 * scale, camera,
            is_error
                ? mix_color(
                    previous_light
                        ? RenderColor{1.0F, 0.955F, 0.958F}
                        : RenderColor{0.17F, 0.075F, 0.085F},
                    light
                        ? RenderColor{1.0F, 0.955F, 0.958F}
                        : RenderColor{0.17F, 0.075F, 0.085F},
                    theme_amount)
                : palette.accent_soft);

        const Vec2d center{
            bounds.x + 21.0 * scale,
            bounds.y + bounds.height * 0.5,
        };
        append_screen_circle(
            output, center, 11.0 * scale, camera,
            is_error ? danger : accent, 24U);
        UiControl info{};
        info.icon = UiIcon::info;
        append_icon(
            output, info,
            {
                center.x - 7.0 * scale,
                center.y - 7.0 * scale,
                14.0 * scale,
                14.0 * scale,
            },
            camera, RenderColor{1.0F, 1.0F, 1.0F}, scale);
    }

    // The only additional surface is the empty recent-files state.
    for (const auto& item : home.panels()) {
        UiRect bounds = item.bounds;
        bounds.y += panel_shift;
        append_screen_rounded_rect(
            output, bounds, 8.0 * scale, camera, card_border);
        UiRect inside = bounds;
        inside.x += 1.0 * scale;
        inside.y += 1.0 * scale;
        inside.width -= 2.0 * scale;
        inside.height -= 2.0 * scale;
        append_screen_rounded_rect(
            output, inside, 7.0 * scale, camera, card_surface);
    }

    if (home.boards().empty() && !home.panels().empty()) {
        const UiRect empty = home.panels().back().bounds;
        const Vec2d center{
            empty.x + empty.width * 0.5,
            empty.y + 38.0 * scale + panel_shift,
        };
        append_screen_circle(
            output, center, 18.0 * scale, camera,
            palette.accent_soft,
            28U);
        UiControl blank{};
        blank.icon = UiIcon::file_new;
        append_icon(
            output, blank,
            {center.x - 10.0 * scale, center.y - 10.0 * scale,
             20.0 * scale, 20.0 * scale},
            camera, accent, scale);
    }

    const auto& controls = home.controls();
    const auto& boards = home.boards();
    const UiControl* const focused_control = home.focused_control();
    const double vertical_margin = 20.0 * scale;

    for (std::size_t index = 0U; index < controls.size(); ++index) {
        const UiControl& control = controls[index];
        const UiAnimation& animation = home.animation(index);
        const double entrance = home.entrance(index);
        if (entrance <= 0.01) {
            continue;
        }
        const bool board_card = index < boards.size();
        const bool board_focused = board_card
            && focused_control == &control;
        const double hover = animation.hover;
        const double raw_board_emphasis = board_card
            ? std::max(
                hover,
                (board_focused || boards[index].editing) ? 1.0 : 0.0)
            : 0.0;
        const double board_emphasis = raw_board_emphasis
            * raw_board_emphasis * (3.0 - 2.0 * raw_board_emphasis);
        UiRect bounds = control.bounds;
        bounds.y += home.control_offset(index);
        if (board_card
            && (control.bounds.y + control.bounds.height <= home.grid_top()
                || bounds.y > home.viewport_height() + vertical_margin)) {
            continue;
        }
        const double corner = board_card ? 10.0 * scale : 8.0 * scale;
        const UiRect surface_bounds = bounds;
        if (focused_control == &control && !board_card) {
            const UiRect focus_bounds{
                bounds.x - 2.0 * scale,
                bounds.y - 2.0 * scale,
                bounds.width + 4.0 * scale,
                bounds.height + 4.0 * scale,
            };
            append_screen_rounded_rect(
                output, focus_bounds, corner + 2.0 * scale,
                camera, accent);
        }

        // Only the shadow changes elevation; card content and hit regions stay
        // fixed throughout hover and press feedback.
        if (board_card) {
            const double idle_strength = previous_light ? 0.48 : 0.9;
            const double strength = idle_strength
                + ((light ? 0.48 : 0.9) - idle_strength) * theme_amount;
            append_soft_shadow(
                output, spans, surface_bounds, corner, camera, card_shadow,
                scale,
                (strength + 0.62 * board_emphasis)
                    * (1.0 - 0.42 * animation.press) * entrance,
                0.85 + 0.45 * board_emphasis - 0.35 * animation.press);
        }

        const auto entering = [&](const RenderColor color) {
            return mix_color(
                backdrop, color, 0.3 + 0.7 * entrance);
        };

        const RenderColor card_accent =
            (board_focused || (board_card && boards[index].editing))
            ? palette.focus : mix_color(accent, accent_hover, animation.press);
        const RenderColor border = board_card
            ? entering((board_focused || boards[index].editing) ? card_accent
                : mix_color(card_border, card_accent, board_emphasis * 0.88))
            : control.selected
                ? entering(accent)
                : entering(mix_color(card_border, accent, hover * 0.38));
        append_screen_rounded_rect(
            output, surface_bounds, corner, camera, border);

        // Card content always starts at the same position. A half-strength
        // second neutral pixel gives the resting edge an optical 1.5-pixel
        // weight. Hover builds a cobalt outline; focus and editing give it
        // full strength so keyboard navigation remains distinct.
        if (board_card) {
            for (int inset = 1; inset <= 2; ++inset) {
                const double amount = static_cast<double>(inset) * scale;
                UiRect quiet_edge = surface_bounds;
                quiet_edge.x += amount;
                quiet_edge.y += amount;
                quiet_edge.width -= amount * 2.0;
                quiet_edge.height -= amount * 2.0;
                append_screen_rounded_rect(
                    output, quiet_edge,
                    std::max(corner - amount, 0.0),
                    camera,
                    entering(mix_color(
                        inset == 1
                            ? mix_color(card_border, card_surface, 0.5)
                            : card_surface,
                        (board_focused || boards[index].editing) ? card_accent
                            : mix_color(card_border, card_accent, 0.88),
                        board_emphasis)));
            }
        }
        const double content_inset = (board_card ? 3.0 : 1.0) * scale;
        UiRect inner = surface_bounds;
        inner.x += content_inset;
        inner.y += content_inset;
        inner.width -= content_inset * 2.0;
        inner.height -= content_inset * 2.0;
        const double inner_corner = std::max(corner - content_inset, 0.0);
        if (control.selected && !board_card) {
            const double active_state = std::clamp(
                hover * 0.72 + animation.press * 0.62, 0.0, 1.0);
            const RenderColor fill =
                mix_color(accent, accent_hover, active_state);
            append_screen_rounded_rect(
                output, inner, inner_corner, camera,
                entering(fill));
        } else {
            const RenderColor idle_surface = card_surface;
            RenderColor fill =
                mix_color(
                    idle_surface,
                    hovered,
                    board_card
                        ? 0.22 * board_emphasis
                        : hover * 0.72);
            fill = mix_color(
                fill, hovered, animation.press * 0.42);
            append_screen_rounded_rect(
                output, inner, inner_corner, camera,
                entering(fill));
        }

        if (board_card) {
            UiRect preview = inner;
            preview.height = std::max(
                preview.height - HomeView::card_text_area * scale,
                1.0);
            append_screen_rounded_rect(
                output, preview, inner_corner, camera,
                entering(mix_color(
                    preview_surface,
                    hovered,
                    0.18 * board_emphasis)));

            append_screen_quad(
                output,
                {
                    inner.x,
                    preview.y + preview.height - std::max(scale, 1.0),
                    inner.width,
                    std::max(scale, 1.0),
                },
                camera,
                entering(mix_color(
                    card_surface,
                    card_border,
                    0.66 + 0.10 * board_emphasis)));

            const RenderColor dot_color = entering(mix_color(
                preview_surface, palette.preview_dots, 0.46));
            const double dot_step = 18.0 * scale;
            const std::size_t dot_columns = std::max<std::size_t>(
                1U, static_cast<std::size_t>(
                    std::floor(preview.width / dot_step)));
            const std::size_t dot_rows = std::max<std::size_t>(
                1U, static_cast<std::size_t>(
                    std::floor(preview.height / dot_step)));
            const double dot_left = preview.x
                + (preview.width
                    - static_cast<double>(dot_columns - 1U) * dot_step) * 0.5;
            const double dot_top = preview.y
                + (preview.height
                    - static_cast<double>(dot_rows - 1U) * dot_step) * 0.5;
            for (std::size_t row = 0U; row < dot_rows; ++row) {
                for (std::size_t column = 0U; column < dot_columns; ++column) {
                    append_screen_circle(
                        output,
                        {
                            dot_left
                                + static_cast<double>(column) * dot_step,
                            dot_top + static_cast<double>(row) * dot_step,
                        },
                        std::max(0.72 * scale, 0.72),
                        camera, dot_color, 10U);
                }
            }

            if (boards[index].editing) {
                UiRect field = home.board_name_bounds(index);
                field.x -= 6.0 * scale;
                field.y -= 4.0 * scale;
                field.width += 12.0 * scale;
                field.height = 32.0 * scale;
                append_screen_rounded_rect(
                    output, field, 6.0 * scale, camera,
                    entering(accent));
                field.x += 1.0 * scale;
                field.y += 1.0 * scale;
                field.width -= 2.0 * scale;
                field.height -= 2.0 * scale;
                append_screen_rounded_rect(
                    output, field, 5.0 * scale, camera,
                    entering(card_surface));
            }
        }

        if (board_card
            && (boards[index].preview == nullptr
                || !boards[index].preview->has_content)) {
            const double text_area = HomeView::card_text_area * scale;
            UiControl blank{};
            blank.icon = UiIcon::pencil;
            const double tile_size = 38.0 * scale;
            const double icon_size = 20.0 * scale;
            const Vec2d center{
                bounds.x + bounds.width * 0.5,
                bounds.y + (bounds.height - text_area) * 0.5,
            };
            append_screen_circle(
                output, center, tile_size * 0.5, camera,
                entering(palette.accent_soft),
                28U);
            const UiRect icon_bounds{
                center.x - icon_size * 0.5,
                center.y - icon_size * 0.5,
                icon_size,
                icon_size,
            };
            append_icon(
                output, blank, icon_bounds, camera,
                entering(palette.muted),
                scale);
        }

        if (control.icon != UiIcon::none) {
            const RenderColor icon_color = control.selected && !board_card
                ? palette.on_primary
                : entering(text);
            const double size = home_button_icon_size(scale);
            const double icon_x = control.label.empty()
                ? inner.x + (inner.width - size) * 0.5
                : inner.x + 12.0 * scale;
            const UiRect icon_bounds{
                icon_x,
                inner.y + (inner.height - size) * 0.5,
                size,
                size,
            };
            append_icon(output, control, icon_bounds, camera, icon_color, scale);
        }
    }

    // Per-card rename chips, drawn on top of their cards.
    UiControl pencil{};
    pencil.icon = UiIcon::pencil;
    for (std::size_t index = 0U;
         index < home.rename_buttons().size(); ++index) {
        if (home.entrance(index) <= 0.01) {
            continue;
        }
        UiRect chip = home.rename_buttons()[index];
        chip.y += home.control_offset(index);
        if (home.rename_buttons()[index].y
                + home.rename_buttons()[index].height <= home.grid_top()
            || chip.y + chip.height < -vertical_margin
            || chip.y > home.viewport_height() + vertical_margin) {
            continue;
        }
        const double hover = home.rename_hover(index);
        const bool card_focused =
            index < controls.size() && focused_control == &controls[index];
        const double raw_visibility = std::max(
            home.animation(index).hover,
            card_focused ? 1.0 : 0.0);
        const double visibility = raw_visibility * raw_visibility
            * (3.0 - 2.0 * raw_visibility);
        if (visibility <= 0.004 || boards[index].editing) {
            continue;
        }
        set_geometry_alpha(spans, output, visibility);
        // Use one surface layer for the fading control. Stacking an outer
        // rounded rectangle under an inner one made their opacity accumulate,
        // so the center appeared sooner than the border and glyph.
        const RenderColor chip_surface = mix_color(
            mix_color(
                card_surface,
                card_border,
                light ? 0.20 : 0.30),
            hovered,
            hover * 0.72);
        append_screen_rounded_rect(
            output, chip, 6.0 * scale, camera,
            chip_surface);
        const double icon_size = 18.0 * scale;
        const UiRect icon_bounds{
            chip.x + (chip.width - icon_size) * 0.5,
            chip.y + (chip.height - icon_size) * 0.5,
            icon_size,
            icon_size,
        };
        append_icon(
            output, pencil, icon_bounds, camera,
            mix_color(palette.muted, text, hover), scale);
        set_geometry_alpha(spans, output, 1.0);
    }

    const UiRect scroll_track = home.scrollbar_track();
    const UiRect scroll_thumb = home.scrollbar_thumb();
    if (scroll_track.height > 1.0 && scroll_thumb.height > 1.0) {
        set_geometry_alpha(
            spans,
            output,
            (previous_light ? 0.38 : 0.46)
                + ((light ? 0.38 : 0.46)
                    - (previous_light ? 0.38 : 0.46))
                    * theme_amount);
        append_screen_rounded_rect(
            output, scroll_track, scroll_track.width * 0.5,
            camera, card_border);
        set_geometry_alpha(
            spans,
            output,
            (previous_light ? 0.72 : 0.82)
                + ((light ? 0.72 : 0.82)
                    - (previous_light ? 0.72 : 0.82))
                    * theme_amount);
        append_screen_rounded_rect(
            output, scroll_thumb, scroll_thumb.width * 0.5,
            camera, mix_color(card_border, text, 0.32));
        set_geometry_alpha(spans, output, 1.0);
    }
}

[[nodiscard]] bool is_canvas_pattern(const UiAction action) noexcept
{
    return action >= UiAction::grid_solid && action <= UiAction::grid_narrow_rule;
}

void append_toolbar_geometry(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans,
    const Camera& camera,
    const Toolbar& toolbar,
    const double tooltip_text_width,
    const double tooltip_text_height,
    const bool tooltip_allowed,
    std::optional<std::uint32_t>& tooltip_first_vertex)
{
    const bool light = toolbar.theme() == Theme::light;
    const bool previous_light =
        toolbar.previous_theme() == Theme::light;
    const double theme_amount = smooth_theme_transition(toolbar);
    const double scale = toolbar.scale();
    const double reveal = toolbar.reveal();
    const double settings_reveal = toolbar.settings_reveal();
    // Keep chrome opaque behind the picker; translucent panels let board
    // strokes show through controls and make their state hard to read.
    constexpr double background_chrome_alpha = 1.0;
    const InterfacePalette palette = toolbar_palette(toolbar);
    const auto theme_value =
        [light, previous_light, theme_amount](
            const double light_value,
            const double dark_value) {
            const double previous =
                previous_light ? light_value : dark_value;
            const double current = light ? light_value : dark_value;
            return previous + (current - previous) * theme_amount;
        };
    const RenderColor panel_top = palette.surface;
    const RenderColor panel_bottom = palette.surface;
    const RenderColor panel = palette.surface;
    const RenderColor button = palette.hover_surface;
    const RenderColor top_bar_button = palette.control_surface;
    const RenderColor hovered = palette.accent_soft;
    const RenderColor accent = palette.primary;
    const RenderColor disabled =
        mix_color(palette.surface, palette.background, 0.55);
    const RenderColor text = palette.text;
    const RenderColor muted = palette.muted;
    const RenderColor shadow = mix_color(
        previous_light
            ? RenderColor{0.09F, 0.13F, 0.26F}
            : RenderColor{0.0F, 0.0F, 0.0F},
        light
            ? RenderColor{0.09F, 0.13F, 0.26F}
            : RenderColor{0.0F, 0.0F, 0.0F},
        theme_amount);
    const RenderColor panel_border = palette.border;
    const RenderColor divider = palette.border;
    const RenderColor danger = mix_color(
        previous_light
            ? RenderColor{0.72F, 0.18F, 0.24F}
            : RenderColor{1.0F, 0.48F, 0.54F},
        light
            ? RenderColor{0.72F, 0.18F, 0.24F}
            : RenderColor{1.0F, 0.48F, 0.54F},
        theme_amount);
    const RenderColor error_border = mix_color(
        panel_border, danger, theme_value(0.42, 0.52));
    const RenderColor error_surface = mix_color(
        panel, danger, theme_value(0.06, 0.13));

    // Fade chrome in place so animated surfaces and their hit regions agree.
    const auto slide = [](double) { return 0.0; };
    const auto shifted = [&](UiRect bounds) {
        bounds.y += slide(bounds.y);
        return bounds;
    };

    if (toolbar.settings_scrim_visible() && settings_reveal > 0.01) {
        // Dim the canvas behind the settings flyout to focus attention.
        set_geometry_alpha(
            spans, output,
            theme_value(0.10, 0.26) * settings_reveal);
        append_screen_quad(
            output,
            {0.0, 0.0, toolbar.viewport_width(), toolbar.viewport_height()},
            camera,
            {0.03F, 0.05F, 0.09F});
        set_geometry_alpha(spans, output, 1.0);
    }

    // The settings flyout is inserted immediately before the status pill; it
    // fades and rises with settings_reveal while the fixed chrome does not.
    const std::size_t settings_panel_index =
        toolbar.settings_open() && toolbar.panels().size() >= 2U
            ? toolbar.panels().size() - 2U
            : toolbar.panels().size();
    std::optional<UiRect> modal_bounds;
    if (toolbar.settings_open()
        && settings_panel_index < toolbar.panels().size()) {
        modal_bounds = shifted(
            toolbar.panels()[settings_panel_index].bounds);
    }
    const auto occluded_by_modal = [&](const UiRect bounds) {
        return modal_bounds.has_value()
            && bounds.x < modal_bounds->x + modal_bounds->width
            && bounds.x + bounds.width > modal_bounds->x
            && bounds.y < modal_bounds->y + modal_bounds->height
            && bounds.y + bounds.height > modal_bounds->y;
    };

    for (std::size_t index = 0U; index < toolbar.panels().size(); ++index) {
        const bool application_bar = index == 0U;
        const bool status_panel = index + 1U == toolbar.panels().size();
        const bool error_panel = toolbar.error_bounds().width > 1.0
            && toolbar.panels()[index].bounds == toolbar.error_bounds();
        const bool settings_panel = index == settings_panel_index;
        const double panel_alpha = settings_panel
            ? settings_reveal
            : background_chrome_alpha;
        if (panel_alpha <= 0.01) {
            continue;
        }
        UiRect bounds = shifted(toolbar.panels()[index].bounds);
        if (application_bar) {
            set_geometry_alpha(spans, output, panel_alpha);
            const UiRect title = toolbar.filename_bounds();
            const UiControl* rename_button = toolbar.find(UiAction::rename_button);
            const double document_right = rename_button != nullptr
                ? rename_button->bounds.x + rename_button->bounds.width
                : title.x + title.width;
            const UiControl* utility = toolbar.find(UiAction::toggle_theme);
            const double island_y = std::max(0.0, (toolbar.height() - 56.0 * scale) * 0.5);
            const UiRect document_island{8.0 * scale, island_y,
                document_right, 56.0 * scale};
            const UiRect utility_island{utility->bounds.x - 8.0 * scale, island_y,
                toolbar.viewport_width() - utility->bounds.x, 56.0 * scale};
            for (const UiRect island : {document_island, utility_island}) {
                append_screen_rounded_rect(output, island, 12.0 * scale, camera, panel_border);
                append_screen_rounded_rect(output,
                    {island.x + scale, island.y + scale, island.width - 2.0 * scale, island.height - 2.0 * scale},
                    11.0 * scale, camera, panel);
            }
            if (const UiControl* file = toolbar.find(UiAction::file_menu)) {
                const double divider_x = file->bounds.x + file->bounds.width + 8.0 * scale;
                append_screen_line(output,
                    {divider_x, file->bounds.y + 8.0 * scale},
                    {divider_x, file->bounds.y + file->bounds.height - 8.0 * scale},
                    std::max(1.0, scale), camera, panel_border);
            }
            set_geometry_alpha(spans, output, 1.0);
            continue;
        }
        if (status_panel) {
            // The filename reads as document context in the flat bar, not as a
            // separate floating pill. Its rename control still provides hover
            // feedback and the whole bounds remain an input exclusion region.
            continue;
        }
        if (error_panel) {
            append_soft_shadow(
                output, spans, bounds, 8.0 * scale, camera, shadow,
                scale,
                theme_value(0.18, 0.34) * reveal * panel_alpha,
                0.8);
            set_geometry_alpha(spans, output, panel_alpha);
            append_screen_rounded_rect(
                output, bounds, 8.0 * scale, camera, error_border);
            UiRect inner = bounds;
            inner.x += 1.0 * scale;
            inner.y += 1.0 * scale;
            inner.width -= 2.0 * scale;
            inner.height -= 2.0 * scale;
            append_screen_rounded_rect(
                output, inner, 7.0 * scale, camera, error_surface);

            const Vec2d alert_center{
                bounds.x + 18.0 * scale,
                bounds.y + bounds.height * 0.5,
            };
            append_screen_circle(
                output, alert_center, 8.0 * scale, camera, danger, 24U);
            const RenderColor alert_mark{1.0F, 1.0F, 1.0F};
            append_screen_line(
                output,
                {alert_center.x, alert_center.y - 3.8 * scale},
                {alert_center.x, alert_center.y + 1.0 * scale},
                1.5 * scale,
                camera,
                alert_mark);
            append_screen_circle(
                output,
                {alert_center.x, alert_center.y + 4.0 * scale},
                1.0 * scale,
                camera,
                alert_mark,
                12U);
            set_geometry_alpha(spans, output, 1.0);
            continue;
        }
        if (settings_panel) {
            append_soft_shadow(
                output, spans, bounds, 10.0 * scale, camera, shadow, scale,
                theme_value(0.52, 0.78) * reveal * panel_alpha,
                1.15);
        }
        set_geometry_alpha(spans, output, panel_alpha);
        const double panel_corner = 12.0 * scale;
        append_screen_rounded_rect(
            output, bounds, panel_corner, camera, panel_border);
        UiRect inner = bounds;
        inner.x += 1.0 * scale;
        inner.y += 1.0 * scale;
        inner.width -= 2.0 * scale;
        inner.height -= 2.0 * scale;
        append_screen_rounded_rect_gradient(
            output, inner, panel_corner - 1.0 * scale,
            camera, panel_top, panel_bottom);
        set_geometry_alpha(spans, output, 1.0);
    }

    set_geometry_alpha(spans, output, background_chrome_alpha * toolbar.properties_reveal());
    const UiRect property_clip = toolbar.properties_clip();
    // Preserve focus outlines at every edge without expanding the input area.
    const UiRect property_render_clip = toolbar.properties_render_clip();
    const std::optional<std::array<double, 4>> property_scissor =
        property_clip.width > 0.0
            ? std::optional<std::array<double, 4>>{{property_render_clip.x,
                property_render_clip.y, property_render_clip.width, property_render_clip.height}}
            : std::nullopt;
    set_geometry_clip(spans, output, property_scissor);
    for (const auto& divider_line : toolbar.dividers()) {
        const Vec2d first{
            divider_line.first.x,
            divider_line.first.y + slide(divider_line.first.y),
        };
        const Vec2d second{
            divider_line.second.x,
            divider_line.second.y + slide(divider_line.second.y),
        };
        const UiRect divider_bounds{
            std::min(first.x, second.x),
            std::min(first.y, second.y),
            std::max(std::abs(second.x - first.x), scale),
            std::max(std::abs(second.y - first.y), scale),
        };
        if (occluded_by_modal(divider_bounds)) {
            continue;
        }
        append_screen_line(
            output,
            first,
            second,
            std::max(1.0, scale),
            camera,
            divider);
    }
    set_geometry_clip(spans, output, std::nullopt);
    set_geometry_alpha(spans, output, 1.0);

    // Gaps larger than the normal control spacing mark semantic groups.
    // A quiet divider makes file, tool, style, and history groups legible
    // without putting every idle icon in its own visible tile.
    const UiControl* previous = nullptr;
    set_geometry_alpha(spans, output, background_chrome_alpha);
    for (const auto& control : toolbar.controls()) {
        const double separation = previous == nullptr
            ? 0.0
            : control.bounds.x
                - (previous->bounds.x + previous->bounds.width);
        if (previous != nullptr
            && control.bounds.y < toolbar.height()
            && std::abs(previous->bounds.y - control.bounds.y) < 0.5
            && separation > 8.0 * scale
            && separation <= 28.0 * scale) {
            const UiRect bounds = shifted(control.bounds);
            if (occluded_by_modal(bounds)) {
                previous = &control;
                continue;
            }
            const double x = (previous->bounds.x + previous->bounds.width
                + control.bounds.x) * 0.5;
            const double center_y = bounds.y + bounds.height * 0.5;
            append_screen_line(
                output,
                {x, center_y - 10.0 * scale},
                {x, center_y + 10.0 * scale},
                1.0 * scale,
                camera,
                divider);
        }
        previous = &control;
    }
    set_geometry_alpha(spans, output, 1.0);

    const UiControl* const focused_control = toolbar.focused_control();
    for (const auto& control : toolbar.controls()) {
        const bool in_settings = is_settings_control(toolbar, &control);
        set_geometry_clip(spans, output,
            toolbar.is_property_control(control.action) ? property_scissor : std::nullopt);
        if (!in_settings
            && occluded_by_modal(shifted(control.bounds))) {
            continue;
        }
        const double control_alpha = in_settings
            ? settings_reveal
            : background_chrome_alpha
                * (toolbar.is_property_control(control.action) ? toolbar.properties_reveal() : 1.0);
        if (control_alpha <= 0.01) {
            continue;
        }
        const double settings_rise = 0.0;
        if (control.action == UiAction::custom_hue_field) {
            UiRect field = shifted(control.bounds);
            field.y += settings_rise;
            set_geometry_alpha(spans, output, control_alpha);
            append_screen_rounded_rect(
                output, field, 7.0 * scale, camera, palette.control_surface);
            const UiRect track = toolbar.color_field_bounds(control.action);
            append_screen_rounded_rect(output,
                {track.x - scale, track.y - scale,
                    track.width + scale * 2.0, track.height + scale * 2.0},
                3.0 * scale, camera, panel_border);
            // HSV hue is exactly piecewise linear across its six sectors.
            // Interpolating the sector endpoints removes the visible 36-band
            // approximation without increasing the geometry budget.
            constexpr std::uint32_t hue_sectors = 6U;
            const double width = track.width;
            for (std::uint32_t sector = 0U;
                 sector < hue_sectors;
                 ++sector) {
                const double first = static_cast<double>(sector)
                    / static_cast<double>(hue_sectors);
                const double second = static_cast<double>(sector + 1U)
                    / static_cast<double>(hue_sectors);
                append_screen_quad_gradient_horizontal(
                    output,
                    {track.x + width * first,
                     track.y,
                     width * (second - first),
                     track.height},
                    camera,
                    hsv_render_color(first, 1.0, 1.0),
                    hsv_render_color(second, 1.0, 1.0));
            }
            const Vec2d marker{
                track.x + width * toolbar.custom_hue(),
                field.y + field.height * 0.5,
            };
            append_screen_rounded_rect(output,
                {marker.x - 5.0 * scale, marker.y - 13.0 * scale,
                    10.0 * scale, 26.0 * scale},
                3.0 * scale, camera, {0.12F, 0.14F, 0.18F});
            append_screen_rounded_rect(output,
                {marker.x - 4.0 * scale, marker.y - 12.0 * scale,
                    8.0 * scale, 24.0 * scale},
                2.0 * scale, camera, {1.0F, 1.0F, 1.0F});
            set_geometry_alpha(spans, output, 1.0);
            continue;
        }
        if (control.action == UiAction::custom_sv_field) {
            UiRect field = shifted(control.bounds);
            field.y += settings_rise;
            set_geometry_alpha(spans, output, control_alpha);
            append_screen_rounded_rect(
                output, field, 8.0 * scale, camera, panel_border);
            // Each cell shares exact HSV corner colors with its neighbours.
            // Per-vertex interpolation makes the surface continuous; the
            // modest grid only approximates HSV's saturation/value cross-term.
            constexpr std::uint32_t columns = 16U;
            constexpr std::uint32_t rows = 12U;
            const UiRect track = toolbar.color_field_bounds(control.action);
            const double width = track.width;
            const double height = track.height;
            for (std::uint32_t row = 0U; row < rows; ++row) {
                for (std::uint32_t column = 0U; column < columns; ++column) {
                    const double saturation_left =
                        static_cast<double>(column)
                        / static_cast<double>(columns);
                    const double saturation_right =
                        static_cast<double>(column + 1U)
                        / static_cast<double>(columns);
                    const double value_top = 1.0
                        - static_cast<double>(row)
                            / static_cast<double>(rows);
                    const double value_bottom = 1.0
                        - static_cast<double>(row + 1U)
                            / static_cast<double>(rows);
                    append_screen_gradient_cell(
                        output,
                        {track.x
                                + width * static_cast<double>(column)
                                    / static_cast<double>(columns),
                         track.y
                                + height * static_cast<double>(row)
                                    / static_cast<double>(rows),
                         width / static_cast<double>(columns),
                         height / static_cast<double>(rows)},
                        camera,
                        hsv_render_color(
                            toolbar.custom_hue(),
                            saturation_left,
                            value_top),
                        hsv_render_color(
                            toolbar.custom_hue(),
                            saturation_right,
                            value_top),
                        hsv_render_color(
                            toolbar.custom_hue(),
                            saturation_right,
                            value_bottom),
                        hsv_render_color(
                            toolbar.custom_hue(),
                            saturation_left,
                            value_bottom));
                }
            }
            const Vec2d marker{
                track.x + width * toolbar.custom_saturation(),
                track.y + height * (1.0 - toolbar.custom_value()),
            };
            append_screen_circle(
                output, marker, 7.0 * scale, camera,
                {0.07F, 0.08F, 0.11F}, 24U);
            append_screen_circle(
                output, marker, 5.7 * scale, camera,
                {1.0F, 1.0F, 1.0F}, 20U);
            append_screen_circle(output, marker, 4.0 * scale, camera,
                to_render_color(toolbar.custom_color()), 20U);
            set_geometry_alpha(spans, output, 1.0);
            continue;
        }
        if (control.action == UiAction::custom_hex_field) {
            const UiRect field = control.bounds;
            const RenderColor border = !toolbar.hex_valid() ? danger
                : (toolbar.hex_editing() || focused_control == &control ? palette.focus : panel_border);
            append_screen_rounded_rect(output, field, 8.0 * scale, camera, border);
            append_screen_rounded_rect(output,
                {field.x + scale, field.y + scale, field.width - 2.0 * scale, field.height - 2.0 * scale},
                7.0 * scale, camera, toolbar.hex_selected() ? palette.accent_soft : panel);
            continue;
        }
        const UiAnimation& animation = toolbar.animation(control.action);
        const bool information = control.icon == UiIcon::info;
        const bool swatch_control = control.icon == UiIcon::none
            && control.label.empty()
            && control.accent.has_value();
        const bool color_well =
            control.action == UiAction::color_target_stroke
            || control.action == UiAction::color_target_fill;
        const Color displayed_swatch = control.accent.has_value()
            ? board_display_color(*control.accent, board_theme_amount(toolbar)) : Color{};
        const bool no_fill_control =
            control.action == UiAction::fill_none;
        const bool canvas_choice = in_settings && toolbar.settings_page() == SettingsPage::canvas
            && (is_canvas_pattern(control.action) || control.action == UiAction::grid_color_auto
                || control.action == UiAction::edit_background_custom || control.action == UiAction::edit_grid_custom);
        const bool custom_color_control = control.accent.has_value()
            && (control.action == UiAction::edit_stroke_custom
                || control.action == UiAction::edit_background_custom || control.action == UiAction::edit_grid_custom);
        const bool soft_property = canvas_choice || uses_soft_property_selection(control.action);
        const bool history_control =
            control.action == UiAction::undo
            || control.action == UiAction::redo;
        const bool width_stepper = control.action == UiAction::width_decrease
            || control.action == UiAction::width_increase;
        const bool top_bar_control =
            control.bounds.y < toolbar.height();
        const bool quiet_chrome = control.label.empty()
            && !in_settings;
        const bool menu_row = in_settings
            && (toolbar.settings_page() == SettingsPage::file || toolbar.settings_page() == SettingsPage::preferences)
            && control.action != UiAction::settings_close;
        RenderColor background = information
            ? panel
            : (control.enabled
                ? (top_bar_control
                    ? (control.label.empty() && !history_control ? palette.background : top_bar_button)
                    : (quiet_chrome && !history_control ? panel : button))
                : (history_control
                    ? (top_bar_control ? top_bar_button : button)
                    : disabled));
        background = mix_color(background, hovered, animation.hover);
        if (menu_row || (toolbar.is_property_control(control.action) && control.action != UiAction::width_cycle)
            || top_bar_control) {
            background = mix_color(panel, palette.hover_surface, animation.hover);
        }
        if (canvas_choice) {
            background = mix_color(panel, palette.hover_surface, animation.hover * 0.8);
        }
        if (width_stepper) {
            background = control.enabled
                ? mix_color(button, hovered, animation.hover)
                : disabled;
        }
        if (swatch_control || custom_color_control) {
            background = mix_color(panel, hovered, animation.hover);
            background = mix_color(background, palette.accent_soft, animation.selected);
        }
        if (control.action == UiAction::delete_selection
            && animation.hover > 0.0) {
            background = mix_color(
                background,
                {0.82F, 0.20F, 0.18F},
                animation.hover * 0.82);
        }
        if (animation.press > 0.0) {
            background = mix_color(background, accent, animation.press * 0.22);
        }

        UiRect bounds = shifted(control.bounds);
        bounds.y += settings_rise;
        const double inset = animation.press * 1.3 * scale;
        bounds.x += inset;
        bounds.y += inset + animation.press * 0.8 * scale;
        bounds.width -= inset * 2.0;
        bounds.height -= inset * 2.0;

        const double corner = 8.0 * scale;
        set_geometry_alpha(spans, output, control_alpha);
        if (focused_control == &control) {
            append_screen_rounded_rect(
                output,
                {
                    bounds.x - 2.0 * scale,
                    bounds.y - 2.0 * scale,
                    bounds.width + 4.0 * scale,
                    bounds.height + 4.0 * scale,
                },
                corner + 2.0 * scale,
                camera,
                palette.focus);
        }
        if (!information && !swatch_control) {
            // The active tool owns the solid cobalt fill. Property choices use
            // a quieter tint so several simultaneous values do not compete
            // with the tool state.
            background = mix_color(
                background,
                soft_property ? palette.accent_soft : accent,
                animation.selected);
        }
        const bool outlined_property_control =
            color_well || no_fill_control || custom_color_control;
        if (canvas_choice) {
            append_screen_rounded_rect(output, bounds, corner, camera,
                mix_color(panel_border, palette.focus, animation.selected));
            const double edge = (1.0 + animation.selected) * scale;
            append_screen_rounded_rect(output,
                {bounds.x + edge, bounds.y + edge, bounds.width - edge * 2.0, bounds.height - edge * 2.0},
                corner - edge, camera, background);
        } else if (width_stepper) {
            append_screen_rounded_rect(output, bounds, corner, camera,
                mix_color(panel_border, palette.focus,
                    control.enabled ? animation.hover * 0.3 : 0.0));
            const UiRect inner{bounds.x + scale, bounds.y + scale,
                bounds.width - 2.0 * scale, bounds.height - 2.0 * scale};
            append_screen_rounded_rect(output, inner, corner - scale,
                camera, background);
        } else if (outlined_property_control && animation.selected > 0.02) {
            append_screen_rounded_rect(
                output,
                bounds,
                corner,
                camera,
                mix_color(panel_border, palette.focus, animation.selected));
            UiRect selected_inner = bounds;
            selected_inner.x += 2.0 * scale;
            selected_inner.y += 2.0 * scale;
            selected_inner.width -= 4.0 * scale;
            selected_inner.height -= 4.0 * scale;
            append_screen_rounded_rect(
                output,
                selected_inner,
                std::max(2.0 * scale, corner - 2.0 * scale),
                camera,
                background);
        } else {
            append_screen_rounded_rect(
                output, bounds, corner, camera, background);
        }

        if (swatch_control) {
            const Vec2d center{
                bounds.x + bounds.width * 0.5,
                bounds.y + bounds.height * 0.5,
            };
            // Selected colors use the same cobalt outline language as board
            // focus without turning the whole swatch tile into a blue button.
            const double swatch_radius =
                std::min(bounds.width, bounds.height) * 0.28;
            if (animation.selected > 0.02) {
                append_screen_circle(
                    output, center,
                    swatch_radius + 3.0 * scale,
                    camera, palette.focus, 28U);
                append_screen_circle(
                    output, center,
                    swatch_radius + 1.5 * scale,
                    camera, background, 28U);
            } else {
                append_screen_circle(
                    output, center,
                    swatch_radius + 1.25 * scale,
                    camera, panel_border, 28U);
            }
            append_screen_circle(
                output, center, swatch_radius, camera,
                to_render_color(displayed_swatch), 28U);
            if (animation.selected > 0.45) {
                const Color swatch = displayed_swatch;
                const double brightness = (0.2126 * swatch.red
                    + 0.7152 * swatch.green + 0.0722 * swatch.blue) / 255.0;
                const RenderColor mark = brightness > 0.58
                    ? RenderColor{0.10F, 0.12F, 0.16F}
                    : RenderColor{1.0F, 1.0F, 1.0F};
                append_screen_line(output,
                    {center.x - 4.0 * scale, center.y},
                    {center.x - scale, center.y + 3.0 * scale},
                    1.8 * scale, camera, mark);
                append_screen_line(output,
                    {center.x - scale, center.y + 3.0 * scale},
                    {center.x + 5.0 * scale, center.y - 4.0 * scale},
                    1.8 * scale, camera, mark);
            }
        }
        if (control.icon != UiIcon::none) {
            RenderColor icon_color = information
                ? muted
                : (control.enabled ? text : muted);
            if (animation.selected > 0.45) {
                icon_color = soft_property
                    ? palette.focus
                    : RenderColor{0.98F, 0.99F, 1.0F};
            }
            UiRect icon_bounds = bounds;
            if (menu_row) icon_bounds.width = 28.0 * scale;
            if (control.action == UiAction::properties_menu) {
                // Keep the chevron centered in a square at the trailing edge,
                // including when the tab morphs into the collapse button.
                const double icon_width = std::min(bounds.width, bounds.height);
                icon_bounds.x += bounds.width - icon_width;
                icon_bounds.width = icon_width;
            }
            double icon_scale = scale;
            if (control.action == UiAction::file_menu) {
                icon_bounds.x += bounds.width - 20.0 * scale;
                icon_bounds.width = 16.0 * scale;
                icon_scale = scale * 0.65;
            } else if (canvas_choice && is_canvas_pattern(control.action)) {
                icon_bounds.y += 2.0 * scale;
                icon_bounds.height = bounds.height - 20.0 * scale;
                icon_scale = std::min(scale * 0.85, icon_bounds.height / 34.0);
            } else if (control.action == UiAction::edit_background_custom) {
                icon_bounds.width = 32.0 * scale;
                icon_scale = scale * 0.78;
            } else if (control.action == UiAction::edit_grid_custom) {
                icon_scale = scale * 0.78;
            }
            append_icon(output, control, icon_bounds, camera, icon_color, icon_scale);
            if (custom_color_control && animation.selected > 0.02) {
                const Vec2d badge{bounds.x + bounds.width - 9.0 * scale,
                    bounds.y + bounds.height - 8.0 * scale};
                append_screen_circle(output, badge, 5.0 * scale,
                    camera, panel, 20U);
                append_screen_circle(output, badge, 3.75 * scale,
                    camera, to_render_color(displayed_swatch), 20U);
            }
        }
        if (control.partial) {
            append_screen_circle(
                output,
                {
                    bounds.x + bounds.width - 5.5 * scale,
                    bounds.y + 5.5 * scale,
                },
                2.5 * scale,
                camera,
                {0.95F, 0.66F, 0.18F},
                16U);
        }
        set_geometry_alpha(spans, output, 1.0);
    }
    set_geometry_clip(spans, output, std::nullopt);
    const UiRect color_preview = toolbar.custom_color_preview_bounds();
    if (toolbar.settings_open() && color_preview.width > 0.0) {
        set_geometry_alpha(spans, output, settings_reveal);
        append_screen_rounded_rect(output,
            {color_preview.x, color_preview.y, color_preview.height, color_preview.height},
            7.0 * scale, camera, palette.control_surface);
        const double swatch_size = color_preview.height - 8.0 * scale;
        const UiRect swatch{color_preview.x + 4.0 * scale,
            color_preview.y + 4.0 * scale, swatch_size, swatch_size};
        append_screen_rounded_rect(output, swatch,
            5.0 * scale, camera, panel_border);
        append_screen_rounded_rect(output,
            {swatch.x + scale, swatch.y + scale,
                swatch.width - 2.0 * scale, swatch.height - 2.0 * scale},
            4.0 * scale, camera, to_render_color(board_display_color(toolbar.custom_color(), board_theme_amount(toolbar))));
        set_geometry_alpha(spans, output, 1.0);
    }
    if (toolbar.properties_scroll_limit() > 0.0
        && toolbar.properties_reveal() == 1.0
        && !occluded_by_modal(toolbar.properties_bounds())) {
        const UiRect bounds = toolbar.properties_bounds();
        const double track_height = property_clip.height;
        const double thumb_height = std::max(24.0 * scale,
            track_height * track_height
                / (track_height + toolbar.properties_scroll_limit()));
        const double thumb_y = property_clip.y
            + (track_height - thumb_height) * toolbar.properties_scroll()
                / toolbar.properties_scroll_limit();
        append_screen_rounded_rect(output,
            {bounds.x + bounds.width - 6.0 * scale, thumb_y,
                3.0 * scale, thumb_height}, 1.5 * scale, camera, muted);
    }

    const UiControl* const described_control = toolbar.tooltip_control();
    const double described_visibility = described_control == nullptr ? 0.0 : 1.0;
    if (tooltip_allowed && described_control != nullptr && described_visibility > 0.08) {
        const double visibility = described_visibility;
        if (const auto bounds = tooltip_bounds(toolbar, described_control, tooltip_text_width, tooltip_text_height)) {
            tooltip_first_vertex = static_cast<std::uint32_t>(output.size());
            UiRect animated = shifted(*bounds);
            animated.y += (1.0 - visibility) * 4.0 * scale;
            append_soft_shadow(
                output, spans, animated, 8.0 * scale, camera, shadow,
                scale, 0.7 * visibility);
            set_geometry_alpha(spans, output, visibility);
            append_screen_rounded_rect(
                output,
                animated,
                8.0 * scale,
                camera,
                palette.text);
            set_geometry_alpha(spans, output, 1.0);
        }
    }
}

void append_screen_outline(
    std::vector<GeometryVertex>& output,
    const UiRect bounds,
    const double thickness,
    const Camera& camera,
    const RenderColor color)
{
    append_screen_quad(
        output, {bounds.x, bounds.y, bounds.width, thickness}, camera, color);
    append_screen_quad(
        output,
        {bounds.x, bounds.y + bounds.height - thickness, bounds.width, thickness},
        camera,
        color);
    append_screen_quad(
        output, {bounds.x, bounds.y, thickness, bounds.height}, camera, color);
    append_screen_quad(
        output,
        {bounds.x + bounds.width - thickness, bounds.y, thickness, bounds.height},
        camera,
        color);
}

UiRect screen_bounds(const Aabb bounds, const Camera& camera)
{
    const Vec2d first = camera.world_to_screen({bounds.min_x, bounds.min_y});
    const Vec2d second = camera.world_to_screen({bounds.max_x, bounds.max_y});
    return {
        std::min(first.x, second.x),
        std::min(first.y, second.y),
        std::abs(second.x - first.x),
        std::abs(second.y - first.y),
    };
}

void append_selection_handle(
    std::vector<GeometryVertex>& output,
    const Vec2d world_point,
    const Camera& camera)
{
    const Vec2d point = camera.world_to_screen(world_point);
    constexpr double size = 10.0;
    append_screen_quad(
        output,
        {point.x - size * 0.5, point.y - size * 0.5, size, size},
        camera,
        {0.96F, 0.97F, 1.0F});
    append_screen_outline(
        output,
        {point.x - size * 0.5, point.y - size * 0.5, size, size},
        2.0,
        camera,
        {0.12F, 0.48F, 0.94F});
}

void append_selection_geometry(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans,
    const Camera& camera,
    const Document& document,
    const Selection& selection,
    const SelectionPreview* const preview)
{
    constexpr RenderColor selection_color{0.16F, 0.55F, 0.98F};
    const std::optional<Aabb> selected_bounds = selection.bounds(document);
    Aabb effective_bounds{};
    bool has_effective_bounds = selected_bounds.has_value();
    if (has_effective_bounds) {
        effective_bounds = *selected_bounds;
    }
    if (preview != nullptr && has_effective_bounds) {
        if (preview->replacement.has_value()) {
            effective_bounds = preview->replacement->bounds;
        } else {
            has_effective_bounds = false;
            for (const auto id : preview->ids) {
                const Object* const object = document.find(id);
                if (object == nullptr) {
                    continue;
                }
                const Aabb transformed = transformed_object_bounds(
                    *object, preview->transform);
                if (!has_effective_bounds) {
                    effective_bounds = transformed;
                    has_effective_bounds = true;
                    continue;
                }
                effective_bounds.min_x = std::min(
                    effective_bounds.min_x, transformed.min_x);
                effective_bounds.min_y = std::min(
                    effective_bounds.min_y, transformed.min_y);
                effective_bounds.max_x = std::max(
                    effective_bounds.max_x, transformed.max_x);
                effective_bounds.max_y = std::max(
                    effective_bounds.max_y, transformed.max_y);
            }
        }
    }
    if (has_effective_bounds) {
        append_screen_outline(
            output, screen_bounds(effective_bounds, camera), 2.0, camera,
            selection_color);
        if (selection.ids().size() > 1U) {
            append_selection_handle(
                output,
                {effective_bounds.min_x, effective_bounds.min_y},
                camera);
            append_selection_handle(
                output,
                {effective_bounds.max_x, effective_bounds.min_y},
                camera);
            append_selection_handle(
                output,
                {effective_bounds.max_x, effective_bounds.max_y},
                camera);
            append_selection_handle(
                output,
                {effective_bounds.min_x, effective_bounds.max_y},
                camera);
        }
    }
    if (selection.ids().size() == 1U) {
        const Object* object = document.find(selection.ids().front());
        if (preview != nullptr && preview->replacement.has_value()) {
            object = &*preview->replacement;
        }
        if (object != nullptr) {
            if (const auto* const line = std::get_if<Line>(&object->geometry)) {
                const Vec2d start = preview != nullptr
                        && !preview->replacement.has_value()
                    ? preview->transform.apply(line->start)
                    : line->start;
                const Vec2d end = preview != nullptr
                        && !preview->replacement.has_value()
                    ? preview->transform.apply(line->end)
                    : line->end;
                append_selection_handle(output, start, camera);
                append_selection_handle(output, end, camera);
            } else {
                Vec2d first;
                Vec2d second;
                bool has_handles = false;
                if (const auto* const rectangle =
                        std::get_if<RectangleShape>(&object->geometry)) {
                    first = rectangle->first;
                    second = rectangle->second;
                    has_handles = true;
                } else if (const auto* const ellipse =
                               std::get_if<Ellipse>(&object->geometry)) {
                    first = ellipse->first;
                    second = ellipse->second;
                    has_handles = true;
                }
                if (has_handles) {
                    Aabb bounds = Aabb::from_points(first, second);
                    if (preview != nullptr
                        && !preview->replacement.has_value()) {
                        bounds = preview->transform.apply(bounds);
                    }
                    append_selection_handle(
                        output, {bounds.min_x, bounds.min_y}, camera);
                    append_selection_handle(
                        output, {bounds.max_x, bounds.min_y}, camera);
                    append_selection_handle(
                        output, {bounds.max_x, bounds.max_y}, camera);
                    append_selection_handle(
                        output, {bounds.min_x, bounds.max_y}, camera);
                }
            }
        }
    }
    if (selection.marquee().has_value()) {
        const UiRect marquee = screen_bounds(*selection.marquee(), camera);
        // A whisper of accent inside the marquee makes the captured region
        // legible over any board background without hiding content.
        set_geometry_alpha(spans, output, 0.10);
        append_screen_quad(output, marquee, camera, {0.30F, 0.58F, 0.98F});
        set_geometry_alpha(spans, output, 1.0);
        append_screen_outline(
            output,
            marquee,
            1.5,
            camera,
            {0.38F, 0.72F, 1.0F});
    }
}

void append_unsaved_dialog_geometry(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans,
    const Camera& camera,
    const Toolbar& toolbar,
    const UnsavedDialog& dialog)
{
    if (!dialog.visible()) {
        return;
    }
    const InterfacePalette palette = toolbar_palette(toolbar);
    const bool light = toolbar.theme() == Theme::light;
    const double reveal = dialog.reveal();
    const double scale = dialog.scale();
    const UiRect card = dialog.bounds();
    const RenderColor shadow = light
        ? RenderColor{0.06F, 0.09F, 0.16F}
        : RenderColor{0.0F, 0.0F, 0.0F};
    const RenderColor danger = light
        ? RenderColor{0.72F, 0.16F, 0.20F}
        : RenderColor{1.0F, 0.43F, 0.47F};

    set_geometry_alpha(spans, output, 0.42 * reveal);
    append_screen_quad(
        output,
        {
            0.0,
            0.0,
            toolbar.viewport_width(),
            toolbar.viewport_height(),
        },
        camera,
        {0.015F, 0.022F, 0.035F});

    append_soft_shadow(
        output,
        spans,
        card,
        14.0 * scale,
        camera,
        shadow,
        scale,
        2.2 * reveal,
        1.25);
    set_geometry_alpha(spans, output, reveal);
    append_screen_rounded_rect(
        output, card, 14.0 * scale, camera, palette.border);
    UiRect inner = card;
    inner.x += 1.0 * scale;
    inner.y += 1.0 * scale;
    inner.width -= 2.0 * scale;
    inner.height -= 2.0 * scale;
    append_screen_rounded_rect(
        output,
        inner,
        13.0 * scale,
        camera,
        palette.surface);

    const Vec2d icon_center{
        card.x + 42.0 * scale,
        card.y + 43.0 * scale,
    };
    append_screen_circle(
        output,
        icon_center,
        17.0 * scale,
        camera,
        palette.accent_soft,
        28U);
    append_screen_circle(
        output,
        icon_center,
        9.0 * scale,
        camera,
        palette.primary,
        24U);
    append_screen_line(
        output,
        {icon_center.x, icon_center.y - 4.2 * scale},
        {icon_center.x, icon_center.y + 1.2 * scale},
        1.7 * scale,
        camera,
        palette.on_primary);
    append_screen_circle(
        output,
        {icon_center.x, icon_center.y + 4.7 * scale},
        1.15 * scale,
        camera,
        palette.on_primary,
        16U);

    constexpr std::array choices{
        UnsavedDialogChoice::cancel,
        UnsavedDialogChoice::discard,
        UnsavedDialogChoice::save,
    };
    for (const UnsavedDialogChoice choice : choices) {
        UiRect button = dialog.button_bounds(choice);
        const double press = dialog.press(choice);
        const double inset = press * 1.2 * scale;
        button.x += inset;
        button.y += inset + press * 0.5 * scale;
        button.width -= inset * 2.0;
        button.height -= inset * 2.0;
        const double hover = dialog.hover(choice);
        RenderColor fill = choice == UnsavedDialogChoice::save
            ? mix_color(
                palette.primary,
                palette.primary_hover,
                hover)
            : mix_color(
                palette.control_surface,
                choice == UnsavedDialogChoice::discard
                    ? mix_color(palette.hover_surface, danger, 0.08)
                    : palette.hover_surface,
                hover);
        if (press > 0.0) {
            fill = mix_color(fill, palette.background, press * 0.14);
        }

        if (dialog.focused_choice() == choice) {
            append_screen_rounded_rect(
                output,
                {
                    button.x - 2.0 * scale,
                    button.y - 2.0 * scale,
                    button.width + 4.0 * scale,
                    button.height + 4.0 * scale,
                },
                9.0 * scale,
                camera,
                palette.focus);
        }
        append_screen_rounded_rect(
            output,
            button,
            7.0 * scale,
            camera,
            choice == UnsavedDialogChoice::save
                ? fill
                : palette.border);
        if (choice != UnsavedDialogChoice::save) {
            UiRect button_inner = button;
            button_inner.x += 1.0 * scale;
            button_inner.y += 1.0 * scale;
            button_inner.width -= 2.0 * scale;
            button_inner.height -= 2.0 * scale;
            append_screen_rounded_rect(
                output,
                button_inner,
                6.0 * scale,
                camera,
                fill);
        }
    }
    set_geometry_alpha(spans, output, 1.0);
}

} // namespace

GpuRenderer::GpuRenderer(SDL_Window& window, const bool enable_antialiasing)
    : window_{&window}, antialiasing_enabled_{enable_antialiasing}
{
    try {
        scene_geometry_.reserve(initial_scene_vertex_capacity);
        draft_gpu_geometry_.reserve(initial_draft_vertex_capacity);
        geometry_.reserve(initial_overlay_vertex_capacity);

        constexpr auto shader_formats =
            SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#if defined(NDEBUG)
        constexpr bool enable_validation = false;
#else
        constexpr bool enable_validation = true;
#endif

        device_ = SDL_CreateGPUDevice(
            shader_formats, enable_validation, requested_driver());
        if (device_ == nullptr) {
            throw_sdl("GPU device creation");
        }

        if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
            throw_sdl("GPU window claim");
        }
        window_claimed_ = true;

        if (!SDL_SetGPUAllowedFramesInFlight(device_, 1U)) {
            throw_sdl("GPU frame-latency configuration");
        }

        create_geometry_buffers();
        create_pipeline();
        create_background_pipeline();
        create_retained_pipeline();
        create_text_resources();
        create_image_resources();
        collect_diagnostics();
        log::write(log::Level::info, diagnostics_);
    } catch (...) {
        release();
        throw;
    }
}

GpuRenderer::~GpuRenderer()
{
    release();
}

bool GpuRenderer::render(
    const Camera& camera,
    const Document& document,
    const ObjectDraft* const preview,
    const Toolbar& toolbar,
    const Selection& selection,
    const HomeView* const home,
    const SelectionPreview* const selection_preview,
    const UnsavedDialog* const unsaved_dialog,
    const DrawingCursor* const drawing_cursor,
    const double navigation_opacity)
{
    const auto frame_start = std::chrono::steady_clock::now();
    if (rendering_) {
        return false;
    }
    struct RenderScope final {
        explicit RenderScope(bool& active) noexcept : active_(active)
        {
            active_ = true;
        }
        ~RenderScope()
        {
            active_ = false;
        }
        bool& active_;
    } render_scope{rendering_};

    ++stats_.frame;
    reap_mesh_submissions();
    sync_document_changes(document);
    resident_scene_invalidated_ = false;
    stats_.background_draw_calls = 0U;
    stats_.resident_upload_bytes = 0U;
    stats_.resident_draws = 0U;
    stats_.visible_objects = 0U;
    stats_.visibility_query_reuses = 0U;
    stats_.rendered_objects = 0U;
    stats_.scene_batches = 0U;
    stats_.batch_reuse_waits = 0U;
    stats_.batch_wait_milliseconds = 0.0;
    stats_.peak_scene_batch_vertices = 0U;
    stats_.peak_stroke_page_vertices = 0U;
    stats_.tessellated_objects = 0U;
    stats_.cache_hits = 0U;
    stats_.segment_index_builds = 0U;
    stats_.scene_upload_bytes = 0U;
    stats_.draft_upload_bytes = 0U;
    stats_.geometry_upload_bytes = 0U;
    stats_.text_upload_bytes = 0U;
    stats_.thumbnail_upload_bytes = 0U;
    stats_.image_upload_bytes = 0U;
    stats_.image_decodes = 0U;
    stats_.draw_calls = 0U;
    stats_.text_draw_calls = 0U;
    stats_.tooltip_draw_calls = 0U;
    tooltip_first_vertex_.reset();
    tooltip_first_text_batch_ = 0U;
    tooltip_allowed_this_frame_ = unsaved_dialog == nullptr || !unsaved_dialog->visible();
    stats_.draft_full_rebuilds = 0U;
    stats_.draft_appended_points = 0U;
    stats_.background_milliseconds = 0.0;
    stats_.query_milliseconds = 0.0;
    stats_.objects_milliseconds = 0.0;
    stats_.overlay_ui_milliseconds = 0.0;
    stats_.staging_milliseconds = 0.0;
    stats_.command_acquire_milliseconds = 0.0;
    stats_.swapchain_wait_milliseconds = 0.0;
    stats_.command_record_milliseconds = 0.0;
    stats_.submit_milliseconds = 0.0;
    stats_.total_cpu_milliseconds = 0.0;
    stats_.scene_rebuilt = false;
    thumbnail_upload_pixels_.clear();
    thumbnail_uploads_.clear();
    image_upload_pixels_.clear();
    image_uploads_.clear();
    const auto build_start = std::chrono::steady_clock::now();
    geometry_spans_.clear();
    geometry_spans_.push_back(GeometrySpan{0U, 1.0F});
    if (home != nullptr) {
        if (!home_cache_trimmed_) {
            trim_board_caches_for_home();
            document.clear_transient_caches();
            home_cache_trimmed_ = true;
        }
        scene_active_this_frame_ = false;
        scene_upload_pending_ = false;
        draft_active_this_frame_ = false;
        draft_upload_pending_ = false;
        geometry_.clear();
        append_home_geometry(geometry_, geometry_spans_, camera, *home);
        build_home_text_geometry(*home, camera);
    } else {
        home_cache_trimmed_ = false;
        build_board_geometry(
            camera,
            document,
            preview,
            toolbar,
            selection,
            selection_preview,
            drawing_cursor);
        if (unsaved_dialog != nullptr && unsaved_dialog->visible()) {
            append_unsaved_dialog_geometry(
                geometry_,
                geometry_spans_,
                camera,
                toolbar,
                *unsaved_dialog);
        }
        const double dialog_reveal =
            unsaved_dialog != nullptr ? unsaved_dialog->reveal() : 0.0;
        build_text_geometry(
            toolbar,
            camera,
            1.0 - dialog_reveal * 0.68);
        if (unsaved_dialog != nullptr && unsaved_dialog->visible()) {
            build_unsaved_dialog_text_geometry(
                *unsaved_dialog, toolbar);
        }
    }
    trim_geometry_to_vertex_budget(geometry_, geometry_spans_);
    if (tooltip_first_vertex_.has_value() && *tooltip_first_vertex_ >= geometry_.size()) {
        // An exhausted geometry budget must not leave floating tooltip text.
        text_batches_.resize(std::min(tooltip_first_text_batch_, text_batches_.size()));
        tooltip_first_vertex_.reset();
    }
    const auto interface_vertex_count = static_cast<Uint32>(geometry_.size());
    // Composite over geometry, thumbnails, labels, and tooltips together. The
    // transition needs only one screen-sized quad and no retained board copy.
    const bool navigation_overlay = std::isfinite(navigation_opacity)
        && navigation_opacity > 0.0
        && geometry_.size() + 6U <= maximum_vertex_count;
    if (navigation_overlay) {
        const RenderColor backdrop = home != nullptr
            ? home_palette(*home).background : toolbar_palette(toolbar).background;
        geometry_spans_.push_back(GeometrySpan{
            interface_vertex_count,
            static_cast<float>(std::clamp(navigation_opacity, 0.0, 1.0)),
            std::nullopt});
        append_screen_quad(geometry_,
            {0.0, 0.0, camera.viewport().x, camera.viewport().y}, camera, backdrop);
    }
    const auto build_end = std::chrono::steady_clock::now();
    stats_.build_milliseconds = std::chrono::duration<double, std::milli>(
        build_end - build_start).count();
    stats_.emitted_vertices =
        geometry_.size()
        + (scene_active_this_frame_
            ? scene_total_vertices_
            : 0U)
        + (draft_active_this_frame_
            ? draft_gpu_geometry_.size()
            : 0U)
        + (scene_active_this_frame_ ? image_vertices_.size() : 0U);
    stats_.cache_entries = object_cache_.size();
    stats_.cache_bytes = geometry_cache_bytes_;
    stats_.cache_metadata_bytes = geometry_metadata_bytes();
    stats_.evictions = total_evictions_;

    const auto staging_start = std::chrono::steady_clock::now();
    const std::size_t scene_vertex_count =
        scene_geometry_.size();
    if (scene_active_this_frame_ && !scene_streamed_
        && ensure_vertex_buffer_capacity(
            scene_vertex_buffer_,
            scene_vertex_capacity_,
            scene_vertex_count,
            "GPU scene vertex-buffer growth")) {
        scene_upload_pending_ = true;
    }
    if (scene_active_this_frame_
        && ensure_image_vertex_buffer_capacity(image_vertices_.size())) {
        scene_upload_pending_ = true;
    }
    const std::size_t draft_vertex_count =
        draft_gpu_geometry_.size();
    if (draft_active_this_frame_
        && ensure_vertex_buffer_capacity(
            draft_vertex_buffer_,
            draft_vertex_capacity_,
            draft_vertex_count,
            "GPU draft vertex-buffer growth")) {
        draft_upload_first_vertex_ = 0U;
        draft_upload_pending_ = true;
    }
    const std::size_t overlay_vertex_count =
        geometry_.size();
    static_cast<void>(ensure_vertex_buffer_capacity(
        vertex_buffer_,
        overlay_vertex_capacity_,
        overlay_vertex_count,
        "GPU overlay vertex-buffer growth"));

    Uint32 scene_geometry_bytes = 0U;
    if (scene_active_this_frame_ && scene_upload_pending_ && !scene_streamed_) {
        scene_geometry_bytes = static_cast<Uint32>(
            scene_geometry_.size() * sizeof(GeometryVertex));
        stats_.scene_upload_bytes = scene_geometry_bytes;
    }
    Uint32 draft_geometry_bytes = 0U;
    Uint32 draft_destination_offset = 0U;
    std::size_t draft_first_vertex = 0U;
    if (draft_active_this_frame_ && draft_upload_pending_) {
        if (draft_upload_first_vertex_ > draft_vertex_count) {
            throw std::runtime_error{
                "Draft upload offset exceeded its vertex count"};
        }
        draft_first_vertex = draft_upload_first_vertex_;
        draft_geometry_bytes = static_cast<Uint32>(
            (draft_gpu_geometry_.size() - draft_first_vertex)
            * sizeof(GeometryVertex));
        draft_destination_offset = static_cast<Uint32>(
            draft_upload_first_vertex_ * sizeof(GeometryVertex));
        stats_.draft_upload_bytes = draft_geometry_bytes;
    }
    const auto geometry_bytes = static_cast<Uint32>(
        geometry_.size() * sizeof(GeometryVertex));
    stats_.geometry_upload_bytes = geometry_bytes;
    Uint32 image_geometry_bytes = 0U;
    if (scene_active_this_frame_ && scene_upload_pending_) {
        image_geometry_bytes = static_cast<Uint32>(
            image_vertices_.size() * sizeof(ImageVertex));
        stats_.scene_upload_bytes += image_geometry_bytes;
    }
    const Uint32 scene_transfer_offset = 0U;
    const Uint32 draft_transfer_offset = scene_geometry_bytes;
    const Uint32 geometry_transfer_offset =
        draft_transfer_offset + draft_geometry_bytes;
    const Uint32 image_geometry_transfer_offset =
        geometry_transfer_offset + geometry_bytes;
    const Uint32 resident_transfer_offset = image_geometry_transfer_offset + image_geometry_bytes;
    const auto resident_bytes = static_cast<Uint32>(mesh_upload_vertices_.size() * sizeof(GeometryVertex));
    stats_.resident_upload_bytes = resident_bytes;
    stats_.scene_upload_bytes += resident_bytes;
    const std::size_t total_geometry_upload_bytes =
        static_cast<std::size_t>(resident_transfer_offset) + resident_bytes;
    constexpr std::size_t texture_upload_alignment = 512U;
    const std::size_t thumbnail_transfer_offset =
        thumbnail_uploads_.empty()
        ? total_geometry_upload_bytes
        : (total_geometry_upload_bytes + texture_upload_alignment - 1U)
            / texture_upload_alignment * texture_upload_alignment;
    const std::size_t total_upload_bytes =
        (thumbnail_transfer_offset + thumbnail_upload_pixels_.size()
            + texture_upload_alignment - 1U)
            / texture_upload_alignment * texture_upload_alignment
        + image_upload_pixels_.size();
    const std::size_t image_texture_transfer_offset = total_upload_bytes
        - image_upload_pixels_.size();
    ensure_transfer_capacity(total_upload_bytes);
    if (total_upload_bytes > 0U) {
        void* const mapped =
            SDL_MapGPUTransferBuffer(device_, transfer_buffer_, true);
        if (mapped == nullptr) {
            throw_sdl("GPU staging-arena mapping");
        }
        auto* const bytes = static_cast<std::byte*>(mapped);
        if (scene_geometry_bytes > 0U) {
            std::memcpy(
                bytes + scene_transfer_offset,
                scene_geometry_.data(),
                scene_geometry_bytes);
        }
        if (draft_geometry_bytes > 0U) {
            std::memcpy(
                bytes + draft_transfer_offset,
                draft_gpu_geometry_.data() + draft_first_vertex,
                draft_geometry_bytes);
        }
        if (geometry_bytes > 0U) {
            std::memcpy(
                bytes + geometry_transfer_offset,
                geometry_.data(),
                geometry_bytes);
        }
        if (image_geometry_bytes > 0U) {
            std::memcpy(
                bytes + image_geometry_transfer_offset,
                image_vertices_.data(),
                image_geometry_bytes);
        }
        if (resident_bytes != 0U) std::memcpy(bytes + resident_transfer_offset,
            mesh_upload_vertices_.data(), resident_bytes);
        if (!thumbnail_upload_pixels_.empty()) {
            std::memcpy(
                bytes + thumbnail_transfer_offset,
                thumbnail_upload_pixels_.data(),
                thumbnail_upload_pixels_.size());
        }
        if (!image_upload_pixels_.empty()) {
            std::memcpy(
                bytes + image_texture_transfer_offset,
                image_upload_pixels_.data(),
                image_upload_pixels_.size());
        }
        SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);
    }
    stats_.cpu_geometry_live_bytes =
        (scene_geometry_.size()
            + draft_gpu_geometry_.size()
            + geometry_.size())
            * sizeof(GeometryVertex)
        + (draft_shape_geometry_.size()
            + selection_preview_geometry_.size())
            * sizeof(CachedWorldVertex)
        + (selection_preview_points_.size()
            + draft_render_points_.size())
            * sizeof(Vec2d)
        + image_vertices_.size() * sizeof(ImageVertex)
        + scene_draws_.size() * sizeof(SceneDraw);
    stats_.cpu_geometry_capacity_bytes =
        (scene_geometry_.capacity()
            + draft_gpu_geometry_.capacity()
            + geometry_.capacity())
            * sizeof(GeometryVertex)
        + (draft_shape_geometry_.capacity()
            + selection_preview_geometry_.capacity())
            * sizeof(CachedWorldVertex)
        + (selection_preview_points_.capacity()
            + draft_render_points_.capacity())
            * sizeof(Vec2d)
        + image_vertices_.capacity() * sizeof(ImageVertex)
        + scene_draws_.capacity() * sizeof(SceneDraw);
    stats_.gpu_geometry_capacity_bytes =
        (scene_vertex_capacity_
            + draft_vertex_capacity_
            + overlay_vertex_capacity_)
        * sizeof(GeometryVertex);
    stats_.gpu_geometry_capacity_bytes +=
        image_vertex_capacity_ * sizeof(ImageVertex);
    stats_.transfer_capacity_bytes = transfer_capacity_bytes_;
    stats_.text_geometry_capacity_bytes =
        text_vertices_.capacity() * sizeof(TextVertex)
        + text_indices_.capacity() * sizeof(std::uint16_t)
        + text_batches_.capacity() * sizeof(TextBatch)
        + filename_caret_stops_.capacity()
            * sizeof(decltype(filename_caret_stops_)::value_type)
        + sizeof(TextVertex) * maximum_text_vertices
        + sizeof(std::uint16_t) * maximum_text_indices
        + sizeof(TextVertex) * maximum_text_vertices
        + sizeof(std::uint16_t) * maximum_text_indices;
    stats_.query_scratch_capacity_bytes =
        visible_objects_.capacity() * sizeof(const Object*)
        + visible_object_ids_.capacity() * sizeof(ObjectId);
    stats_.text_cache_entries = text_cache_.size();
    stats_.thumbnail_cache_entries = thumbnail_textures_.size();
    stats_.thumbnail_upload_bytes = thumbnail_upload_pixels_.size();
    stats_.image_upload_bytes = image_upload_pixels_.size();
    stats_.image_texture_cache_entries = image_textures_.size();
    stats_.image_texture_tiles = 0U;
    for (const auto& [id, cached] : image_textures_) {
        static_cast<void>(id);
        stats_.image_texture_tiles += cached.tiles.size();
    }
    stats_.image_texture_cache_bytes = image_texture_bytes_;
    stats_.image_texture_evictions = image_texture_evictions_;
    stats_.image_decode_cache_bytes = image_decode_cache_.bytes();

    const auto text_vertex_bytes = static_cast<Uint32>(
        text_vertices_.size() * sizeof(TextVertex));
    const auto text_index_bytes = static_cast<Uint32>(
        text_indices_.size() * sizeof(std::uint16_t));
    stats_.text_upload_bytes =
        static_cast<std::size_t>(text_vertex_bytes) + text_index_bytes;
    constexpr Uint32 text_index_transfer_offset = static_cast<Uint32>(
        sizeof(TextVertex) * maximum_text_vertices);
    if (!text_vertices_.empty()) {
        void* const text_mapped =
            SDL_MapGPUTransferBuffer(device_, text_transfer_buffer_, true);
        if (text_mapped == nullptr) {
            throw_sdl("GPU text-buffer mapping");
        }
        std::memcpy(text_mapped, text_vertices_.data(), text_vertex_bytes);
        std::memcpy(
            static_cast<std::byte*>(text_mapped) + text_index_transfer_offset,
            text_indices_.data(),
            text_index_bytes);
        SDL_UnmapGPUTransferBuffer(device_, text_transfer_buffer_);
    }
    const auto staging_end = std::chrono::steady_clock::now();
    stats_.staging_milliseconds =
        std::chrono::duration<double, std::milli>(
            staging_end - staging_start).count();

    const auto command_acquire_start = std::chrono::steady_clock::now();
    SDL_GPUCommandBuffer* command_buffer =
        SDL_AcquireGPUCommandBuffer(device_);
    if (command_buffer == nullptr) {
        throw_sdl("GPU command-buffer acquisition");
    }
    CommandBufferGuard command_buffer_guard{command_buffer};
    const auto command_acquire_end = std::chrono::steady_clock::now();
    stats_.command_acquire_milliseconds =
        std::chrono::duration<double, std::milli>(
            command_acquire_end - command_acquire_start).count();

    const auto command_record_start = std::chrono::steady_clock::now();
    SDL_GPUCopyPass* const copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (copy_pass == nullptr) {
        throw_sdl("GPU geometry copy-pass creation");
    }

    if (scene_active_this_frame_ && scene_upload_pending_
        && scene_geometry_bytes > 0U) {
        const SDL_GPUTransferBufferLocation scene_source{
            transfer_buffer_, scene_transfer_offset};
        const SDL_GPUBufferRegion scene_destination{
            scene_vertex_buffer_,
            0U,
            scene_geometry_bytes,
        };
        SDL_UploadToGPUBuffer(
            copy_pass,
            &scene_source,
            &scene_destination,
            true);
    }
    if (draft_active_this_frame_ && draft_upload_pending_
        && draft_geometry_bytes > 0U) {
        const SDL_GPUTransferBufferLocation draft_source{
            transfer_buffer_, draft_transfer_offset};
        const SDL_GPUBufferRegion draft_destination{
            draft_vertex_buffer_,
            draft_destination_offset,
            draft_geometry_bytes,
        };
        SDL_UploadToGPUBuffer(
            copy_pass,
            &draft_source,
            &draft_destination,
            // A partial update must preserve the retained prefix. With one
            // frame in flight, queue ordering keeps the prior draw safe.
            draft_upload_first_vertex_ == 0U);
    }
    if (geometry_bytes > 0U) {
        const SDL_GPUTransferBufferLocation source{
            transfer_buffer_, geometry_transfer_offset};
        const SDL_GPUBufferRegion destination{
            vertex_buffer_,
            0U,
            geometry_bytes,
        };
        SDL_UploadToGPUBuffer(
            copy_pass, &source, &destination, true);
    }
    if (image_geometry_bytes > 0U) {
        const SDL_GPUTransferBufferLocation source{
            transfer_buffer_, image_geometry_transfer_offset};
        const SDL_GPUBufferRegion destination{
            image_vertex_buffer_, 0U, image_geometry_bytes};
        SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);
    }
    if (!text_vertices_.empty()) {
        const SDL_GPUTransferBufferLocation text_vertex_source{
            text_transfer_buffer_, 0U};
        const SDL_GPUBufferRegion text_vertex_destination{
            text_vertex_buffer_, 0U, text_vertex_bytes};
        SDL_UploadToGPUBuffer(
            copy_pass,
            &text_vertex_source,
            &text_vertex_destination,
            true);
        const SDL_GPUTransferBufferLocation text_index_source{
            text_transfer_buffer_, text_index_transfer_offset};
        const SDL_GPUBufferRegion text_index_destination{
            text_index_buffer_, 0U, text_index_bytes};
        SDL_UploadToGPUBuffer(
            copy_pass,
            &text_index_source,
            &text_index_destination,
            true);
    }
    for (const auto& upload : thumbnail_uploads_) {
        const SDL_GPUTextureTransferInfo source{
            transfer_buffer_,
            static_cast<Uint32>(
                thumbnail_transfer_offset + upload.data_offset),
            BoardPreview::pixel_width,
            BoardPreview::pixel_height,
        };
        const SDL_GPUTextureRegion destination{
            upload.texture,
            0U,
            0U,
            0U,
            0U,
            0U,
            BoardPreview::pixel_width,
            BoardPreview::pixel_height,
            1U,
        };
        SDL_UploadToGPUTexture(
            copy_pass, &source, &destination, true);
    }
    for (const auto& upload : image_uploads_) {
        const SDL_GPUTextureTransferInfo source{
            transfer_buffer_,
            static_cast<Uint32>(image_texture_transfer_offset + upload.data_offset),
            upload.width,
            upload.height,
        };
        const SDL_GPUTextureRegion destination{
            upload.texture, 0U, 0U, 0U, 0U, 0U,
            upload.width, upload.height, 1U,
        };
        SDL_UploadToGPUTexture(copy_pass, &source, &destination, true);
    }
    SDL_EndGPUCopyPass(copy_pass);
    if (!mesh_uploads_.empty()) {
        auto* resident_copy = SDL_BeginGPUCopyPass(command_buffer);
        if (resident_copy == nullptr) throw_sdl("GPU resident copy pass");
        for (const auto& upload : mesh_uploads_) {
            const SDL_GPUTransferBufferLocation source{transfer_buffer_, resident_transfer_offset
                + upload.first * static_cast<Uint32>(sizeof(GeometryVertex))};
            const SDL_GPUBufferRegion destination{mesh_pages_[upload.range.page],
                upload.range.first * static_cast<Uint32>(sizeof(GeometryVertex)),
                upload.range.count * static_cast<Uint32>(sizeof(GeometryVertex))};
            SDL_UploadToGPUBuffer(resident_copy, &source, &destination, false);
        }
        SDL_EndGPUCopyPass(resident_copy);
    }
    for (const auto& upload : image_uploads_) {
        if (upload.mip_levels > 1U) {
            SDL_GenerateMipmapsForGPUTexture(command_buffer, upload.texture);
        }
    }

    if (scene_active_this_frame_ && scene_streamed_) {
        int window_width = 0;
        int window_height = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &window_width, &window_height)
            || window_width <= 0 || window_height <= 0) return false;
        if (scene_upload_pending_ || scene_stream_texture_ == nullptr
            || scene_stream_width_ != static_cast<std::uint32_t>(window_width)
            || scene_stream_height_ != static_cast<std::uint32_t>(window_height)) {
            // Complete resource uploads before reusing bounded batch staging.
            // A warm scene is sampled from its viewport-sized raster cache.
            submit_and_wait(device_, command_buffer_guard.release());
            render_scene_batches(camera, document, toolbar,
                static_cast<std::uint32_t>(window_width),
                static_cast<std::uint32_t>(window_height));
            command_buffer = SDL_AcquireGPUCommandBuffer(device_);
            if (command_buffer == nullptr) throw_sdl("GPU final batch acquisition");
            command_buffer_guard.reset(command_buffer);
        } else {
            stats_.scene_batches = scene_stream_batches_;
            stats_.peak_scene_batch_vertices = scene_stream_peak_vertices_;
        }
    }

    SDL_GPUTexture* swapchain_texture = nullptr;
    Uint32 drawable_width = 0U;
    Uint32 drawable_height = 0U;
    const auto swapchain_wait_start = std::chrono::steady_clock::now();
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            command_buffer,
            window_,
            &swapchain_texture,
            &drawable_width,
            &drawable_height)) {
        throw_sdl("GPU swapchain acquisition");
    }
    command_buffer_guard.mark_swapchain_acquired();
    const auto swapchain_wait_end = std::chrono::steady_clock::now();
    stats_.swapchain_wait_milliseconds =
        std::chrono::duration<double, std::milli>(
            swapchain_wait_end - swapchain_wait_start).count();

    // D3D12 can briefly return a swapchain image with a zero drawable extent
    // while a window is hidden, minimized, or transitioning between display
    // states. Recording a render pass for that image only fails later when
    // SDL closes the command list, obscuring the real cause behind E_INVALIDARG.
    if (swapchain_texture == nullptr
        || drawable_width == 0U
        || drawable_height == 0U) {
        stats_.total_cpu_milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - frame_start).count();
        return false;
    }

    const bool capture_pixel = pixel_readback_request_.has_value()
        && (*pixel_readback_request_)[0] < drawable_width
        && (*pixel_readback_request_)[1] < drawable_height;
    SDL_GPUTexture* final_surface = swapchain_texture;
    if (capture_pixel) {
        // Vulkan swapchain images are not transfer sources. Render the final
        // frame to a normal single-sample texture, then present that same image.
        if (pixel_readback_surface_ == nullptr || pixel_readback_width_ != drawable_width
            || pixel_readback_height_ != drawable_height) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = drawable_width;
            info.height = drawable_height;
            info.layer_count_or_depth = 1U;
            info.num_levels = 1U;
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device_, &info);
            if (replacement == nullptr) throw_sdl("GPU final-frame diagnostic surface");
            if (pixel_readback_surface_ != nullptr) SDL_ReleaseGPUTexture(device_, pixel_readback_surface_);
            pixel_readback_surface_ = replacement;
            pixel_readback_width_ = drawable_width;
            pixel_readback_height_ = drawable_height;
        }
        if (pixel_readback_transfer_ == nullptr) {
            const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, 256U, 0U};
            pixel_readback_transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
            if (pixel_readback_transfer_ == nullptr) throw_sdl("GPU final-frame diagnostic transfer");
        }
        final_surface = pixel_readback_surface_;
    }

    // Keep MSAA active while resizing. The targets grow geometrically, then
    // the current drawable region is blitted from the resolved texture to the
    // swapchain. This avoids both jagged live-resize frames and per-tick GPU
    // texture reallocations.
    const bool direct_present = antialiasing_samples_ == 1;
    if (!direct_present) {
        ensure_msaa_target(drawable_width, drawable_height);
    }

    SDL_GPUColorTargetInfo color_target{};
    const RenderColor clear_color = mix_color(
        toolbar.previous_theme() == Theme::light
            ? RenderColor{0.82F, 0.84F, 0.88F}
            : RenderColor{0.025F, 0.03F, 0.045F},
        toolbar.theme() == Theme::light
            ? RenderColor{0.82F, 0.84F, 0.88F}
            : RenderColor{0.025F, 0.03F, 0.045F},
        smooth_theme_transition(toolbar));
    color_target.clear_color = {
        clear_color.red,
        clear_color.green,
        clear_color.blue,
        1.0F,
    };
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    const bool msaa_active = !direct_present
        && msaa_texture_ != nullptr
        && msaa_resolve_texture_ != nullptr
        && msaa_width_ >= drawable_width
        && msaa_height_ >= drawable_height;
    if (msaa_active) {
        color_target.texture = msaa_texture_;
        color_target.resolve_texture = msaa_resolve_texture_;
        color_target.store_op = SDL_GPU_STOREOP_RESOLVE;
        color_target.cycle = true;
        color_target.cycle_resolve_texture = true;
    } else {
        color_target.texture = final_surface;
        color_target.store_op = SDL_GPU_STOREOP_STORE;
    }
    if (scene_active_this_frame_ && !scene_streamed_) {
        stats_.scene_batches = 1U;
        stats_.peak_scene_batch_vertices = scene_geometry_.size();
    }
    SDL_GPUGraphicsPipeline* const opaque_pipeline =
        !msaa_active && pipeline_direct_ != nullptr
            ? pipeline_direct_
            : pipeline_;
    SDL_GPUGraphicsPipeline* const translucent_pipeline =
        !msaa_active && blend_pipeline_direct_ != nullptr
            ? blend_pipeline_direct_
            : blend_pipeline_;
    SDL_GPUGraphicsPipeline* const glyph_pipeline =
        !msaa_active && text_pipeline_direct_ != nullptr
            ? text_pipeline_direct_
            : text_pipeline_;
    SDL_GPUGraphicsPipeline* const board_image_pipeline =
        !msaa_active && image_pipeline_direct_ != nullptr
            ? image_pipeline_direct_
            : image_pipeline_;

    const auto viewport = camera.viewport();
    const auto zoom = static_cast<float>(camera.zoom());
    const auto width = static_cast<float>(viewport.x);
    const auto height = static_cast<float>(viewport.y);
    const std::array<float, 16> camera_matrix{{
        2.0F * zoom / width, 0.0F, 0.0F, 0.0F,
        0.0F, -2.0F * zoom / height, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    }};

    const std::array<float, 32U> board_matrices{{
        camera_matrix[0], camera_matrix[1], camera_matrix[2], camera_matrix[3],
        camera_matrix[4], camera_matrix[5], camera_matrix[6], camera_matrix[7],
        camera_matrix[8], camera_matrix[9], camera_matrix[10], camera_matrix[11],
        camera_matrix[12], camera_matrix[13], camera_matrix[14], camera_matrix[15],
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    }};
    SDL_PushGPUVertexUniformData(
        command_buffer,
        0U,
        board_matrices.data(),
        static_cast<Uint32>(sizeof(board_matrices)));

    const auto theme_parameters = board_theme_parameters(toolbar);
    SDL_PushGPUVertexUniformData(command_buffer, 1U, &theme_parameters, sizeof(theme_parameters));
    SDL_GPUGraphicsPipeline* const board_vector_pipeline =
        !msaa_active && retained_pipeline_direct_ != nullptr ? retained_pipeline_direct_ : retained_pipeline_;
    SDL_GPURenderPass* const render_pass =
        SDL_BeginGPURenderPass(command_buffer, &color_target, 1U, nullptr);
    if (render_pass == nullptr) {
        throw_sdl("GPU render-pass creation");
    }
    if (msaa_active) {
        // The reserved targets can be larger than the current window. Keep
        // the coordinate mapping and resolved region at the drawable size.
        const SDL_GPUViewport drawable_viewport{
            0.0F,
            0.0F,
            static_cast<float>(drawable_width),
            static_cast<float>(drawable_height),
            0.0F,
            1.0F,
        };
        SDL_SetGPUViewport(render_pass, &drawable_viewport);
    }

    if (scene_active_this_frame_ && scene_streamed_) {
        SDL_BindGPUGraphicsPipeline(render_pass, board_image_pipeline);
        const SDL_GPUBufferBinding binding{scene_stream_quad_, 0U};
        SDL_BindGPUVertexBuffers(render_pass, 0U, &binding, 1U);
        const SDL_GPUTextureSamplerBinding sampler{scene_stream_texture_, image_sampler_};
        SDL_BindGPUFragmentSamplers(render_pass, 0U, &sampler, 1U);
        SDL_DrawGPUPrimitives(render_pass, 6U, 1U, 0U, 0U);
        ++stats_.draw_calls;
    }

    if (scene_active_this_frame_ && !scene_streamed_)
        draw_background(command_buffer, render_pass, camera, toolbar, drawable_width, drawable_height, msaa_active);

    if (scene_active_this_frame_ && !scene_streamed_ && !scene_draws_.empty()) {
        const SDL_GPUBufferBinding scene_binding{
            scene_vertex_buffer_, 0U};
        const SDL_GPUBufferBinding image_binding{
            image_vertex_buffer_, 0U};
        SceneDraw::Kind bound_kind = SceneDraw::Kind::vector;
        bool has_bound_kind = false;
        for (const SceneDraw& draw : scene_draws_) {
            if (draw.vertex_count == 0U) continue;
            if (draw.kind == SceneDraw::Kind::resident) {
                draw_resident(command_buffer, render_pass, draw, camera, msaa_active);
                SDL_PushGPUVertexUniformData(command_buffer, 0U, board_matrices.data(), sizeof(board_matrices));
                has_bound_kind = false;
                continue;
            }
            if (!has_bound_kind || draw.kind != bound_kind) {
                if (draw.kind == SceneDraw::Kind::vector) {
                    SDL_BindGPUGraphicsPipeline(render_pass, board_vector_pipeline);
                    SDL_BindGPUVertexBuffers(
                        render_pass, 0U, &scene_binding, 1U);
                } else {
                    SDL_BindGPUGraphicsPipeline(
                        render_pass, board_image_pipeline);
                    SDL_BindGPUVertexBuffers(
                        render_pass, 0U, &image_binding, 1U);
                }
                bound_kind = draw.kind;
                has_bound_kind = true;
            }
            if (draw.kind == SceneDraw::Kind::image) {
                const SDL_GPUTextureSamplerBinding binding{
                    draw.texture, image_sampler_};
                SDL_BindGPUFragmentSamplers(
                    render_pass, 0U, &binding, 1U);
            }
            SDL_DrawGPUPrimitives(
                render_pass, draw.vertex_count, 1U,
                draw.first_vertex, 0U);
            ++stats_.draw_calls;
        }
    }

    if (draft_active_this_frame_ && !draft_gpu_geometry_.empty()) {
        const SDL_GPUBufferBinding draft_binding{
            draft_vertex_buffer_, 0U};
        SDL_BindGPUGraphicsPipeline(render_pass, board_vector_pipeline);
        SDL_BindGPUVertexBuffers(
            render_pass, 0U, &draft_binding, 1U);
        SDL_DrawGPUPrimitives(
            render_pass,
            static_cast<Uint32>(draft_gpu_geometry_.size()),
            1U,
            0U,
            0U);
        ++stats_.draw_calls;
    }

    const auto total_vertices = static_cast<Uint32>(geometry_.size());
    const auto set_scissor = [&](SDL_GPURenderPass* pass,
                                const std::optional<std::array<double, 4>>& clip) {
        SDL_Rect rect{0, 0, static_cast<int>(drawable_width),
            static_cast<int>(drawable_height)};
        if (clip.has_value()) {
            const double sx = static_cast<double>(drawable_width) / viewport.x;
            const double sy = static_cast<double>(drawable_height) / viewport.y;
            const int left = static_cast<int>(std::clamp(std::ceil((*clip)[0] * sx),
                0.0, static_cast<double>(drawable_width)));
            const int top = static_cast<int>(std::clamp(std::ceil((*clip)[1] * sy),
                0.0, static_cast<double>(drawable_height)));
            const int right = static_cast<int>(std::clamp(
                std::floor(((*clip)[0] + (*clip)[2]) * sx),
                static_cast<double>(left), static_cast<double>(drawable_width)));
            const int bottom = static_cast<int>(std::clamp(
                std::floor(((*clip)[1] + (*clip)[3]) * sy),
                static_cast<double>(top), static_cast<double>(drawable_height)));
            rect = {left, top, right - left, bottom - top};
        }
        SDL_SetGPUScissor(pass, &rect);
    };
    const auto draw_geometry_spans = [&](SDL_GPURenderPass* pass,
        SDL_GPUGraphicsPipeline* opaque, SDL_GPUGraphicsPipeline* translucent,
        const Uint32 first, const Uint32 last, const bool tooltip = false) {
        // Text uses normalized device coordinates; geometry needs the camera
        // uniforms and vertex binding restored when drawn over text.
        SDL_PushGPUVertexUniformData(command_buffer, 0U, board_matrices.data(),
            static_cast<Uint32>(sizeof(board_matrices)));
        const SDL_GPUBufferBinding binding{vertex_buffer_, 0U};
        SDL_BindGPUVertexBuffers(pass, 0U, &binding, 1U);
        SDL_GPUGraphicsPipeline* bound_pipeline = nullptr;
        float bound_constant = -1.0F;
        for (std::size_t index = 0U; index < geometry_spans_.size(); ++index) {
            const GeometrySpan& span = geometry_spans_[index];
            const Uint32 start = std::max(span.first_vertex, first);
            const Uint32 end = std::min(last, index + 1U < geometry_spans_.size()
                ? geometry_spans_[index + 1U].first_vertex : total_vertices);
            if (end <= start) continue;
            set_scissor(pass, span.clip);
            SDL_GPUGraphicsPipeline* wanted = span.alpha >= 0.999F ? opaque : translucent;
            if (wanted != bound_pipeline) {
                SDL_BindGPUGraphicsPipeline(pass, wanted);
                bound_pipeline = wanted;
                bound_constant = -1.0F;
            }
            if (wanted == translucent && span.alpha != bound_constant) {
                SDL_SetGPUBlendConstants(pass,
                    SDL_FColor{span.alpha, span.alpha, span.alpha, span.alpha});
                bound_constant = span.alpha;
            }
            SDL_DrawGPUPrimitives(pass, end - start, 1U, start, 0U);
            ++stats_.draw_calls;
            if (tooltip) ++stats_.tooltip_draw_calls;
        }
    };
    draw_geometry_spans(render_pass, opaque_pipeline, translucent_pipeline,
        0U, tooltip_first_vertex_.value_or(interface_vertex_count));
    const auto draw_text_batches = [&] (
        SDL_GPURenderPass* const pass,
        SDL_GPUGraphicsPipeline* const pipeline,
        const std::size_t first, const std::size_t last, const bool tooltip = false) {
        if (first >= last) return;
        constexpr std::array<float, 32> identity_matrices{{
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F, 1.0F,
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F, 1.0F,
        }};
        SDL_PushGPUVertexUniformData(
            command_buffer,
            0U,
            identity_matrices.data(),
            static_cast<Uint32>(sizeof(identity_matrices)));
        const SDL_GPUBufferBinding text_vertex_binding{
            text_vertex_buffer_, 0U};
        const SDL_GPUBufferBinding text_index_binding{
            text_index_buffer_, 0U};
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_BindGPUVertexBuffers(
            pass, 0U, &text_vertex_binding, 1U);
        SDL_BindGPUIndexBuffer(
            pass,
            &text_index_binding,
            SDL_GPU_INDEXELEMENTSIZE_16BIT);
        for (std::size_t index = first; index < last; ++index) {
            const TextBatch& batch = text_batches_[index];
            set_scissor(pass, batch.clip);
            const SDL_GPUTextureSamplerBinding binding{
                batch.texture, text_sampler_};
            SDL_BindGPUFragmentSamplers(
                pass, 0U, &binding, 1U);
            SDL_DrawGPUIndexedPrimitives(
                pass,
                batch.index_count,
                1U,
                batch.first_index,
                batch.vertex_offset,
                0U);
            ++stats_.text_draw_calls;
            if (tooltip) ++stats_.tooltip_draw_calls;
        }
    };
    const auto draw_interface_text = [&](SDL_GPURenderPass* pass,
        SDL_GPUGraphicsPipeline* glyphs, SDL_GPUGraphicsPipeline* opaque,
        SDL_GPUGraphicsPipeline* translucent) {
        const std::size_t normal_text_end = tooltip_first_vertex_.has_value()
            ? tooltip_first_text_batch_ : text_batches_.size();
        draw_text_batches(pass, glyphs, 0U, normal_text_end);
        if (tooltip_first_vertex_.has_value()) {
            // Composite the complete tooltip after every ordinary UI label.
            draw_geometry_spans(pass, opaque, translucent,
                *tooltip_first_vertex_, interface_vertex_count, true);
            draw_text_batches(pass, glyphs, tooltip_first_text_batch_, text_batches_.size(), true);
        }
        if (navigation_overlay) {
            draw_geometry_spans(pass, opaque, translucent,
                interface_vertex_count, total_vertices);
        }
    };
    if (!msaa_active) {
        draw_interface_text(render_pass, glyph_pipeline, opaque_pipeline, translucent_pipeline);
    }
    SDL_EndGPURenderPass(render_pass);
    if (msaa_active) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = msaa_resolve_texture_;
        blit.source.w = drawable_width;
        blit.source.h = drawable_height;
        blit.destination.texture = final_surface;
        blit.destination.w = drawable_width;
        blit.destination.h = drawable_height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command_buffer, &blit);

        // Glyphs are already rasterized with grayscale antialiasing. Drawing
        // them into the multisampled scene would filter their coverage a
        // second time during resolve and make small UI text look soft. Draw
        // text and the final tooltip overlay over the resolved scene at native
        // drawable resolution instead.
        if (!text_vertices_.empty() || tooltip_first_vertex_.has_value() || navigation_overlay) {
            SDL_GPUColorTargetInfo text_target{};
            text_target.texture = final_surface;
            text_target.load_op = SDL_GPU_LOADOP_LOAD;
            text_target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* const text_pass =
                SDL_BeginGPURenderPass(
                    command_buffer, &text_target, 1U, nullptr);
            if (text_pass == nullptr) {
                throw_sdl("GPU text render-pass creation");
            }
            draw_interface_text(text_pass, text_pipeline_direct_,
                pipeline_direct_, blend_pipeline_direct_);
            SDL_EndGPURenderPass(text_pass);
        }
    }
    if (capture_pixel) {
        SDL_GPUBlitInfo present{};
        present.source.texture = final_surface;
        present.source.w = drawable_width;
        present.source.h = drawable_height;
        present.destination.texture = swapchain_texture;
        present.destination.w = drawable_width;
        present.destination.h = drawable_height;
        present.load_op = SDL_GPU_LOADOP_DONT_CARE;
        present.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command_buffer, &present);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command_buffer);
        if (copy == nullptr) throw_sdl("GPU final-frame diagnostic copy pass");
        const SDL_GPUTextureRegion source{final_surface, 0U, 0U,
            (*pixel_readback_request_)[0], (*pixel_readback_request_)[1], 0U, 1U, 1U, 1U};
        const SDL_GPUTextureTransferInfo destination{pixel_readback_transfer_, 0U, 64U, 1U};
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
    }
    const auto command_record_end = std::chrono::steady_clock::now();
    stats_.command_record_milliseconds =
        std::chrono::duration<double, std::milli>(
            command_record_end - command_record_start).count();

    const auto submit_start = std::chrono::steady_clock::now();
    if (capture_pixel) {
        submit_and_wait(device_, command_buffer_guard.release());
        completed_mesh_serial_ = ++submitted_mesh_serial_;
        const auto* mapped = static_cast<const std::uint8_t*>(
            SDL_MapGPUTransferBuffer(device_, pixel_readback_transfer_, false));
        if (mapped == nullptr) throw_sdl("GPU final-frame diagnostic mapping");
        std::array<std::uint8_t, 4> rgba{mapped[0], mapped[1], mapped[2], mapped[3]};
        SDL_UnmapGPUTransferBuffer(device_, pixel_readback_transfer_);
        const auto format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM
            || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) std::swap(rgba[0], rgba[2]);
        pixel_readback_result_ = rgba;
        pixel_readback_position_ = pixel_readback_request_;
    } else if (mesh_arena_.pages() != 0U) {
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer_guard.release());
        if (fence == nullptr) throw_sdl("GPU resident frame submission");
        try { mesh_submissions_.push_back({fence, ++submitted_mesh_serial_}); }
        catch (...) { SDL_ReleaseGPUFence(device_, fence); throw; }
    } else if (!SDL_SubmitGPUCommandBuffer(command_buffer_guard.release())) {
        throw_sdl("GPU command-buffer submission");
    }
    pixel_readback_request_.reset();
    for (const auto& upload : thumbnail_uploads_) {
        if (const auto found = thumbnail_textures_.find(upload.key);
            found != thumbnail_textures_.end()
            && found->second.texture == upload.texture) {
            found->second.uploaded = true;
        }
    }
    const auto submit_end = std::chrono::steady_clock::now();
    stats_.submit_milliseconds =
        std::chrono::duration<double, std::milli>(
            submit_end - submit_start).count();
    stats_.total_cpu_milliseconds =
        std::chrono::duration<double, std::milli>(
            submit_end - frame_start).count();
    stats_.cpu_geometry_live_bytes += scene_batch_geometry_.size() * sizeof(GeometryVertex)
        + scene_stream_draws_.size() * sizeof(SceneDraw)
        + stroke_render_points_.size() * sizeof(Vec2d)
        + stroke_page_vertices_.size() * sizeof(CachedWorldVertex);
    stats_.cpu_geometry_capacity_bytes += scene_batch_geometry_.capacity() * sizeof(GeometryVertex)
        + scene_batch_draws_.capacity() * sizeof(SceneDraw)
        + scene_stream_draws_.capacity() * sizeof(SceneDraw)
        + stroke_render_points_.capacity() * sizeof(Vec2d)
        + stroke_page_vertices_.capacity() * sizeof(CachedWorldVertex);
    stats_.query_scratch_capacity_bytes += stroke_segment_scratch_.capacity() * sizeof(std::uint32_t)
        + visibility_cache_.capacity_bytes() + mesh_changes_.capacity() * sizeof(DocumentChange)
        + resident_scene_ids_.capacity() * sizeof(ObjectId);
    stats_.gpu_geometry_capacity_bytes =
        (scene_vertex_capacity_ + draft_vertex_capacity_ + overlay_vertex_capacity_)
            * sizeof(GeometryVertex) + image_vertex_capacity_ * sizeof(ImageVertex)
            + (scene_stream_quad_ != nullptr ? 6U * sizeof(ImageVertex) : 0U);
    for (const auto& slot : batch_slots_) {
        if (slot.transfer != nullptr) stats_.transfer_capacity_bytes +=
            scene_batch_vertex_count * sizeof(GeometryVertex) + 6U * sizeof(ImageVertex);
        if (slot.vertices != nullptr) stats_.gpu_geometry_capacity_bytes +=
            scene_batch_vertex_count * sizeof(GeometryVertex);
    }
    stats_.cache_bytes = geometry_cache_bytes_;
    stats_.cache_metadata_bytes = geometry_metadata_bytes();
    stats_.evictions = total_evictions_;
    scene_upload_pending_ = false;
    draft_upload_pending_ = false;
    stats_.resident_pages = mesh_arena_.pages();
    stats_.gpu_geometry_capacity_bytes += mesh_arena_.pages() * MeshArena::page_vertices * sizeof(GeometryVertex);
    stats_.cpu_geometry_live_bytes += mesh_upload_vertices_.size() * sizeof(GeometryVertex);
    stats_.cpu_geometry_capacity_bytes += mesh_upload_vertices_.capacity() * sizeof(GeometryVertex)
        + mesh_uploads_.capacity() * sizeof(MeshUpload) + mesh_arena_.capacity_bytes()
        + mesh_retirements_.capacity() * sizeof(MeshRetirement)
        + mesh_submissions_.capacity() * sizeof(MeshSubmission);
    for (const auto& retirement : mesh_retirements_)
        stats_.cpu_geometry_capacity_bytes += retirement.ranges.capacity() * sizeof(MeshRange);
    mesh_uploads_.clear();
    mesh_upload_vertices_.clear();
    if (resident_scene_invalidated_ && !scene_streamed_) scene_valid_ = false;

    return true;
}

void GpuRenderer::request_rendered_pixel(const std::uint32_t x, const std::uint32_t y)
{
    pixel_readback_request_ = std::array{x, y};
    pixel_readback_position_.reset();
    pixel_readback_result_.reset();
}

std::optional<std::array<std::uint8_t, 4>> GpuRenderer::read_rendered_pixel(
    const std::uint32_t x, const std::uint32_t y) const noexcept
{
    if (pixel_readback_position_ != std::optional{std::array{x, y}}) return std::nullopt;
    return pixel_readback_result_;
}

void GpuRenderer::render_scene_batches(
    const Camera& camera, const Document& document, const Toolbar& toolbar,
    const std::uint32_t width, const std::uint32_t height)
{
    ensure_msaa_target(width, height, true);
    if (scene_stream_texture_ == nullptr
        || scene_stream_capacity_width_ != msaa_width_
        || scene_stream_capacity_height_ != msaa_height_) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = msaa_width_;
        info.height = msaa_height_;
        info.layer_count_or_depth = 1U;
        info.num_levels = 1U;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device_, &info);
        if (replacement == nullptr) throw_sdl("GPU streamed scene target creation");
        if (scene_stream_texture_ != nullptr) SDL_ReleaseGPUTexture(device_, scene_stream_texture_);
        scene_stream_texture_ = replacement;
        scene_stream_capacity_width_ = msaa_width_;
        scene_stream_capacity_height_ = msaa_height_;
    }
    if (scene_stream_quad_ == nullptr) {
        const SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_VERTEX,
            static_cast<Uint32>(6U * sizeof(ImageVertex)), 0U};
        scene_stream_quad_ = SDL_CreateGPUBuffer(device_, &info);
        if (scene_stream_quad_ == nullptr) throw_sdl("GPU streamed scene quad creation");
    }
    scene_batch_geometry_.clear();
    scene_batch_geometry_.reserve(scene_batch_vertex_count);
    scene_batch_draws_.clear();
    const bool multisampled = antialiasing_samples_ != 1;
    const auto viewport = camera.viewport();
    const float x = static_cast<float>(2.0 * camera.zoom() / viewport.x);
    const float y = static_cast<float>(-2.0 * camera.zoom() / viewport.y);
    const std::array<float, 32> matrices{{
        x, 0, 0, 0, 0, y, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    const RenderColor clear = mix_color(
        toolbar.previous_theme() == Theme::light
            ? RenderColor{0.82F, 0.84F, 0.88F} : RenderColor{0.025F, 0.03F, 0.045F},
        toolbar.theme() == Theme::light
            ? RenderColor{0.82F, 0.84F, 0.88F} : RenderColor{0.025F, 0.03F, 0.045F},
        smooth_theme_transition(toolbar));
    const float half_width = static_cast<float>(viewport.x / (2.0 * camera.zoom()));
    const float half_height = static_cast<float>(viewport.y / (2.0 * camera.zoom()));
    const float u = static_cast<float>(width) / static_cast<float>(msaa_width_);
    const float v = static_cast<float>(height) / static_cast<float>(msaa_height_);
    constexpr std::array<std::uint8_t, 4> white{255U, 255U, 255U, 255U};
    const std::array<ImageVertex, 6> quad{{
        {{-half_width, -half_height}, white, {0, 0}},
        {{half_width, -half_height}, white, {u, 0}},
        {{half_width, half_height}, white, {u, v}},
        {{-half_width, -half_height}, white, {0, 0}},
        {{half_width, half_height}, white, {u, v}},
        {{-half_width, half_height}, white, {0, v}},
    }};
    const auto flush = [&](const bool last = false) {
        if (scene_batch_draws_.empty()) return;
        BatchSlot& slot = batch_slots_[next_batch_slot_];
        next_batch_slot_ = (next_batch_slot_ + 1U) % batch_slots_.size();
        if (slot.fence != nullptr) {
            if (!SDL_QueryGPUFence(device_, slot.fence)) {
                const auto start = std::chrono::steady_clock::now();
                if (!SDL_WaitForGPUFences(device_, true, &slot.fence, 1U))
                    throw_sdl("GPU batch slot completion");
                ++stats_.batch_reuse_waits;
                stats_.batch_wait_milliseconds += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
            }
            SDL_ReleaseGPUFence(device_, slot.fence);
            slot.fence = nullptr;
        }
        if (slot.vertices == nullptr) {
            const SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_VERTEX,
                static_cast<Uint32>(scene_batch_vertex_count * sizeof(GeometryVertex)), 0U};
            slot.vertices = SDL_CreateGPUBuffer(device_, &info);
            if (slot.vertices == nullptr) throw_sdl("GPU batch slot creation");
        }
        if (slot.transfer == nullptr) {
            const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                static_cast<Uint32>(scene_batch_vertex_count * sizeof(GeometryVertex)
                    + 6U * sizeof(ImageVertex)), 0U};
            slot.transfer = SDL_CreateGPUTransferBuffer(device_, &info);
            if (slot.transfer == nullptr) throw_sdl("GPU batch slot staging creation");
        }
        const Uint32 bytes = static_cast<Uint32>(
            scene_batch_geometry_.size() * sizeof(GeometryVertex));
        if (bytes != 0U || last) {
            void* mapped = SDL_MapGPUTransferBuffer(device_, slot.transfer, false);
            if (mapped == nullptr) throw_sdl("GPU batch staging mapping");
            std::memcpy(mapped, scene_batch_geometry_.data(), bytes);
            if (last) std::memcpy(static_cast<std::byte*>(mapped) + bytes, quad.data(), sizeof(quad));
            SDL_UnmapGPUTransferBuffer(device_, slot.transfer);
        }
        SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
        if (command == nullptr) throw_sdl("GPU scene batch acquisition");
        CommandBufferGuard guard{command};
        if (bytes != 0U) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
            if (copy == nullptr) throw_sdl("GPU scene batch copy pass");
            const SDL_GPUTransferBufferLocation source{slot.transfer, 0U};
            const SDL_GPUBufferRegion destination{slot.vertices, 0U, bytes};
            // Only reuse a slot after its last consumer completes. Avoid
            // cycling allocations proportional to the number of batches.
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            SDL_EndGPUCopyPass(copy);
        }
        if (last) {
            SDL_GPUCopyPass* quad_copy = SDL_BeginGPUCopyPass(command);
            if (quad_copy == nullptr) throw_sdl("GPU scene quad copy pass");
            const SDL_GPUTransferBufferLocation source{slot.transfer, bytes};
            const SDL_GPUBufferRegion destination{scene_stream_quad_, 0U, sizeof(quad)};
            SDL_UploadToGPUBuffer(quad_copy, &source, &destination, true);
            SDL_EndGPUCopyPass(quad_copy);
        }
        SDL_GPUColorTargetInfo target{};
        target.texture = multisampled ? msaa_texture_ : scene_stream_texture_;
        target.clear_color = {clear.red, clear.green, clear.blue, 1.0F};
        target.load_op = stats_.scene_batches == 0U ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        // Keep earlier batches, then resolve only the completed canvas. UI
        // and live previews are drawn separately and never enter this cache.
        target.store_op = multisampled && last
            ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
        if (multisampled && last) {
            target.resolve_texture = scene_stream_texture_;
            target.cycle_resolve_texture = true;
        }
        target.cycle = stats_.scene_batches == 0U;
        SDL_PushGPUVertexUniformData(command, 0U, matrices.data(), sizeof(matrices));
        const auto theme_parameters = board_theme_parameters(toolbar);
        SDL_PushGPUVertexUniformData(command, 1U, &theme_parameters, sizeof(theme_parameters));
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1U, nullptr);
        if (pass == nullptr) throw_sdl("GPU scene batch render pass");
        const SDL_GPUViewport drawable{0, 0, static_cast<float>(width),
            static_cast<float>(height), 0, 1};
        SDL_SetGPUViewport(pass, &drawable);
        if (stats_.scene_batches == 0U)
            draw_background(command, pass, camera, toolbar, width, height, multisampled);
        SceneDraw::Kind bound = SceneDraw::Kind::vector;
        bool have_binding = false;
        for (const SceneDraw& draw : scene_batch_draws_) {
            if (draw.kind == SceneDraw::Kind::resident) {
                draw_resident(command, pass, draw, camera, multisampled);
                SDL_PushGPUVertexUniformData(command, 0U, matrices.data(), sizeof(matrices));
                have_binding = false;
                continue;
            }
            if (!have_binding || bound != draw.kind) {
                SDL_BindGPUGraphicsPipeline(pass,
                    draw.kind == SceneDraw::Kind::vector
                        ? (!multisampled && retained_pipeline_direct_ != nullptr ? retained_pipeline_direct_ : retained_pipeline_)
                        : (!multisampled && image_pipeline_direct_ != nullptr ? image_pipeline_direct_ : image_pipeline_));
                const SDL_GPUBufferBinding binding{
                    draw.kind == SceneDraw::Kind::vector ? slot.vertices : image_vertex_buffer_, 0U};
                SDL_BindGPUVertexBuffers(pass, 0U, &binding, 1U);
                bound = draw.kind;
                have_binding = true;
            }
            if (draw.kind == SceneDraw::Kind::image) {
                const SDL_GPUTextureSamplerBinding binding{draw.texture, image_sampler_};
                SDL_BindGPUFragmentSamplers(pass, 0U, &binding, 1U);
            }
            SDL_DrawGPUPrimitives(pass, draw.vertex_count, 1U, draw.first_vertex, 0U);
            ++stats_.draw_calls;
        }
        SDL_EndGPURenderPass(pass);
        slot.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(guard.release());
        if (slot.fence == nullptr) throw_sdl("GPU batch slot submission");
        ++stats_.scene_batches;
        stats_.peak_scene_batch_vertices = std::max(
            stats_.peak_scene_batch_vertices, scene_batch_geometry_.size());
        stats_.scene_upload_bytes += bytes;
        scene_batch_geometry_.clear();
        scene_batch_draws_.clear();
    };
    const auto append_vertices = [&](const auto& vertices) {
        std::size_t first = 0U;
        while (first < vertices.size()) {
            if (scene_batch_geometry_.size() == scene_batch_vertex_count) flush();
            const std::size_t count = std::min(
                scene_batch_vertex_count - scene_batch_geometry_.size(), vertices.size() - first);
            const std::uint32_t offset = static_cast<std::uint32_t>(scene_batch_geometry_.size());
            for (std::size_t index = first; index < first + count; ++index) {
                if constexpr (std::is_same_v<std::decay_t<decltype(vertices[index])>, CachedWorldVertex>) {
                    const auto& vertex = vertices[index];
                    scene_batch_geometry_.push_back({{
                        static_cast<float>(vertex.position.x - camera.position().x),
                        static_cast<float>(vertex.position.y - camera.position().y)}, vertex.color});
                } else {
                    scene_batch_geometry_.push_back(vertices[index]);
                }
            }
            if (!scene_batch_draws_.empty()
                && scene_batch_draws_.back().kind == SceneDraw::Kind::vector) {
                scene_batch_draws_.back().vertex_count += static_cast<std::uint32_t>(count);
            } else {
                scene_batch_draws_.push_back({SceneDraw::Kind::vector, offset,
                    static_cast<std::uint32_t>(count), nullptr, nullptr});
            }
            first += count;
        }
    };
    scene_total_vertices_ = scene_geometry_.size();
    stats_.rendered_objects = scene_rendered_images_ + scene_rendered_residents_;
    for (const SceneDraw& draw : scene_stream_draws_) {
        if (draw.kind == SceneDraw::Kind::resident) {
            scene_batch_draws_.push_back(draw);
            scene_total_vertices_ += draw.vertex_count;
        } else if (draw.kind == SceneDraw::Kind::image) {
            scene_batch_draws_.push_back(draw);
        } else if (draw.object == nullptr) {
            append_vertices(scene_geometry_);
        } else {
            // Descriptors store document objects, not pointers into the LRU
            // cache. An evicted mesh can be rebuilt without losing its draw.
            auto& cached = cached_geometry_for(*draw.object, document, camera,
                camera.visible_world_bounds());
            std::size_t emitted = 0U;
            if (cached.paged) {
                const auto* stroke = std::get_if<Stroke>(&draw.object->geometry);
                const auto* index = document.stroke_segment_index(draw.object->id);
                if (stroke == nullptr || index == nullptr) throw std::logic_error{"Paged stroke has no geometry index"};
                static_cast<void>(visit_indexed_stroke(*stroke, draw.object->style,
                    cached.detail, camera.zoom(), camera.visible_world_bounds(), *index,
                    [&](const auto page) {
                        emitted += page.size();
                        append_vertices(page);
                        return true;
                    }));
            } else {
                emitted = cached.vertices.size();
                append_vertices(cached.vertices);
            }
            scene_total_vertices_ += emitted;
            if (emitted != 0U) ++stats_.rendered_objects;
        }
    }
    flush(true);
    scene_rendered_objects_ = stats_.rendered_objects;
    stats_.emitted_vertices = scene_total_vertices_ + draft_gpu_geometry_.size()
        + geometry_.size() + image_vertices_.size();
    scene_stream_width_ = width;
    scene_stream_height_ = height;
    scene_stream_batches_ = stats_.scene_batches;
    scene_stream_peak_vertices_ = stats_.peak_scene_batch_vertices;
}

void GpuRenderer::release_batch_slots() noexcept
{
    for (auto& slot : batch_slots_) {
        if (slot.fence != nullptr) SDL_ReleaseGPUFence(device_, slot.fence);
        if (slot.vertices != nullptr) SDL_ReleaseGPUBuffer(device_, slot.vertices);
        if (slot.transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device_, slot.transfer);
        slot = {};
    }
    next_batch_slot_ = 0U;
}

void GpuRenderer::invalidate_document_cache() noexcept
{
    release_mesh_storage();
    object_cache_.clear();
    geometry_recency_.clear();
    visibility_cache_.clear();
    std::vector<ObjectId>{}.swap(resident_scene_ids_);
    geometry_cache_bytes_ = 0U;
    scene_stream_draws_.clear();
    scene_draws_.clear();
    visible_objects_.clear();
    visible_object_ids_.clear();
    scene_valid_ = false;
    scene_stream_width_ = scene_stream_height_ = 0U;
    draft_cache_active_ = false;
}

void GpuRenderer::adopt_decoded_images(ImageDecodeCache images) noexcept
{
    image_decode_cache_ = std::move(images);
}

void GpuRenderer::begin_live_resize() noexcept
{
    if (live_resize_) {
        return;
    }
    live_resize_ = true;
    // Present without waiting for vblank while the user drags an edge; any
    // tearing is imperceptible mid-drag and each frame tracks the cursor.
    if (SDL_WindowSupportsGPUPresentMode(
            device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        static_cast<void>(SDL_SetGPUSwapchainParameters(
            device_,
            window_,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_PRESENTMODE_IMMEDIATE));
    }
}

void GpuRenderer::end_live_resize() noexcept
{
    if (!live_resize_) {
        return;
    }
    live_resize_ = false;
    static_cast<void>(SDL_SetGPUSwapchainParameters(
        device_,
        window_,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        SDL_GPU_PRESENTMODE_VSYNC));
}

bool GpuRenderer::rendering() const noexcept
{
    return rendering_;
}

const std::string& GpuRenderer::diagnostics() const noexcept
{
    return diagnostics_;
}

const RendererStats& GpuRenderer::stats() const noexcept
{
    return stats_;
}

std::optional<std::size_t> GpuRenderer::filename_index_at_x(
    const double screen_x) const noexcept
{
    if (filename_caret_stops_.empty()) {
        return std::nullopt;
    }
    std::size_t best_offset = filename_caret_stops_.front().first;
    double best_distance = std::abs(screen_x
        - filename_caret_stops_.front().second);
    for (const auto& [offset, x] : filename_caret_stops_) {
        const double distance = std::abs(screen_x - x);
        if (distance < best_distance) {
            best_distance = distance;
            best_offset = offset;
        }
    }
    return best_offset;
}

void GpuRenderer::create_geometry_buffers()
{
    static_cast<void>(ensure_vertex_buffer_capacity(
        scene_vertex_buffer_,
        scene_vertex_capacity_,
        initial_scene_vertex_capacity,
        "GPU scene vertex-buffer creation"));
    static_cast<void>(ensure_vertex_buffer_capacity(
        draft_vertex_buffer_,
        draft_vertex_capacity_,
        initial_draft_vertex_capacity,
        "GPU draft vertex-buffer creation"));
    static_cast<void>(ensure_vertex_buffer_capacity(
        vertex_buffer_,
        overlay_vertex_capacity_,
        initial_overlay_vertex_capacity,
        "GPU overlay vertex-buffer creation"));
    ensure_transfer_capacity(initial_transfer_capacity_bytes);
}

bool GpuRenderer::ensure_vertex_buffer_capacity(
    SDL_GPUBuffer*& buffer,
    std::size_t& capacity_vertices,
    const std::size_t required_vertices,
    const std::string_view label)
{
    if (required_vertices > maximum_vertex_count) {
        throw std::runtime_error{
            std::string{label} + " exceeded its vertex budget"};
    }
    if (buffer != nullptr && capacity_vertices >= required_vertices) {
        return false;
    }
    std::size_t grown = std::max<std::size_t>(
        capacity_vertices, 1U);
    while (grown < required_vertices) {
        grown = std::min<std::size_t>(
            maximum_vertex_count, grown * 2U);
    }
    const SDL_GPUBufferCreateInfo buffer_info{
        SDL_GPU_BUFFERUSAGE_VERTEX,
        static_cast<Uint32>(grown * sizeof(GeometryVertex)),
        0U,
    };
    SDL_GPUBuffer* const replacement =
        SDL_CreateGPUBuffer(device_, &buffer_info);
    if (replacement == nullptr) {
        throw_sdl(label);
    }
    if (buffer != nullptr) {
        SDL_ReleaseGPUBuffer(device_, buffer);
    }
    buffer = replacement;
    capacity_vertices = grown;
    return true;
}

void GpuRenderer::ensure_transfer_capacity(
    const std::size_t required_bytes)
{
    if (required_bytes > maximum_transfer_capacity_bytes) {
        throw std::runtime_error{
            "GPU staging arena exceeded its byte budget"};
    }
    if (transfer_buffer_ != nullptr
        && transfer_capacity_bytes_ >= required_bytes) {
        return;
    }
    std::size_t grown = std::max<std::size_t>(
        transfer_capacity_bytes_, 1U);
    while (grown < required_bytes) {
        grown = std::min(
            maximum_transfer_capacity_bytes, grown * 2U);
    }
    const SDL_GPUTransferBufferCreateInfo transfer_info{
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        static_cast<Uint32>(grown),
        0U,
    };
    SDL_GPUTransferBuffer* const replacement =
        SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (replacement == nullptr) {
        throw_sdl("GPU staging arena creation");
    }
    if (transfer_buffer_ != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
    }
    transfer_buffer_ = replacement;
    transfer_capacity_bytes_ = grown;
}

void GpuRenderer::create_background_pipeline()
{
    SDL_GPUShader* vertex = create_embedded_shader(device_, true,
        background_vert_dxil, background_vert_spv, 0U);
    SDL_GPUShader* fragment = nullptr;
    try {
        fragment = create_embedded_shader(device_, false,
            background_frag_dxil, background_frag_spv, 1U);
        background_pipeline_ = create_auxiliary_pipeline(device_, window_, vertex, fragment,
            antialiasing_samples_, false);
        if (antialiasing_samples_ != 1) background_pipeline_direct_ =
            create_auxiliary_pipeline(device_, window_, vertex, fragment, 1, false);
    } catch (...) {
        if (fragment != nullptr) SDL_ReleaseGPUShader(device_, fragment);
        SDL_ReleaseGPUShader(device_, vertex);
        throw;
    }
    SDL_ReleaseGPUShader(device_, fragment);
    SDL_ReleaseGPUShader(device_, vertex);
}

void GpuRenderer::create_retained_pipeline()
{
    SDL_GPUShader* vertex = create_embedded_shader(device_, true,
        retained_vert_dxil, retained_vert_spv, 2U);
    SDL_GPUShader* fragment = nullptr;
    try {
        fragment = create_embedded_shader(device_, false, retained_frag_dxil, retained_frag_spv, 0U);
        retained_pipeline_ = create_auxiliary_pipeline(device_, window_, vertex, fragment, antialiasing_samples_, true);
        if (antialiasing_samples_ != 1) retained_pipeline_direct_ =
            create_auxiliary_pipeline(device_, window_, vertex, fragment, 1, true);
    } catch (...) {
        if (fragment != nullptr) SDL_ReleaseGPUShader(device_, fragment);
        SDL_ReleaseGPUShader(device_, vertex);
        throw;
    }
    SDL_ReleaseGPUShader(device_, fragment);
    SDL_ReleaseGPUShader(device_, vertex);
}

void GpuRenderer::draw_background(SDL_GPUCommandBuffer* command, SDL_GPURenderPass* pass,
    const Camera& camera, const Toolbar& toolbar, const std::uint32_t width,
    const std::uint32_t height, const bool multisampled)
{
    const double theme_amount = board_theme_amount(toolbar);
    const auto grid_color = toolbar.grid_color().has_value()
        ? std::optional<Color>{board_display_color(*toolbar.grid_color(), theme_amount)} : std::nullopt;
    const auto parameters = background_parameters(static_cast<BackgroundGridPattern>(toolbar.background_style()),
        board_display_color(toolbar.background_color(), theme_amount), grid_color, camera.position(), camera.viewport(), camera.zoom(),
        {static_cast<double>(width), static_cast<double>(height)});
    SDL_PushGPUFragmentUniformData(command, 0U, &parameters, sizeof(parameters));
    SDL_BindGPUGraphicsPipeline(pass, !multisampled && background_pipeline_direct_ != nullptr
        ? background_pipeline_direct_ : background_pipeline_);
    SDL_DrawGPUPrimitives(pass, 3U, 1U, 0U, 0U);
    ++stats_.draw_calls;
    ++stats_.background_draw_calls;
}

void GpuRenderer::create_pipeline()
{
    SDL_GPUShader* vertex_shader = create_shader(device_, true);
    SDL_GPUShader* fragment_shader = nullptr;
    try {
        fragment_shader = create_shader(device_, false);

        const SDL_GPUVertexBufferDescription buffer_description{
            0U,
            static_cast<Uint32>(sizeof(GeometryVertex)),
            SDL_GPU_VERTEXINPUTRATE_VERTEX,
            0U,
        };
        const std::array<SDL_GPUVertexAttribute, 2> attributes{{
            {0U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0U},
            {1U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 8U},
        }};

        SDL_GPUColorTargetDescription color_target{};
        color_target.format =
            SDL_GetGPUSwapchainTextureFormat(device_, window_);

        // Use the same preferred quality on D3D12 and Vulkan instead of
        // independently selecting each backend's maximum. Retain 2x as a
        // compatibility fallback for hardware that cannot create 4x targets.
        constexpr auto preferred_sample_count = SDL_GPU_SAMPLECOUNT_4;
        constexpr auto fallback_sample_count = SDL_GPU_SAMPLECOUNT_2;
        antialiasing_samples_ = 1;
        if (antialiasing_enabled_ && SDL_GPUTextureSupportsSampleCount(
                device_, color_target.format, preferred_sample_count)) {
            antialiasing_samples_ = 4;
        } else if (antialiasing_enabled_ && SDL_GPUTextureSupportsSampleCount(
                       device_, color_target.format, fallback_sample_count)) {
            antialiasing_samples_ = 2;
            log::write(
                log::Level::warning,
                "GPU does not support the preferred 4x MSAA drawing target; "
                "using 2x MSAA");
        } else if (antialiasing_enabled_) {
            log::write(
                log::Level::warning,
                "GPU does not support a multisampled drawing target; "
                "antialiasing is disabled");
        }

        // Builds one pipeline over the shared shaders. The blending variant
        // uses a constant factor set per draw span, giving batch-level
        // translucency (soft shadows, glows, scrims) without an alpha vertex
        // attribute. Single-sample variants are retained for hardware that
        // cannot provide a multisampled drawing target.
        const auto make_pipeline = [&](const bool blend_enabled,
                                       const int samples) {
            SDL_GPUColorTargetDescription target = color_target;
            if (blend_enabled) {
                target.blend_state.enable_blend = true;
                target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                target.blend_state.src_color_blendfactor =
                    SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
                target.blend_state.dst_color_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
                target.blend_state.src_alpha_blendfactor =
                    SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
                target.blend_state.dst_alpha_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
            }

            SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
            pipeline_info.vertex_shader = vertex_shader;
            pipeline_info.fragment_shader = fragment_shader;
            pipeline_info.vertex_input_state.vertex_buffer_descriptions =
                &buffer_description;
            pipeline_info.vertex_input_state.num_vertex_buffers = 1U;
            pipeline_info.vertex_input_state.vertex_attributes =
                attributes.data();
            pipeline_info.vertex_input_state.num_vertex_attributes =
                static_cast<Uint32>(attributes.size());
            pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pipeline_info.rasterizer_state.front_face =
                SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            pipeline_info.rasterizer_state.enable_depth_clip = true;
            pipeline_info.multisample_state.sample_count =
                gpu_sample_count(samples);
            pipeline_info.target_info.color_target_descriptions = &target;
            pipeline_info.target_info.num_color_targets = 1U;

            SDL_GPUGraphicsPipeline* const created =
                SDL_CreateGPUGraphicsPipeline(device_, &pipeline_info);
            if (created == nullptr) {
                throw_sdl("GPU graphics-pipeline creation");
            }
            return created;
        };

        pipeline_ = make_pipeline(false, antialiasing_samples_);
        blend_pipeline_ = make_pipeline(true, antialiasing_samples_);
        if (antialiasing_samples_ != 1) {
            pipeline_direct_ = make_pipeline(false, 1);
            blend_pipeline_direct_ = make_pipeline(true, 1);
        }
    } catch (...) {
        SDL_ReleaseGPUShader(device_, vertex_shader);
        if (fragment_shader != nullptr) {
            SDL_ReleaseGPUShader(device_, fragment_shader);
        }
        throw;
    }

    SDL_ReleaseGPUShader(device_, vertex_shader);
    SDL_ReleaseGPUShader(device_, fragment_shader);
}

bool GpuRenderer::ensure_image_vertex_buffer_capacity(
    const std::size_t required_vertices)
{
    if (required_vertices > maximum_vertex_count) {
        throw std::runtime_error{"GPU image geometry exceeded its vertex budget"};
    }
    if (image_vertex_buffer_ != nullptr
        && image_vertex_capacity_ >= required_vertices) {
        return false;
    }
    std::size_t grown = std::max<std::size_t>(image_vertex_capacity_, 4'096U);
    while (grown < required_vertices) {
        grown = std::min<std::size_t>(maximum_vertex_count, grown * 2U);
    }
    const SDL_GPUBufferCreateInfo info{
        SDL_GPU_BUFFERUSAGE_VERTEX,
        static_cast<Uint32>(grown * sizeof(ImageVertex)),
        0U,
    };
    SDL_GPUBuffer* const replacement = SDL_CreateGPUBuffer(device_, &info);
    if (replacement == nullptr) throw_sdl("GPU image vertex-buffer creation");
    if (image_vertex_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, image_vertex_buffer_);
    }
    image_vertex_buffer_ = replacement;
    image_vertex_capacity_ = grown;
    return true;
}

void GpuRenderer::create_image_resources()
{
    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    image_sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
    if (image_sampler_ == nullptr) throw_sdl("Image sampler creation");
    static_cast<void>(ensure_image_vertex_buffer_capacity(4'096U));

    SDL_GPUShader* const vertex_shader = create_image_shader(device_, true);
    SDL_GPUShader* fragment_shader = nullptr;
    try {
        fragment_shader = create_image_shader(device_, false);
        const SDL_GPUVertexBufferDescription buffer_description{
            0U,
            static_cast<Uint32>(sizeof(ImageVertex)),
            SDL_GPU_VERTEXINPUTRATE_VERTEX,
            0U,
        };
        const std::array<SDL_GPUVertexAttribute, 3U> attributes{{
            {0U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0U},
            {1U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 8U},
            {2U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12U},
        }};
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        target.blend_state.enable_blend = true;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        const auto make_pipeline = [&](const int samples) {
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vertex_shader;
            info.fragment_shader = fragment_shader;
            info.vertex_input_state.vertex_buffer_descriptions =
                &buffer_description;
            info.vertex_input_state.num_vertex_buffers = 1U;
            info.vertex_input_state.vertex_attributes = attributes.data();
            info.vertex_input_state.num_vertex_attributes =
                static_cast<Uint32>(attributes.size());
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.multisample_state.sample_count = gpu_sample_count(samples);
            info.target_info.color_target_descriptions = &target;
            info.target_info.num_color_targets = 1U;
            SDL_GPUGraphicsPipeline* result =
                SDL_CreateGPUGraphicsPipeline(device_, &info);
            if (result == nullptr) throw_sdl("Image graphics-pipeline creation");
            return result;
        };
        image_pipeline_ = make_pipeline(antialiasing_samples_);
        if (antialiasing_samples_ != 1) image_pipeline_direct_ = make_pipeline(1);
    } catch (...) {
        SDL_ReleaseGPUShader(device_, vertex_shader);
        if (fragment_shader != nullptr) SDL_ReleaseGPUShader(device_, fragment_shader);
        throw;
    }
    SDL_ReleaseGPUShader(device_, vertex_shader);
    SDL_ReleaseGPUShader(device_, fragment_shader);
}

GpuRenderer::CachedImage& GpuRenderer::ensure_image_texture(
    const std::shared_ptr<const ImageAsset>& asset)
{
    if (!asset) throw std::runtime_error{"Image placement has no asset"};
    if (auto found = image_textures_.find(asset->id);
        found != image_textures_.end()) {
        found->second.owner = asset;
        found->second.last_used_frame = stats_.frame;
        return found->second;
    }

    std::shared_ptr<const DecodedImage> decoded = image_decode_cache_.find(asset->id);
    if (!decoded) {
        decoded = std::make_shared<DecodedImage>(decode_image_rgba(asset->png));
        ++stats_.image_decodes;
        if (decoded->width != asset->pixel_width
            || decoded->height != asset->pixel_height) {
            throw std::runtime_error{"Image asset dimensions changed during decode"};
        }
        image_decode_cache_.insert(asset->id, decoded);
    }

    CachedImage cached;
    cached.owner = asset;
    cached.last_used_frame = stats_.frame;
    constexpr std::size_t alignment = 512U;
    for (std::uint32_t y = 0U; y < decoded->height; y += image_tile_extent) {
        for (std::uint32_t x = 0U; x < decoded->width; x += image_tile_extent) {
            const std::uint32_t width =
                std::min(image_tile_extent, decoded->width - x);
            const std::uint32_t height =
                std::min(image_tile_extent, decoded->height - y);
            const std::uint32_t maximum = std::max(width, height);
            const std::uint32_t levels = 1U + static_cast<std::uint32_t>(
                std::floor(std::log2(static_cast<double>(maximum))));
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER
                | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = width;
            info.height = height;
            info.layer_count_or_depth = 1U;
            info.num_levels = levels;
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
            if (texture == nullptr) {
                for (const ImageTile& tile : cached.tiles) {
                    SDL_ReleaseGPUTexture(device_, tile.texture);
                }
                throw_sdl("Board image texture creation");
            }
            const std::size_t aligned =
                (image_upload_pixels_.size() + alignment - 1U)
                / alignment * alignment;
            image_upload_pixels_.resize(aligned, 0U);
            const std::size_t data_offset = image_upload_pixels_.size();
            const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
            image_upload_pixels_.resize(
                data_offset + row_bytes * static_cast<std::size_t>(height));
            for (std::uint32_t row = 0U; row < height; ++row) {
                const std::size_t source =
                    (static_cast<std::size_t>(y + row) * decoded->width + x) * 4U;
                std::memcpy(
                    image_upload_pixels_.data() + data_offset
                        + static_cast<std::size_t>(row) * row_bytes,
                    decoded->rgba.data() + source,
                    row_bytes);
            }
            image_uploads_.push_back({
                texture,
                static_cast<std::uint32_t>(data_offset),
                width,
                height,
                levels,
            });
            cached.tiles.push_back({texture, x, y, width, height, levels});
            cached.gpu_bytes += static_cast<std::size_t>(width) * height * 4U
                * (levels > 1U ? 4U : 3U) / 3U;
        }
    }
    image_texture_bytes_ += cached.gpu_bytes;
    return image_textures_.emplace(asset->id, std::move(cached)).first->second;
}

void GpuRenderer::append_image_draws(
    const Image& image,
    const Camera& camera)
{
    CachedImage& cached = ensure_image_texture(image.asset);
    const Vec2d position = camera.position();
    const auto relative = [&](const double x, const double y) {
        return std::array<float, 2>{
            static_cast<float>(x - position.x),
            static_cast<float>(y - position.y),
        };
    };
    const std::array<std::uint8_t, 4U> white{255U, 255U, 255U, 255U};
    const double full_width = static_cast<double>(image.asset->pixel_width);
    const double full_height = static_cast<double>(image.asset->pixel_height);
    for (const ImageTile& tile : cached.tiles) {
        if (image_vertices_.size() + 6U > maximum_vertex_count) return;
        const double u0 = static_cast<double>(tile.pixel_x) / full_width;
        const double v0 = static_cast<double>(tile.pixel_y) / full_height;
        const double u1 = static_cast<double>(tile.pixel_x + tile.pixel_width)
            / full_width;
        const double v1 = static_cast<double>(tile.pixel_y + tile.pixel_height)
            / full_height;
        const double x0 = image.first.x + (image.second.x - image.first.x) * u0;
        const double y0 = image.first.y + (image.second.y - image.first.y) * v0;
        const double x1 = image.first.x + (image.second.x - image.first.x) * u1;
        const double y1 = image.first.y + (image.second.y - image.first.y) * v1;
        const std::uint32_t first = static_cast<std::uint32_t>(image_vertices_.size());
        image_vertices_.insert(image_vertices_.end(), {
            {relative(x0, y0), white, {0.0F, 0.0F}},
            {relative(x1, y0), white, {1.0F, 0.0F}},
            {relative(x1, y1), white, {1.0F, 1.0F}},
            {relative(x0, y0), white, {0.0F, 0.0F}},
            {relative(x1, y1), white, {1.0F, 1.0F}},
            {relative(x0, y1), white, {0.0F, 1.0F}},
        });
        scene_draws_.push_back({SceneDraw::Kind::image, first, 6U, tile.texture});
    }
}

void GpuRenderer::prune_image_cache()
{
    while (!image_textures_.empty()) {
        auto victim = std::ranges::min_element(
            image_textures_, {}, [](const auto& entry) {
                return entry.second.last_used_frame;
            });
        if (victim == image_textures_.end()) return;
        const bool stale = victim->second.last_used_frame + 600U < stats_.frame;
        if (!stale && image_texture_bytes_ <= image_texture_budget) return;
        if (victim->second.last_used_frame == stats_.frame) return;
        for (const ImageTile& tile : victim->second.tiles) {
            SDL_ReleaseGPUTexture(device_, tile.texture);
        }
        image_texture_bytes_ -= victim->second.gpu_bytes;
        image_textures_.erase(victim);
        ++image_texture_evictions_;
    }
}

void GpuRenderer::ensure_msaa_target(
    const std::uint32_t width, const std::uint32_t height, const bool force)
{
    if ((!force && antialiasing_samples_ == 1) || width == 0U || height == 0U) {
        return;
    }
    if (msaa_texture_ != nullptr
        && msaa_resolve_texture_ != nullptr
        && msaa_width_ >= width
        && msaa_height_ >= height) {
        return;
    }

    // Leave some headroom so an outward resize only replaces these resources
    // occasionally. The current drawable rectangle is selected with the
    // render-pass viewport and copied 1:1 after resolve.
    const auto padded_extent = [](const std::uint32_t required) {
        constexpr std::uint32_t divisor = 4U;
        const std::uint32_t padding = required / divisor;
        if (required > std::numeric_limits<std::uint32_t>::max() - padding) {
            return required;
        }
        return required + padding;
    };
    const std::uint32_t capacity_width = padded_extent(width);
    const std::uint32_t capacity_height = padded_extent(height);

    SDL_GPUTextureCreateInfo create_info{};
    create_info.type = SDL_GPU_TEXTURETYPE_2D;
    create_info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    create_info.width = capacity_width;
    create_info.height = capacity_height;
    create_info.layer_count_or_depth = 1U;
    create_info.num_levels = 1U;
    create_info.sample_count = gpu_sample_count(antialiasing_samples_);
    create_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    const auto create_target_pair = [&](const std::uint32_t target_width,
                                        const std::uint32_t target_height) {
        create_info.width = target_width;
        create_info.height = target_height;
        create_info.sample_count = gpu_sample_count(antialiasing_samples_);
        create_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        if (antialiasing_samples_ == 1) create_info.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
        SDL_GPUTexture* const msaa =
            SDL_CreateGPUTexture(device_, &create_info);
        if (msaa == nullptr) {
            return std::pair<SDL_GPUTexture*, SDL_GPUTexture*>{
                nullptr, nullptr};
        }
        create_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        create_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
            | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        SDL_GPUTexture* const resolve =
            SDL_CreateGPUTexture(device_, &create_info);
        if (resolve == nullptr) {
            SDL_ReleaseGPUTexture(device_, msaa);
            return std::pair<SDL_GPUTexture*, SDL_GPUTexture*>{
                nullptr, nullptr};
        }
        return std::pair{msaa, resolve};
    };

    auto replacement = create_target_pair(capacity_width, capacity_height);
    bool allocated_with_margin = replacement.first != nullptr;
    if (replacement.first == nullptr
        && (capacity_width != width || capacity_height != height)) {
        // A window can legitimately sit on the device texture-size limit.
        // The requested drawable size is known to be usable, even if the
        // growth margin is not, so fall back to an exact allocation.
        replacement = create_target_pair(width, height);
    }
    if (replacement.first == nullptr) {
        throw_sdl("GPU MSAA render/resolve target creation");
    }

    if (msaa_texture_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, msaa_texture_);
    }
    if (msaa_resolve_texture_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, msaa_resolve_texture_);
    }
    msaa_texture_ = replacement.first;
    msaa_resolve_texture_ = replacement.second;
    msaa_width_ = allocated_with_margin ? capacity_width : width;
    msaa_height_ = allocated_with_margin ? capacity_height : height;
}

void GpuRenderer::create_text_resources()
{
    if (!TTF_Init()) {
        throw_sdl("Text library initialization");
    }

    SDL_IOStream* const font_stream = SDL_IOFromConstMem(
        assets::inter_regular,
        assets::inter_regular_size);
    if (font_stream == nullptr) {
        throw_sdl("Embedded font stream creation");
    }
    font_ = TTF_OpenFontIO(font_stream, true, 16.0F);
    if (font_ == nullptr) {
        throw_sdl("Embedded Inter Regular font opening");
    }
    TTF_SetFontHinting(font_, TTF_HINTING_NORMAL);

    SDL_IOStream* const secondary_font_stream = SDL_IOFromConstMem(
        assets::inter_regular,
        assets::inter_regular_size);
    if (secondary_font_stream == nullptr) {
        throw_sdl("Embedded secondary font stream creation");
    }
    secondary_font_ = TTF_OpenFontIO(
        secondary_font_stream, true, 14.0F);
    if (secondary_font_ == nullptr) {
        throw_sdl("Embedded Inter secondary font opening");
    }
    TTF_SetFontHinting(secondary_font_, TTF_HINTING_NORMAL);

    SDL_IOStream* const bold_font_stream = SDL_IOFromConstMem(
        assets::inter_medium,
        assets::inter_medium_size);
    if (bold_font_stream == nullptr) {
        throw_sdl("Embedded bold font stream creation");
    }
    bold_font_ = TTF_OpenFontIO(bold_font_stream, true, 16.0F);
    if (bold_font_ == nullptr) {
        throw_sdl("Embedded Inter Medium font opening");
    }
    TTF_SetFontHinting(bold_font_, TTF_HINTING_NORMAL);

    SDL_IOStream* const title_font_stream = SDL_IOFromConstMem(
        assets::inter_semibold,
        assets::inter_semibold_size);
    if (title_font_stream == nullptr) {
        throw_sdl("Embedded title font stream creation");
    }
    title_font_ = TTF_OpenFontIO(title_font_stream, true, 29.0F);
    if (title_font_ == nullptr) {
        throw_sdl("Embedded Inter Semibold font opening");
    }
    TTF_SetFontHinting(title_font_, TTF_HINTING_NORMAL);

    SDL_IOStream* const document_font_stream = SDL_IOFromConstMem(
        assets::inter_medium, assets::inter_medium_size);
    if (document_font_stream == nullptr) throw_sdl("Document font stream creation");
    document_font_ = TTF_OpenFontIO(document_font_stream, true, 14.0F);
    if (document_font_ == nullptr) throw_sdl("Document font opening");
    TTF_SetFontHinting(document_font_, TTF_HINTING_NORMAL);

    SDL_IOStream* const caption_font_stream = SDL_IOFromConstMem(
        assets::inter_regular, assets::inter_regular_size);
    if (caption_font_stream == nullptr) throw_sdl("Caption font stream creation");
    caption_font_ = TTF_OpenFontIO(caption_font_stream, true, 12.0F);
    if (caption_font_ == nullptr) throw_sdl("Caption font opening");
    TTF_SetFontHinting(caption_font_, TTF_HINTING_NORMAL);

    text_engine_ = TTF_CreateGPUTextEngine(device_);
    if (text_engine_ == nullptr) {
        throw_sdl("GPU text engine creation");
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    text_sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
    if (text_sampler_ == nullptr) {
        throw_sdl("Text sampler creation");
    }

    const SDL_GPUBufferCreateInfo vertex_info{
        SDL_GPU_BUFFERUSAGE_VERTEX,
        static_cast<Uint32>(sizeof(TextVertex) * maximum_text_vertices),
        0U,
    };
    text_vertex_buffer_ = SDL_CreateGPUBuffer(device_, &vertex_info);
    if (text_vertex_buffer_ == nullptr) {
        throw_sdl("Text vertex-buffer creation");
    }
    const SDL_GPUBufferCreateInfo index_info{
        SDL_GPU_BUFFERUSAGE_INDEX,
        static_cast<Uint32>(sizeof(std::uint16_t) * maximum_text_indices),
        0U,
    };
    text_index_buffer_ = SDL_CreateGPUBuffer(device_, &index_info);
    if (text_index_buffer_ == nullptr) {
        throw_sdl("Text index-buffer creation");
    }
    const SDL_GPUTransferBufferCreateInfo transfer_info{
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        static_cast<Uint32>(
            sizeof(TextVertex) * maximum_text_vertices
            + sizeof(std::uint16_t) * maximum_text_indices),
        0U,
    };
    text_transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
    if (text_transfer_buffer_ == nullptr) {
        throw_sdl("Text transfer-buffer creation");
    }

    SDL_GPUShader* const vertex_shader = create_text_shader(device_, true);
    SDL_GPUShader* fragment_shader = nullptr;
    try {
        fragment_shader = create_text_shader(device_, false);
        const SDL_GPUVertexBufferDescription buffer_description{
            0U,
            static_cast<Uint32>(sizeof(TextVertex)),
            SDL_GPU_VERTEXINPUTRATE_VERTEX,
            0U,
        };
        const std::array<SDL_GPUVertexAttribute, 3> attributes{{
            {0U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0U},
            {1U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 8U},
            {2U, 0U, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12U},
        }};
        SDL_GPUColorTargetDescription color_target{};
        color_target.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        color_target.blend_state.enable_blend = true;
        color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        color_target.blend_state.src_color_blendfactor =
            SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color_target.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color_target.blend_state.src_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color_target.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = vertex_shader;
        pipeline_info.fragment_shader = fragment_shader;
        pipeline_info.vertex_input_state.vertex_buffer_descriptions =
            &buffer_description;
        pipeline_info.vertex_input_state.num_vertex_buffers = 1U;
        pipeline_info.vertex_input_state.vertex_attributes = attributes.data();
        pipeline_info.vertex_input_state.num_vertex_attributes =
            static_cast<Uint32>(attributes.size());
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipeline_info.multisample_state.sample_count =
            gpu_sample_count(antialiasing_samples_);
        pipeline_info.target_info.color_target_descriptions = &color_target;
        pipeline_info.target_info.num_color_targets = 1U;
        text_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline_info);
        if (text_pipeline_ == nullptr) {
            throw_sdl("Text graphics-pipeline creation");
        }
        if (antialiasing_samples_ != 1) {
            pipeline_info.multisample_state.sample_count =
                SDL_GPU_SAMPLECOUNT_1;
            text_pipeline_direct_ =
                SDL_CreateGPUGraphicsPipeline(device_, &pipeline_info);
            if (text_pipeline_direct_ == nullptr) {
                throw_sdl("Direct text graphics-pipeline creation");
            }
        }
    } catch (...) {
        SDL_ReleaseGPUShader(device_, vertex_shader);
        if (fragment_shader != nullptr) {
            SDL_ReleaseGPUShader(device_, fragment_shader);
        }
        throw;
    }
    SDL_ReleaseGPUShader(device_, vertex_shader);
    SDL_ReleaseGPUShader(device_, fragment_shader);
}

SDL_GPUTexture* GpuRenderer::ensure_thumbnail_texture(
    const std::shared_ptr<const BoardPreview>& preview)
{
    if (preview == nullptr || !preview->has_content) {
        return nullptr;
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(BoardPreview::pixel_width)
        * BoardPreview::pixel_height;
    const std::size_t rgba_bytes = pixel_count * 4U;
    if (preview->rgba.size() != rgba_bytes) {
        return nullptr;
    }

    const BoardPreview* const key = preview.get();
    auto found = thumbnail_textures_.find(key);
    if (found != thumbnail_textures_.end()) {
        const auto owner = found->second.owner.lock();
        if (owner == nullptr || owner.get() != key) {
            SDL_ReleaseGPUTexture(device_, found->second.texture);
            thumbnail_textures_.erase(found);
            found = thumbnail_textures_.end();
        }
    }
    if (found == thumbnail_textures_.end()) {
        SDL_GPUTextureCreateInfo texture_info{};
        texture_info.type = SDL_GPU_TEXTURETYPE_2D;
        texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texture_info.width = BoardPreview::pixel_width;
        texture_info.height = BoardPreview::pixel_height;
        texture_info.layer_count_or_depth = 1U;
        texture_info.num_levels = 1U;
        texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* const texture =
            SDL_CreateGPUTexture(device_, &texture_info);
        if (texture == nullptr) {
            throw_sdl("Home thumbnail texture creation");
        }
        found = thumbnail_textures_.emplace(
            key,
            ThumbnailTexture{
                .texture = texture,
                .owner = preview,
                .last_used_frame = stats_.frame,
                .uploaded = false,
            }).first;
    }
    found->second.last_used_frame = stats_.frame;

    const bool upload_already_queued = std::ranges::any_of(
        thumbnail_uploads_,
        [key](const ThumbnailUpload& upload) {
            return upload.key == key;
        });
    if (!found->second.uploaded && !upload_already_queued) {
        const std::size_t first_byte = thumbnail_upload_pixels_.size();
        if (first_byte > std::numeric_limits<std::uint32_t>::max()
            || rgba_bytes
                > std::numeric_limits<std::uint32_t>::max() - first_byte) {
            throw std::runtime_error{
                "Home thumbnail upload exceeded its byte budget"};
        }
        thumbnail_upload_pixels_.insert(
            thumbnail_upload_pixels_.end(),
            preview->rgba.begin(),
            preview->rgba.end());
        thumbnail_uploads_.push_back({
            found->second.texture,
            key,
            static_cast<std::uint32_t>(first_byte),
        });
    }
    return found->second.texture;
}

void GpuRenderer::queue_home_thumbnail(
    const std::shared_ptr<const BoardPreview>& preview,
    const UiRect bounds,
    const std::array<float, 4> color)
{
    if (bounds.width <= 1.0 || bounds.height <= 1.0
        || color[3] <= 0.004F
        || text_vertices_.size() + 4U > maximum_text_vertices
        || text_indices_.size() + 6U > maximum_text_indices) {
        return;
    }
    SDL_GPUTexture* const texture = ensure_thumbnail_texture(preview);
    if (texture == nullptr) {
        return;
    }

    const auto to_ndc = [&](const double x, const double y) {
        return std::array<float, 2>{
            static_cast<float>(2.0 * x / text_viewport_width_ - 1.0),
            static_cast<float>(1.0 - 2.0 * y / text_viewport_height_),
        };
    };
    const auto vertex_offset =
        static_cast<std::int32_t>(text_vertices_.size());
    const auto first_index =
        static_cast<std::uint32_t>(text_indices_.size());
    const auto packed = packed_color(color);
    text_vertices_.push_back({
        to_ndc(bounds.x, bounds.y), packed, {0.0F, 0.0F}});
    text_vertices_.push_back({
        to_ndc(bounds.x + bounds.width, bounds.y), packed, {1.0F, 0.0F}});
    text_vertices_.push_back({
        to_ndc(
            bounds.x + bounds.width,
            bounds.y + bounds.height),
        packed,
        {1.0F, 1.0F}});
    text_vertices_.push_back({
        to_ndc(bounds.x, bounds.y + bounds.height),
        packed,
        {0.0F, 1.0F}});
    text_indices_.insert(
        text_indices_.end(), {0U, 1U, 2U, 0U, 2U, 3U});
    text_batches_.push_back({
        texture,
        first_index,
        vertex_offset,
        6U,
    });
}

TTF_Font* GpuRenderer::text_font(const TextStyle style) const noexcept
{
    switch (style) {
    case TextStyle::secondary: return secondary_font_;
    case TextStyle::bold: return bold_font_;
    case TextStyle::title: return title_font_;
    case TextStyle::document: return document_font_;
    case TextStyle::caption: return caption_font_;
    default: return font_;
    }
}

Vec2d GpuRenderer::tooltip_text_extent(const Toolbar& toolbar, const UiControl& control)
{
    const double width = measure_text_width(control.tooltip);
    const double scale = toolbar.scale();
    const double wrap_width = std::min(480.0 * scale,
        toolbar.viewport_width() - 32.0 * scale) - 24.0 * scale;
    if (control.action != UiAction::rename_board || width <= wrap_width) return {width, 0.0};
    int measured_width = 0;
    int measured_height = 0;
    if (!TTF_GetStringSizeWrapped(font_, control.tooltip.data(), control.tooltip.size(),
            std::max(1, static_cast<int>(std::floor(wrap_width / text_geometry_scale_))),
            &measured_width, &measured_height)) {
        throw_sdl("Filename tooltip measurement");
    }
    return {wrap_width, static_cast<double>(measured_height) * text_geometry_scale_};
}

void GpuRenderer::queue_text(
    const std::string_view text,
    const double x,
    const double y,
    const std::array<float, 4> color,
    const TextStyle style,
    const double wrap_width)
{
    if (text.empty()) return;
    if (color[3] <= 0.004F) return;
    std::string key;
    key.reserve(text.size() + 1U);
    key.push_back(text_style_key(style));
    const int wrap_pixels = wrap_width > 0.0
        ? std::max(1, static_cast<int>(std::floor(wrap_width / text_geometry_scale_))) : 0;
    key.append(std::to_string(wrap_pixels));
    key.push_back('|');
    key.append(text);
    auto found = text_cache_.find(key);
    if (found == text_cache_.end()) {
        if (text_cache_.size() >= maximum_cached_texts) {
            const auto oldest = std::ranges::min_element(
                text_cache_,
                {},
                [](const auto& entry) {
                    return entry.second.last_used_frame;
                });
            if (oldest != text_cache_.end()) {
                TTF_DestroyText(oldest->second.text);
                text_cache_.erase(oldest);
            }
        }
        TTF_Font* const selected_font = text_font(style);
        TTF_Text* const created = TTF_CreateText(
            text_engine_, selected_font, text.data(), text.size());
        if (created == nullptr) {
            throw_sdl("Text shaping");
        }
        if (wrap_pixels > 0 && !TTF_SetTextWrapWidth(created, wrap_pixels)) {
            TTF_DestroyText(created);
            throw_sdl("Filename tooltip wrapping");
        }
        found = text_cache_.emplace(
            key,
            CachedText{
                .text = created,
                .last_used_frame = stats_.frame,
            }).first;
    }
    found->second.last_used_frame = stats_.frame;

    for (TTF_GPUAtlasDrawSequence* sequence =
             TTF_GetGPUTextDrawData(found->second.text);
         sequence != nullptr;
         sequence = sequence->next) {
        if (sequence->atlas_texture == nullptr
            || sequence->num_vertices <= 0
            || sequence->num_indices <= 0) {
            continue;
        }
        if (text_vertices_.size()
                + static_cast<std::size_t>(sequence->num_vertices)
                > maximum_text_vertices
            || text_indices_.size()
                + static_cast<std::size_t>(sequence->num_indices)
                > maximum_text_indices) {
            return;
        }
        for (int index = 0; index < sequence->num_indices; ++index) {
            if (sequence->indices[index] < 0
                || sequence->indices[index]
                    > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error{
                    "Text batch exceeded its 16-bit index range"};
            }
        }
        const auto vertex_offset = static_cast<std::int32_t>(
            text_vertices_.size());
        const auto first_index = static_cast<std::uint32_t>(
            text_indices_.size());
        const double snapped_x = std::round(x / text_geometry_scale_)
            * text_geometry_scale_;
        const double snapped_y = std::round(y / text_geometry_scale_)
            * text_geometry_scale_;
        for (int index = 0; index < sequence->num_vertices; ++index) {
            const double screen_x = snapped_x
                + sequence->xy[index].x * text_geometry_scale_;
            const double screen_y = snapped_y
                - sequence->xy[index].y * text_geometry_scale_;
            text_vertices_.push_back({
                {
                    static_cast<float>(
                        2.0 * screen_x / text_viewport_width_ - 1.0),
                    static_cast<float>(
                        1.0 - 2.0 * screen_y / text_viewport_height_),
                },
                packed_color(color),
                {sequence->uv[index].x, sequence->uv[index].y},
            });
        }
        for (int index = 0; index < sequence->num_indices; ++index) {
            text_indices_.push_back(
                static_cast<std::uint16_t>(sequence->indices[index]));
        }
        text_batches_.push_back({
            sequence->atlas_texture,
            first_index,
            vertex_offset,
            static_cast<std::uint32_t>(sequence->num_indices),
            text_clip_,
        });
    }
}

double GpuRenderer::measure_text_width(
    const std::string_view text,
    const TextStyle style)
{
    if (text.empty()) {
        return 0.0;
    }
    std::string key;
    key.reserve(text.size() + 1U);
    key.push_back(text_style_key(style));
    key.append(text);
    if (const auto found = text_width_cache_.find(key);
        found != text_width_cache_.end()) {
        return found->second;
    }

    TTF_Font* const selected_font = text_font(style);
    int width = 0;
    int height = 0;
    if (!TTF_GetStringSize(
            selected_font, text.data(), text.size(), &width, &height)) {
        throw_sdl("Text measurement");
    }
    static_cast<void>(height);
    const double measured =
        static_cast<double>(width) * text_geometry_scale_;
    if (text_width_cache_.size() >= 8'192U) {
        text_width_cache_.clear();
    }
    text_width_cache_.emplace(std::move(key), measured);
    return measured;
}

std::string GpuRenderer::fit_text_to_width(
    const std::string_view text,
    const double maximum_width,
    const TextStyle style)
{
    if (text.empty()) {
        return {};
    }
    const auto pixel_budget = static_cast<long long>(std::max(
        0.0,
        std::floor(maximum_width / text_geometry_scale_)));
    std::string key;
    key.reserve(text.size() + 24U);
    key.push_back(text_style_key(style));
    key.append(std::to_string(pixel_budget));
    key.push_back('|');
    key.append(text);
    if (const auto found = fitted_text_cache_.find(key);
        found != fitted_text_cache_.end()) {
        return found->second;
    }

    std::string fitted{text};
    if (measure_text_width(fitted, style) > maximum_width) {
        const auto previous_codepoint = [](const std::string& value,
                                           std::size_t offset) {
            if (offset == 0U) return std::size_t{0U};
            --offset;
            while (offset > 0U
                   && (static_cast<unsigned char>(value[offset]) & 0xC0U)
                       == 0x80U) {
                --offset;
            }
            return offset;
        };
        bool found_fit = false;
        while (!fitted.empty()) {
            fitted.erase(previous_codepoint(fitted, fitted.size()));
            const std::string candidate = fitted + "...";
            if (measure_text_width(candidate, style) <= maximum_width) {
                fitted = candidate;
                found_fit = true;
                break;
            }
        }
        if (!found_fit) {
            fitted = "...";
        }
    }

    if (fitted_text_cache_.size() >= 4'096U) {
        fitted_text_cache_.clear();
    }
    fitted_text_cache_.emplace(std::move(key), fitted);
    return fitted;
}

double GpuRenderer::prepare_text_frame(
    const double viewport_width,
    const double viewport_height,
    const double scale)
{
    text_vertices_.clear();
    text_indices_.clear();
    text_batches_.clear();
    text_clip_.reset();
    text_viewport_width_ = viewport_width;
    text_viewport_height_ = viewport_height;

    int pixel_width = 0;
    int pixel_height = 0;
    double pixel_density = 1.0;
    if (SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height)
        && pixel_height > 0) {
        pixel_density = std::max(
            1.0,
            static_cast<double>(pixel_height) / text_viewport_height_);
    }
    const double previous_geometry_scale = text_geometry_scale_;
    text_geometry_scale_ = 1.0 / pixel_density;
    constexpr double base_text_size = 16.0;
    const double desired_raster_size = std::max(
        text_raster_size_quantum,
        std::round(
            base_text_size * scale * pixel_density
            / text_raster_size_quantum) * text_raster_size_quantum);
    const bool raster_size_changed =
        desired_raster_size != text_raster_size_;
    const bool geometry_scale_changed =
        std::abs(text_geometry_scale_ - previous_geometry_scale) > 1.0e-6;
    if (raster_size_changed) {
        for (const auto& [key, cached] : text_cache_) {
            static_cast<void>(key);
            TTF_DestroyText(cached.text);
        }
        text_cache_.clear();

        // SDL_ttf may recycle glyph rectangles after a font-size change.
        // The previous frame can still be sampling those rectangles, and a
        // partial upload to a bound SDL_GPU texture without cycling is
        // undefined. Start a fresh atlas generation instead; SDL defers the
        // old texture's release until its submitted draws have completed.
        TTF_DestroyGPUTextEngine(text_engine_);
        text_engine_ = nullptr;

        if (!TTF_SetFontSize(
                font_, static_cast<float>(desired_raster_size))) {
            throw_sdl("DPI-aware font sizing");
        }
        constexpr double secondary_text_size = 14.0;
        if (!TTF_SetFontSize(
                secondary_font_,
                static_cast<float>(
                    desired_raster_size
                    * secondary_text_size / base_text_size))) {
            throw_sdl("DPI-aware secondary font sizing");
        }
        if (!TTF_SetFontSize(
                bold_font_, static_cast<float>(desired_raster_size))) {
            throw_sdl("DPI-aware bold font sizing");
        }
        if (!TTF_SetFontSize(document_font_,
                static_cast<float>(desired_raster_size * 14.0 / base_text_size))
            || !TTF_SetFontSize(caption_font_,
                static_cast<float>(desired_raster_size * 12.0 / base_text_size))) {
            throw_sdl("DPI-aware document font sizing");
        }
        constexpr double title_text_size = 29.0;
        if (!TTF_SetFontSize(
                title_font_,
                static_cast<float>(
                    desired_raster_size
                    * title_text_size / base_text_size))) {
            throw_sdl("DPI-aware title font sizing");
        }
        text_engine_ = TTF_CreateGPUTextEngine(device_);
        if (text_engine_ == nullptr) {
            throw_sdl("DPI-aware text-atlas creation");
        }
        text_raster_size_ = desired_raster_size;
    }
    if (raster_size_changed || geometry_scale_changed) {
        text_width_cache_.clear();
        fitted_text_cache_.clear();
    }

    // The font atlas is rasterized at the final DPI-aware size. Geometry only
    // converts physical font pixels back into window coordinates, avoiding
    // arbitrary per-label scaling and keeping weight consistent.
    return static_cast<double>(TTF_GetFontHeight(font_))
        * text_geometry_scale_;
}

void GpuRenderer::build_text_geometry(
    const Toolbar& toolbar,
    const Camera& camera,
    const double background_opacity)
{
    const double font_line_height = prepare_text_frame(
        toolbar.viewport_width(),
        toolbar.viewport_height(),
        toolbar.scale());
    const InterfacePalette palette = toolbar_palette(toolbar);
    const std::array<float, 4> text = text_color(palette.text);
    const std::array<float, 4> muted = text_color(palette.muted);
    const std::array<float, 4> selected_text =
        text_color(palette.on_primary);
    const double scale = toolbar.scale();
    const double reveal = toolbar.reveal();
    const double chrome_reveal = reveal
        * std::clamp(background_opacity, 0.0, 1.0);
    std::optional<UiRect> modal_bounds;
    if (toolbar.settings_open()
        && toolbar.panels().size() >= 2U) {
        modal_bounds =
            toolbar.panels()[toolbar.panels().size() - 2U].bounds;
    }
    const auto base_text_alpha = [&](const UiRect bounds) {
        if (modal_bounds.has_value()
            && bounds.x < modal_bounds->x + modal_bounds->width
            && bounds.x + bounds.width > modal_bounds->x
            && bounds.y < modal_bounds->y + modal_bounds->height
            && bounds.y + bounds.height > modal_bounds->y) {
            return 0.0;
        }
        return chrome_reveal;
    };

    const auto slide = [](double) { return 0.0; };
    const auto faded = [](std::array<float, 4> color, const double alpha) {
        color[3] *= static_cast<float>(std::clamp(alpha, 0.0, 1.0));
        return color;
    };

    // Rebuilt below when a filename is being edited; cleared here so a stale
    // layout from a previous frame cannot be hit tested after editing ends.
    filename_caret_stops_.clear();

    const auto centered_text_top = [&](const double box_y,
                                       const double box_height) {
        return box_y + (box_height - font_line_height) * 0.5;
    };

    if (!toolbar.panels().empty()
        && toolbar.panels().back().bounds.width > 1.0
        && toolbar.panels().back().bounds.height > 1.0) {
        UiRect status = toolbar.panels().back().bounds;
        status.y += slide(status.y);
        const std::array<float, 4> status_text =
            faded(text, base_text_alpha(status));
        const double text_x = status.x + 12.0 * scale;
        const double baseline = toolbar.filename_editing()
            ? centered_text_top(status.y, status.height) : status.y + 3.0 * scale;
        const std::string raw_filename{toolbar.filename()};
        std::string filename{raw_filename};
        if (const UiControl* const rename =
                toolbar.find(UiAction::rename_board)) {
            const double available = std::max(
                12.0 * scale, status.x + status.width - text_x - 12.0 * scale);
            const auto text_width = [&](const std::string& value) {
                int width = 0;
                int height = 0;
                if (!TTF_GetStringSize(
                        font_, value.c_str(), 0U, &width, &height)) {
                    return 0.0;
                }
                return static_cast<double>(width) * text_geometry_scale_;
            };
            const auto fits = [&](const std::string& value) {
                return text_width(value) <= available;
            };
            const auto previous_codepoint = [](const std::string& value,
                                               std::size_t offset) {
                if (offset == 0U) return std::size_t{0U};
                --offset;
                while (offset > 0U
                       && (static_cast<unsigned char>(value[offset]) & 0xC0U)
                           == 0x80U) {
                    --offset;
                }
                return offset;
            };
            const auto next_codepoint = [](const std::string& value,
                                           std::size_t offset) {
                if (offset >= value.size()) return value.size();
                ++offset;
                while (offset < value.size()
                       && (static_cast<unsigned char>(value[offset]) & 0xC0U)
                           == 0x80U) {
                    ++offset;
                }
                return offset;
            };

            if (toolbar.filename_editing()) {
                std::size_t visible_start = 0U;
                std::size_t visible_end = raw_filename.size();
                const auto visible_text = [&]() {
                    std::string result;
                    if (visible_start > 0U) result = "...";
                    result.append(raw_filename.substr(
                        visible_start, visible_end - visible_start));
                    return result;
                };
                while (visible_start < visible_end
                       && !fits(visible_text())) {
                    visible_start = next_codepoint(
                        raw_filename, visible_start);
                }
                const std::size_t cursor = std::min(
                    toolbar.filename_cursor(), raw_filename.size());
                if (cursor < visible_start) {
                    visible_start = cursor;
                    visible_end = raw_filename.size();
                    while (visible_end > visible_start
                           && !fits(visible_text())) {
                        visible_end = previous_codepoint(
                            raw_filename, visible_end);
                    }
                }
                filename = visible_text();

                const std::size_t selected_first = std::min(
                    toolbar.filename_cursor(), toolbar.filename_anchor());
                const std::size_t selected_last = std::max(
                    toolbar.filename_cursor(), toolbar.filename_anchor());
                const std::size_t visible_selected_first = std::clamp(
                    selected_first, visible_start, visible_end);
                const std::size_t visible_selected_last = std::clamp(
                    selected_last, visible_start, visible_end);
                const std::string prefix = visible_start > 0U ? "..." : "";

                // Record the screen x of every visible caret boundary so the
                // application can resolve a pointer click to a caret position.
                // The leading "..." prefix (when text is scrolled) contributes
                // width but maps to the first visible byte offset.
                {
                    const double base_x = text_x + text_width(prefix);
                    filename_caret_stops_.emplace_back(visible_start, base_x);
                    std::size_t offset = visible_start;
                    while (offset < visible_end) {
                        const std::size_t next =
                            next_codepoint(raw_filename, offset);
                        const std::string span = prefix
                            + raw_filename.substr(
                                visible_start, next - visible_start);
                        filename_caret_stops_.emplace_back(
                            next, text_x + text_width(span));
                        offset = next;
                    }
                }

                if (visible_selected_first < visible_selected_last) {
                    std::string before = prefix;
                    before.append(raw_filename.substr(
                        visible_start,
                        visible_selected_first - visible_start));
                    const std::string selected = raw_filename.substr(
                        visible_selected_first,
                        visible_selected_last - visible_selected_first);
                    const std::string after = raw_filename.substr(
                        visible_selected_last,
                        visible_end - visible_selected_last);
                    const double before_width = text_width(before);
                    const double selected_width = text_width(selected);
                    append_screen_rounded_rect(
                        geometry_,
                        {text_x + before_width,
                         baseline - 1.0 * scale,
                         std::max(selected_width, 2.0 * scale),
                         font_line_height + 2.0 * scale},
                        3.0 * scale,
                        camera,
                        palette.primary);
                    queue_text(before, text_x, baseline, status_text);
                    queue_text(
                        selected, text_x + before_width,
                        baseline, faded(selected_text, chrome_reveal));
                    queue_text(
                        after, text_x + before_width + selected_width,
                        baseline, status_text);
                } else {
                    queue_text(filename, text_x, baseline, status_text);
                    const std::size_t visible_cursor = std::clamp(
                        cursor, visible_start, visible_end);
                    std::string before_cursor = prefix;
                    before_cursor.append(raw_filename.substr(
                        visible_start, visible_cursor - visible_start));
                    const double caret_x = text_x + text_width(before_cursor);
                    append_screen_line(
                        geometry_,
                        {caret_x, baseline + 1.0 * scale},
                        {caret_x,
                         baseline + font_line_height - 1.0 * scale},
                        1.5 * scale,
                        camera,
                        palette.focus);
                }
            } else {
                queue_text(fit_text_to_width(filename, available, TextStyle::document),
                    text_x, baseline, status_text, TextStyle::document);
            }
        } else {
            queue_text(filename, text_x, baseline, status_text, TextStyle::document);
        }
        if (!toolbar.filename_editing()) {
            const double status_budget = status.x + status.width - text_x - 12.0 * scale;
            const auto status_color = text_color(mix_color(palette.muted, palette.surface, 0.12));
            queue_text(fit_text_to_width(toolbar.document_status(), status_budget, TextStyle::caption),
                text_x, status.y + 23.0 * scale, faded(status_color, base_text_alpha(status)), TextStyle::caption);
        }
        if (!toolbar.error_message().empty()) {
            UiRect error = toolbar.error_bounds();
            error.y += slide(error.y);
            const double error_text_x = error.x + 34.0 * scale;
            const double text_budget = std::max(
                error.width - 48.0 * scale, 1.0);
            queue_text(
                fit_text_to_width(
                    toolbar.error_message(),
                    text_budget,
                    TextStyle::bold),
                error_text_x,
                centered_text_top(error.y, error.height),
                faded(text, base_text_alpha(error)),
                TextStyle::bold);
        } else if (!toolbar.status_message().empty()) {
            UiRect activity = toolbar.status_bounds();
            activity.y += slide(activity.y);
            const double text_budget = std::max(
                activity.width - 24.0 * scale, 1.0);
            queue_text(
                fit_text_to_width(
                    toolbar.status_message(),
                    text_budget,
                    TextStyle::regular),
                activity.x + 12.0 * scale,
                centered_text_top(activity.y, activity.height),
                faded(status_text, base_text_alpha(activity)));
        }
    }

    if (toolbar.properties_surface_bounds().width > 0.0) {
        const UiRect panel = toolbar.properties_surface_bounds();
        const UiControl* toggle = toolbar.find(UiAction::properties_menu);
        if (toggle != nullptr) queue_text("Style", panel.x + 12.0 * scale,
            centered_text_top(toggle->bounds.y, toggle->bounds.height),
            faded(muted, base_text_alpha(panel)), TextStyle::bold);
    }
    for (const UiAction action : {UiAction::file_menu}) {
        const UiControl* const control = toolbar.find(action);
        if (control == nullptr || control->label.empty()) continue;
        UiRect label_bounds = control->bounds;
        if (action == UiAction::file_menu) label_bounds.width -= 18.0 * scale;
        if (action == UiAction::properties_menu) label_bounds.width -= 32.0 * scale;
        const TextStyle label_style = action == UiAction::file_menu
            ? TextStyle::secondary : TextStyle::bold;
        const std::string label = fit_text_to_width(control->label,
            label_bounds.width - 12.0 * scale, label_style);
        const double label_width = measure_text_width(label, label_style);
        queue_text(
            label,
            label_bounds.x + (label_bounds.width - label_width) * 0.5,
            centered_text_top(control->bounds.y, control->bounds.height),
            faded(!control->enabled ? muted
                    : (control->selected && uses_soft_property_selection(action)
                        ? text_color(palette.focus) : (control->selected ? selected_text : text)),
                base_text_alpha(control->bounds)),
            label_style);
    }

    if (const UiControl* const zoom = toolbar.find(UiAction::zoom_menu)) {
        std::array<char, 16> label{};
        static_cast<void>(std::snprintf(
            label.data(), label.size(), "%d%%",
            static_cast<int>(std::lround(toolbar.zoom() * 100.0))));
        int label_width = 0;
        int label_height = 0;
        if (!TTF_GetStringSize(
                font_, label.data(), 0U, &label_width, &label_height)) {
            throw_sdl("Zoom-label measurement");
        }
        static_cast<void>(label_height);
        queue_text(
            label.data(),
            zoom->bounds.x
                + (zoom->bounds.width
                    - static_cast<double>(label_width)
                        * text_geometry_scale_) * 0.5,
            centered_text_top(zoom->bounds.y, zoom->bounds.height)
                + slide(zoom->bounds.y),
            faded(zoom->selected ? selected_text : text, base_text_alpha(zoom->bounds)));
    }

    const UiRect clip = toolbar.properties_clip();
    if (clip.width > 0.0) {
        text_clip_ = std::array<double, 4>{clip.x, clip.y, clip.width, clip.height};
    }
    const auto properties_text_alpha = [&](const UiRect bounds) {
        return base_text_alpha(bounds) * toolbar.properties_reveal();
    };
    constexpr std::array context_label_actions{
        UiAction::color_target_stroke, UiAction::color_target_fill,
        UiAction::fill_none, UiAction::width_cycle,
    };
    for (const UiAction action : context_label_actions) {
        const UiControl* const control = toolbar.find(action);
        if (control == nullptr || control->label.empty()) {
            continue;
        }
        int label_width = 0;
        int label_height = 0;
        if (!TTF_GetStringSize(
                font_,
                control->label.data(),
                control->label.size(),
                &label_width,
                &label_height)) {
            throw_sdl("Context-label measurement");
        }
        static_cast<void>(label_height);
        const double label_area_x = control->bounds.x;
        const double label_area_width = control->bounds.width;
        const bool soft_selected =
            control->selected && uses_soft_property_selection(action);
        const std::string fitted_label = fit_text_to_width(control->label,
            std::max(1.0, label_area_width - 12.0 * scale), TextStyle::regular);
        const double fitted_width = measure_text_width(fitted_label, TextStyle::regular);
        queue_text(
            fitted_label,
            label_area_x
                + (label_area_width - fitted_width) * 0.5,
            centered_text_top(
                control->bounds.y, control->bounds.height)
                + slide(control->bounds.y),
            faded(
                !control->enabled
                    ? muted
                    : (soft_selected
                        ? text_color(palette.focus)
                        : (control->selected ? selected_text : text)),
                properties_text_alpha(control->bounds)),
            TextStyle::regular);
    }

    const auto context_section_label =
        [&](const UiAction first_action, const std::string_view label) {
            const UiControl* const first = toolbar.find(first_action);
            if (first == nullptr) return;
            queue_text(
                label,
                first->bounds.x,
                first->bounds.y - font_line_height - 1.0 * scale
                    + slide(first->bounds.y),
                faded(
                    muted,
                    properties_text_alpha({
                        first->bounds.x,
                        first->bounds.y - font_line_height - 2.0 * scale,
                        100.0 * scale,
                        font_line_height + 3.0 * scale,
                    })), TextStyle::bold);
        };
    context_section_label(UiAction::color_white, "Color");
    context_section_label(UiAction::width_thin, "Width");
    context_section_label(UiAction::roundness_square, "Corners");
    text_clip_.reset();

    if (toolbar.settings_open()) {
        // Settings labels fade and rise with the flyout geometry.
        const double settings_alpha = toolbar.settings_reveal();
        const auto settings_shift = [&](const double y) {
            return slide(y);
        };
        const std::array<float, 4> settings_text = faded(text, settings_alpha);
        const std::array<float, 4> settings_muted =
            faded(muted, settings_alpha);
        const std::array<float, 4> settings_selected =
            faded(selected_text, settings_alpha);
        const UiControl* const back = toolbar.find(UiAction::settings_close);
        if (back != nullptr) {
            std::string_view title = "Canvas settings";
            if (toolbar.settings_page() == SettingsPage::file) {
                title = "File";
            } else if (toolbar.settings_page() == SettingsPage::preferences) {
                title = "Board settings";
            } else if (toolbar.settings_page() == SettingsPage::canvas) {
                title = "Canvas settings";
            } else if (toolbar.settings_page() == SettingsPage::view) {
                title = "Zoom and framing";
            } else if (toolbar.settings_page() == SettingsPage::about) {
                title = "About Sawer";
            } else if (toolbar.settings_page() == SettingsPage::color_editor) {
                if (toolbar.custom_color_target()
                    == CustomColorTarget::stroke) {
                    title = "Custom stroke color";
                } else if (toolbar.custom_color_target()
                           == CustomColorTarget::fill) {
                    title = "Custom fill color";
                } else if (toolbar.custom_color_target()
                           == CustomColorTarget::background) {
                    title = "Custom background color";
                } else {
                    title = "Custom grid color";
                }
            }
            queue_text(
                title,
                toolbar.panels()[toolbar.panels().size() - 2U].bounds.x + 16.0 * scale,
                centered_text_top(back->bounds.y, back->bounds.height)
                    + settings_shift(back->bounds.y),
                settings_text,
                TextStyle::bold);
        }

        const auto section_label = [&](const UiAction first_action,
                                       const std::string_view label) {
            if (const UiControl* const first = toolbar.find(first_action)) {
                queue_text(
                    label,
                    first->bounds.x,
                    first->bounds.y - font_line_height - 4.0 * scale
                        + settings_shift(first->bounds.y),
                    settings_text);
            }
        };
        if (toolbar.settings_page() == SettingsPage::canvas) {
            const double section_height = static_cast<double>(TTF_GetFontHeight(document_font_)) * text_geometry_scale_;
            const double caption_height = static_cast<double>(TTF_GetFontHeight(caption_font_)) * text_geometry_scale_;
            const auto canvas_heading = [&](const UiAction first_action, const std::string_view label) {
                if (const UiControl* first = toolbar.find(first_action)) {
                    queue_text(label, first->bounds.x, first->bounds.y - section_height - 6.0 * scale,
                        settings_muted, TextStyle::document);
                }
            };
            canvas_heading(UiAction::bg_color_6, "Background");
            canvas_heading(UiAction::grid_color_6, "Grid color");
            canvas_heading(UiAction::grid_solid, "Pattern");
            for (const UiControl& control : toolbar.controls()) {
                if (!is_canvas_pattern(control.action) || control.label.empty()) continue;
                const std::string label = fit_text_to_width(control.label,
                    control.bounds.width - 8.0 * scale, TextStyle::caption);
                const double width = measure_text_width(label, TextStyle::caption);
                queue_text(label, control.bounds.x + (control.bounds.width - width) * 0.5,
                    control.bounds.y + control.bounds.height - 18.0 * scale
                        + (16.0 * scale - caption_height) * 0.5,
                    control.selected ? faded(text_color(palette.focus), settings_alpha) : settings_muted,
                    TextStyle::caption);
            }
            for (const UiAction action : {UiAction::grid_color_auto, UiAction::edit_background_custom}) {
                const UiControl* control = toolbar.find(action);
                if (control == nullptr) continue;
                UiRect label = control->bounds;
                if (action == UiAction::edit_background_custom) {
                    label.x += 32.0 * scale;
                    label.width -= 36.0 * scale;
                }
                const std::string value = fit_text_to_width(control->label, label.width - 4.0 * scale, TextStyle::caption);
                const double width = measure_text_width(value, TextStyle::caption);
                queue_text(value, label.x + (label.width - width) * 0.5,
                    label.y + (label.height - caption_height) * 0.5,
                    control->selected ? faded(text_color(palette.focus), settings_alpha) : settings_text,
                    TextStyle::caption);
            }
        } else if (toolbar.settings_page() == SettingsPage::color_editor) {
            section_label(UiAction::custom_hue_field, "Hue");
            section_label(
                UiAction::custom_sv_field, "Saturation & brightness");
            const UiRect preview = toolbar.custom_color_preview_bounds();
            const Color color = toolbar.custom_color();
            std::array<char, 16> hex{};
            std::array<char, 32> rgb{};
            static_cast<void>(std::snprintf(hex.data(), hex.size(), "#%02X%02X%02X",
                static_cast<unsigned>(color.red), static_cast<unsigned>(color.green),
                static_cast<unsigned>(color.blue)));
            static_cast<void>(std::snprintf(rgb.data(), rgb.size(), "RGB %u, %u, %u",
                static_cast<unsigned>(color.red), static_cast<unsigned>(color.green),
                static_cast<unsigned>(color.blue)));
            const double text_x = preview.x + preview.height + 8.0 * scale;
            const std::string hex_label = toolbar.hex_editing()
                ? std::string{toolbar.hex_text()} + (toolbar.hex_selected() ? "" : "|") : std::string{hex.data()};
            queue_text(hex_label, text_x + 8.0 * scale, preview.y + 3.0 * scale,
                settings_text, TextStyle::bold);
            queue_text(toolbar.hex_valid() ? std::string_view{rgb.data()} : "Use #RRGGBB or #RGB",
                text_x + 8.0 * scale, preview.y + 23.0 * scale,
                toolbar.hex_valid() ? settings_muted
                    : faded(text_color(toolbar.theme() == Theme::light
                        ? RenderColor{0.72F, 0.18F, 0.24F}
                        : RenderColor{1.0F, 0.48F, 0.54F}), settings_alpha),
                TextStyle::secondary);
            if (toolbar.find(UiAction::recent_color_0) == nullptr) {
                const UiRect footer = toolbar.find(UiAction::custom_color_done)->bounds;
                queue_text("Custom colors appear here after Apply", preview.x,
                    footer.y - 34.0 * scale, settings_muted, TextStyle::secondary);
            }
        }

        if (toolbar.settings_page() == SettingsPage::about
            && toolbar.panels().size() >= 2U) {
            struct NoticeLine final {
                std::string text;
                TextStyle style{TextStyle::regular};
            };
            std::vector<NoticeLine> lines;
            lines.reserve(384U);

            const UiRect panel =
                toolbar.panels()[toolbar.panels().size() - 2U].bounds;
            const double body_x = panel.x + 20.0 * scale;
            const double body_width = std::max(
                panel.width - 40.0 * scale, 1.0);
            const double body_top = panel.y + 62.0 * scale;
            const double body_bottom = panel.y + panel.height - 34.0 * scale;
            const double line_height = font_line_height + 4.0 * scale;

            const auto append_wrapped = [&](std::string_view raw,
                                            const TextStyle line_style) {
                while (!raw.empty() && raw.front() == '>') {
                    raw.remove_prefix(1U);
                    if (!raw.empty() && raw.front() == ' ') {
                        raw.remove_prefix(1U);
                    }
                }
                if (raw.empty()) {
                    lines.push_back({});
                    return;
                }

                std::istringstream words{std::string{raw}};
                std::string word;
                std::string current;
                while (words >> word) {
                    const std::string candidate = current.empty()
                        ? word
                        : current + ' ' + word;
                    if (measure_text_width(candidate, line_style)
                        <= body_width) {
                        current = candidate;
                        continue;
                    }
                    if (!current.empty()) {
                        lines.push_back({std::move(current), line_style});
                        current.clear();
                    }
                    while (measure_text_width(word, line_style)
                           > body_width && word.size() > 1U) {
                        std::size_t count = word.size() - 1U;
                        while (count > 1U
                               && measure_text_width(
                                      std::string_view{word}.substr(0U, count),
                                      line_style) > body_width) {
                            --count;
                        }
                        lines.push_back({word.substr(0U, count), line_style});
                        word.erase(0U, count);
                    }
                    current = std::move(word);
                }
                if (!current.empty()) {
                    lines.push_back({std::move(current), line_style});
                }
            };

            append_wrapped(
                std::string{BuildInfo::name} + " "
                    + std::string{BuildInfo::version} + " ("
                    + std::string{build_configuration()} + ")",
                TextStyle::bold);
            append_wrapped(
                "Third-party licenses are embedded in this executable. "
                "Scroll with the mouse wheel, arrow keys, Page Up/Down, "
                "Home, or End.",
                TextStyle::regular);
            lines.push_back({});

            const std::string_view notices = third_party_notices();
            std::size_t offset = 0U;
            while (offset <= notices.size()) {
                const std::size_t end = notices.find('\n', offset);
                std::string_view raw = notices.substr(
                    offset,
                    end == std::string_view::npos
                        ? notices.size() - offset
                        : end - offset);
                TextStyle line_style = TextStyle::regular;
                if (raw.starts_with('#')) {
                    while (!raw.empty()
                           && (raw.front() == '#' || raw.front() == ' ')) {
                        raw.remove_prefix(1U);
                    }
                    line_style = TextStyle::bold;
                }
                append_wrapped(raw, line_style);
                if (end == std::string_view::npos) {
                    break;
                }
                offset = end + 1U;
            }

            const auto visible_count = static_cast<std::size_t>(std::max(
                1.0,
                std::floor((body_bottom - body_top) / line_height)));
            const std::size_t maximum_start = lines.size() > visible_count
                ? lines.size() - visible_count
                : 0U;
            const std::size_t first = static_cast<std::size_t>(std::llround(
                toolbar.about_scroll()
                * static_cast<double>(maximum_start)));
            const std::size_t last = std::min(
                lines.size(), first + visible_count);
            double line_y = body_top + settings_shift(body_top);
            for (std::size_t index = first; index < last; ++index) {
                queue_text(
                    lines[index].text,
                    body_x,
                    line_y,
                    lines[index].style == TextStyle::bold
                        ? settings_text
                        : settings_muted,
                    lines[index].style);
                line_y += line_height;
            }

            std::array<char, 48> progress{};
            static_cast<void>(std::snprintf(
                progress.data(),
                progress.size(),
                "Licenses  %d%%",
                static_cast<int>(std::lround(
                    toolbar.about_scroll() * 100.0))));
            queue_text(
                progress.data(),
                body_x,
                panel.y + panel.height - font_line_height - 10.0 * scale
                    + settings_shift(panel.y),
                settings_muted);
        }

        constexpr std::array label_actions{
            UiAction::new_board,
            UiAction::open_board,
            UiAction::rename_file,
            UiAction::save_as,
            UiAction::save_copy,
            UiAction::format_background,
            UiAction::toggle_theme,
            UiAction::about,
            UiAction::zoom_reset,
            UiAction::zoom_fit_content,
            UiAction::zoom_fit_selection,
            UiAction::custom_color_done,
            UiAction::custom_color_cancel,
        };
        for (const auto action : label_actions) {
            const UiControl* const control = toolbar.find(action);
            if (control == nullptr || control->label.empty()
                || !is_settings_control(toolbar, control)) {
                continue;
            }
            int label_width = 0;
            int label_height = 0;
            if (!TTF_GetStringSize(
                    font_, control->label.data(), control->label.size(),
                    &label_width, &label_height)) {
                throw_sdl("Settings-label measurement");
            }
            static_cast<void>(label_height);
            const bool menu_row = toolbar.settings_page() == SettingsPage::file
                || toolbar.settings_page() == SettingsPage::preferences;
            const std::string_view shortcut = action == UiAction::rename_file ? "F2"
                : action == UiAction::new_board ? "Ctrl+N"
                : (action == UiAction::open_board ? "Ctrl+O"
                    : (action == UiAction::save_as ? "Ctrl+S"
                        : (action == UiAction::save_copy ? "Ctrl+Shift+S" : (action == UiAction::toggle_theme ? "T" : ""))));
            const double shortcut_width = shortcut.empty() ? 0.0 : measure_text_width(shortcut, TextStyle::secondary);
            const std::string label = menu_row
                ? fit_text_to_width(control->label,
                    control->bounds.width - 48.0 * scale - shortcut_width, TextStyle::regular)
                : std::string{control->label};
            queue_text(
                label,
                menu_row ? control->bounds.x + 36.0 * scale : control->bounds.x
                    + (control->bounds.width
                        - static_cast<double>(label_width)
                            * text_geometry_scale_) * 0.5,
                centered_text_top(
                    control->bounds.y, control->bounds.height)
                    + settings_shift(control->bounds.y),
                !control->enabled
                    ? settings_muted
                    : (control->selected
                        ? settings_selected
                        : settings_text));
            if (menu_row && !shortcut.empty()) queue_text(shortcut,
                control->bounds.x + control->bounds.width - shortcut_width - 8.0 * scale,
                centered_text_top(control->bounds.y, control->bounds.height), settings_muted, TextStyle::secondary);
        }

    }

    const UiControl* const described = toolbar.tooltip_control();
    const double described_visibility = described == nullptr ? 0.0 : 1.0;
    tooltip_first_text_batch_ = text_batches_.size();
    text_clip_.reset();
    if (tooltip_allowed_this_frame_ && described != nullptr && described_visibility > 0.08) {
        const Vec2d tooltip_extent = tooltip_text_extent(toolbar, *described);
        if (const auto bounds = tooltip_bounds(toolbar, described,
                tooltip_extent.x, tooltip_extent.y)) {
            const double visibility = described_visibility;
            const double offset =
                (1.0 - visibility) * 4.0 * scale + slide(bounds->y);
            const std::array<float, 4> tooltip_text =
                text_color(palette.background);
            queue_text(
                described->action == UiAction::rename_board ? described->tooltip
                    : fit_text_to_width(described->tooltip, bounds->width - 24.0 * scale),
                bounds->x + 12.0 * scale,
                tooltip_extent.y > 0.0 ? bounds->y + offset + 6.0 * scale
                    : centered_text_top(bounds->y + offset, bounds->height),
                faded(tooltip_text, visibility), TextStyle::regular,
                tooltip_extent.y > 0.0 ? bounds->width - 24.0 * scale : 0.0);
        }
    }
}

void GpuRenderer::build_unsaved_dialog_text_geometry(
    const UnsavedDialog& dialog,
    const Toolbar& toolbar)
{
    if (!dialog.visible()) {
        return;
    }
    const InterfacePalette palette = toolbar_palette(toolbar);
    const bool light = toolbar.theme() == Theme::light;
    const double reveal = dialog.reveal();
    const double scale = dialog.scale();
    const UiRect card = dialog.bounds();
    const double font_line_height =
        static_cast<double>(TTF_GetFontHeight(font_))
        * text_geometry_scale_;
    const auto faded = [reveal](std::array<float, 4> color) {
        color[3] *= static_cast<float>(reveal);
        return color;
    };
    const std::array<float, 4> text =
        faded(text_color(palette.text));
    const std::array<float, 4> muted =
        faded(text_color(palette.muted));
    const std::array<float, 4> danger = faded(text_color(
        light
        ? RenderColor{0.72F, 0.16F, 0.20F}
        : RenderColor{1.0F, 0.43F, 0.47F}));
    const std::array<float, 4> on_primary =
        faded(text_color(palette.on_primary));

    queue_text(
        "Unsaved changes",
        card.x + 70.0 * scale,
        card.y + 27.0 * scale,
        text,
        TextStyle::title);

    const double body_x = card.x + 26.0 * scale;
    const double body_width = card.width - 52.0 * scale;
    const std::string question =
        "Save changes to \"" + std::string{dialog.board_name()}
        + "\" before continuing?";
    queue_text(
        fit_text_to_width(question, body_width),
        body_x,
        card.y + 88.0 * scale,
        text);
    queue_text(
        fit_text_to_width(
            "If you don't save, your latest changes will be lost.",
            body_width),
        body_x,
        card.y + 116.0 * scale,
        muted);

    constexpr std::array choices{
        UnsavedDialogChoice::cancel,
        UnsavedDialogChoice::discard,
        UnsavedDialogChoice::save,
    };
    constexpr std::array<std::string_view, 3> labels{
        "Cancel",
        "Don't save",
        "Save",
    };
    for (std::size_t index = 0U; index < choices.size(); ++index) {
        const UnsavedDialogChoice choice = choices[index];
        const UiRect button = dialog.button_bounds(choice);
        const TextStyle style = choice == UnsavedDialogChoice::save
            ? TextStyle::bold
            : TextStyle::regular;
        const std::string label = fit_text_to_width(
            labels[index],
            std::max(1.0, button.width - 16.0 * scale),
            style);
        const double width = measure_text_width(label, style);
        queue_text(
            label,
            button.x + (button.width - width) * 0.5,
            button.y + (button.height - font_line_height) * 0.5,
            choice == UnsavedDialogChoice::save
                ? on_primary
                : (choice == UnsavedDialogChoice::discard ? danger : text),
            style);
    }
}

void GpuRenderer::build_home_text_geometry(
    const HomeView& home,
    const Camera& camera)
{
    const double font_line_height = prepare_text_frame(
        home.viewport_width(), home.viewport_height(), home.scale());
    const double date_line_height =
        static_cast<double>(TTF_GetFontHeight(caption_font_))
        * text_geometry_scale_;
    for (auto cached = thumbnail_textures_.begin();
         cached != thumbnail_textures_.end();) {
        if (cached->second.owner.expired()) {
            SDL_ReleaseGPUTexture(device_, cached->second.texture);
            cached = thumbnail_textures_.erase(cached);
        } else {
            ++cached;
        }
    }
    // Rebuilt below only while a recent board title is being edited.
    filename_caret_stops_.clear();
    const double scale = home.scale();
    const bool light = home.theme() == Theme::light;
    const bool previous_light = home.previous_theme() == Theme::light;
    const double theme_amount = smooth_theme_transition(home);
    const InterfacePalette palette = home_palette(home);
    const std::array<float, 4> text = text_color(palette.text);
    const std::array<float, 4> muted = text_color(palette.muted);
    const std::array<float, 4> strong = text_color(palette.text);
    const std::array<float, 4> on_accent =
        text_color(palette.on_primary);
    const std::array<float, 4> danger = text_color(mix_color(
        previous_light
            ? RenderColor{0.62F, 0.12F, 0.18F}
            : RenderColor{1.0F, 0.68F, 0.72F},
        light
            ? RenderColor{0.62F, 0.12F, 0.18F}
            : RenderColor{1.0F, 0.68F, 0.72F},
        theme_amount));
    const double reveal = home.reveal();
    const double panel_shift = (1.0 - reveal) * 16.0 * scale;
    const double header_shift = -(1.0 - reveal) * 8.0 * scale;

    const auto faded = [](std::array<float, 4> color, const double alpha) {
        color[3] *= static_cast<float>(std::clamp(alpha, 0.0, 1.0));
        return color;
    };

    const auto centered_text_top = [&](const double box_y,
                                       const double box_height) {
        return box_y + (box_height - font_line_height) * 0.5;
    };
    const auto previous_codepoint = [](const std::string& value,
                                       std::size_t offset) {
        if (offset == 0U) return std::size_t{0U};
        --offset;
        while (offset > 0U
               && (static_cast<unsigned char>(value[offset]) & 0xC0U)
                   == 0x80U) {
            --offset;
        }
        return offset;
    };
    const auto next_codepoint = [](const std::string& value,
                                   std::size_t offset) {
        if (offset >= value.size()) return value.size();
        ++offset;
        while (offset < value.size()
               && (static_cast<unsigned char>(value[offset]) & 0xC0U)
                   == 0x80U) {
            ++offset;
        }
        return offset;
    };
    // Fitting and measurement are cached across frames so entrance and hover
    // animation do not repeatedly reshape identical Home labels.
    const auto fit = [&](const std::string_view value,
                         const double max_width,
                         const TextStyle style = TextStyle::regular) {
        return fit_text_to_width(value, max_width, style);
    };

    const auto measure = [&](const std::string_view value,
                              const TextStyle style = TextStyle::regular) {
        return measure_text_width(value, style);
    };

    const auto& boards = home.boards();
    const double vertical_margin = 20.0 * scale;
    for (std::size_t index = 0U; index < boards.size(); ++index) {
        if (boards[index].preview == nullptr
            || !boards[index].preview->has_content) {
            continue;
        }
        UiRect card = home.controls()[index].bounds;
        card.y += home.control_offset(index);
        if (home.controls()[index].bounds.y
                + home.controls()[index].bounds.height <= home.grid_top()
            || card.y > home.viewport_height() + vertical_margin) {
            continue;
        }
        const UiRect thumbnail = home.board_preview_bounds(index);
        queue_home_thumbnail(
            boards[index].preview,
            thumbnail,
            {1.0F, 1.0F, 1.0F,
             static_cast<float>(home.entrance(index))});
    }

    const UiRect heading = home.heading_bounds();
    const std::string heading_text{"Recent boards"};
    const double title_line_height =
        static_cast<double>(TTF_GetFontHeight(title_font_))
        * text_geometry_scale_;
    queue_text(
        heading_text,
        heading.x,
        heading.y + (heading.height - title_line_height) * 0.5
            + header_shift,
        faded(strong, reveal),
        TextStyle::title);
    queue_text(
        std::to_string(boards.size()) + (boards.size() == 1U ? " board" : " boards"),
        heading.x + measure(heading_text, TextStyle::title) + 12.0 * scale,
        heading.y + (heading.height - date_line_height) * 0.5 + header_shift,
        faded(muted, reveal), TextStyle::caption);

    if ((!home.error_message().empty() || !home.status_message().empty())
        && home.status_bounds().width > 1.0) {
        UiRect status = home.status_bounds();
        status.y += panel_shift;
        const double text_x = status.x + 42.0 * scale;
        const double budget = std::max(
            status.width - 54.0 * scale, 1.0);
        if (!home.error_message().empty()) {
            queue_text(
                "Could not complete that action",
                text_x,
                status.y + 5.0 * scale,
                faded(danger, reveal),
                TextStyle::bold);
            queue_text(
                fit(std::string{home.error_message()}, budget),
                text_x,
                status.y + 5.0 * scale + font_line_height,
                faded(muted, reveal));
        } else {
            queue_text(
                fit(std::string{home.status_message()}, budget),
                text_x,
                centered_text_top(status.y, status.height),
                faded(text, reveal),
                TextStyle::regular);
        }
    }

    for (std::size_t index = 0U; index < home.controls().size(); ++index) {
        const UiControl& control = home.controls()[index];
        if (control.action != UiAction::home_new_board
            && control.action != UiAction::open_board) {
            continue;
        }
        queue_text(
            control.label,
            home_button_label_x(control.bounds, scale),
            centered_text_top(control.bounds.y, control.bounds.height)
                + home.control_offset(index),
            faded(control.selected ? on_accent : text,
                home.entrance(index)),
            TextStyle::bold);
    }

    if (boards.empty() && !home.panels().empty()) {
        const UiRect empty = home.panels().back().bounds;
        const std::string title{"No recent boards"};
        const std::string subtitle{
            "Create a new board or open a .sawer file."};
        queue_text(
            title,
            empty.x + (empty.width - measure(title, TextStyle::bold)) * 0.5,
            empty.y + 68.0 * scale + panel_shift,
            faded(strong, reveal),
            TextStyle::bold);
        queue_text(
            subtitle,
            empty.x + (empty.width - measure(subtitle)) * 0.5,
            empty.y + 98.0 * scale + panel_shift,
            faded(muted, reveal));
    }
    for (std::size_t index = 0U; index < boards.size(); ++index) {
        const UiControl& card = home.controls()[index];
        const std::array<float, 4> card_name_color = text;
        const std::array<float, 4> card_date_color = muted;
        const double entrance = home.entrance(index);
        const double offset = home.control_offset(index);
        const double card_top = card.bounds.y + offset;
        if (card.bounds.y + card.bounds.height <= home.grid_top()
            || card_top > home.viewport_height() + vertical_margin) {
            continue;
        }
        const UiRect name = home.board_name_bounds(index);
        const UiRect date = home.board_date_bounds(index);
        const double name_x = name.x;
        const double budget = name.width;
        const double name_y = name.y + (name.height - font_line_height) * 0.5;
        if (boards[index].editing) {
            const std::string& raw_name = boards[index].name;
            const std::size_t cursor =
                std::min(boards[index].edit_cursor, raw_name.size());
            std::size_t visible_start = 0U;
            std::size_t visible_end = raw_name.size();
            const auto visible_name = [&]() {
                std::string value;
                if (visible_start > 0U) value = "...";
                value.append(raw_name.substr(
                    visible_start, visible_end - visible_start));
                if (visible_end < raw_name.size()) value.append("...");
                return value;
            };
            const auto name_fits = [&]() {
                return measure(visible_name(), TextStyle::bold) <= budget;
            };

            // Keep the caret visible while long names scroll horizontally.
            while (visible_start < cursor && !name_fits()) {
                visible_start = next_codepoint(raw_name, visible_start);
            }
            while (visible_end > cursor && !name_fits()) {
                visible_end = previous_codepoint(raw_name, visible_end);
            }

            const std::string prefix = visible_start > 0U ? "..." : "";
            const std::string suffix =
                visible_end < raw_name.size() ? "..." : "";
            const double prefix_width =
                measure(prefix, TextStyle::bold);

            filename_caret_stops_.emplace_back(
                visible_start, name_x + prefix_width);
            for (std::size_t stop = visible_start; stop < visible_end;) {
                stop = next_codepoint(raw_name, stop);
                const std::string through =
                    raw_name.substr(visible_start, stop - visible_start);
                filename_caret_stops_.emplace_back(
                    stop,
                    name_x + prefix_width
                        + measure(through, TextStyle::bold));
            }

            const std::size_t selection_first = std::clamp(
                std::min(boards[index].edit_cursor,
                         boards[index].edit_anchor),
                visible_start, visible_end);
            const std::size_t selection_last = std::clamp(
                std::max(boards[index].edit_cursor,
                         boards[index].edit_anchor),
                visible_start, visible_end);
            std::string before = prefix;
            before.append(raw_name.substr(
                visible_start, selection_first - visible_start));
            const std::string selected = raw_name.substr(
                selection_first, selection_last - selection_first);
            std::string after = raw_name.substr(
                selection_last, visible_end - selection_last);
            after.append(suffix);
            const double before_width =
                measure(before, TextStyle::bold);
            const double selected_width =
                measure(selected, TextStyle::bold);
            if (selection_first < selection_last) {
                append_screen_rounded_rect(
                    geometry_,
                    {
                        name_x + before_width,
                        name_y - 1.0 * scale,
                        std::max(selected_width, 2.0 * scale),
                        font_line_height + 2.0 * scale,
                    },
                    3.0 * scale,
                    camera,
                    palette.primary);
                queue_text(
                    before, name_x, name_y,
                    faded(card_name_color, entrance), TextStyle::bold);
                queue_text(
                    selected, name_x + before_width, name_y,
                    faded(
                        on_accent,
                        entrance),
                    TextStyle::bold);
                queue_text(
                    after, name_x + before_width + selected_width, name_y,
                    faded(card_name_color, entrance), TextStyle::bold);
            } else {
                const std::string visible = visible_name();
                queue_text(
                    visible, name_x, name_y,
                    faded(card_name_color, entrance), TextStyle::bold);
                const std::string before_cursor =
                    raw_name.substr(
                        visible_start, cursor - visible_start);
                const double caret_x = name_x + prefix_width
                    + measure(before_cursor, TextStyle::bold);
                append_screen_line(
                    geometry_,
                    {caret_x, name_y + 1.0 * scale},
                    {caret_x,
                     name_y + font_line_height - 1.0 * scale},
                    1.5 * scale,
                    camera,
                    palette.focus);
            }
        } else {
            queue_text(
                fit(boards[index].name, budget, TextStyle::bold),
                name_x,
                name_y,
                faded(card_name_color, entrance),
                TextStyle::bold);
        }
        queue_text(
            fit(boards[index].display_date, date.width, TextStyle::caption),
            name_x,
            date.y + (date.height - date_line_height) * 0.5,
            faded(card_date_color, entrance),
            TextStyle::caption);
    }
    if (const auto described = home.date_tooltip(); tooltip_allowed_this_frame_ && described) {
        const std::string& timestamp = boards[*described].date;
        const UiRect date = home.board_date_bounds(*described);
        const double padding = 12.0 * scale;
        const double width = std::min(measure(timestamp) + padding * 2.0,
            std::max(home.viewport_width() - padding * 2.0, 1.0));
        const double height = font_line_height + 16.0 * scale;
        const double x = std::clamp(date.x, padding,
            std::max(padding, home.viewport_width() - width - padding));
        const double y = std::clamp(date.y - height - 8.0 * scale, padding,
            std::max(padding, home.viewport_height() - height - padding));
        const UiRect bounds{x, y, width, height};
        tooltip_first_vertex_ = static_cast<std::uint32_t>(geometry_.size());
        tooltip_first_text_batch_ = text_batches_.size();
        text_clip_.reset();
        append_soft_shadow(geometry_, geometry_spans_, bounds, 8.0 * scale,
            camera, RenderColor{0.0F, 0.0F, 0.0F}, scale, 0.7);
        append_screen_rounded_rect(geometry_, bounds, 8.0 * scale, camera, palette.text);
        queue_text(fit(timestamp, width - padding * 2.0), x + padding,
            y + (height - font_line_height) * 0.5, text_color(palette.background));
    }

}

bool GpuRenderer::tessellate_indexed_stroke(
    std::vector<CachedWorldVertex>& output, const Stroke& stroke,
    const Style& style, const std::uint32_t detail, const double zoom,
    const Aabb& visible, const StrokeSegmentIndex& index)
{
    output.clear();
    const bool complete = visit_indexed_stroke(stroke, style, detail, zoom, visible, index,
        [&](const std::span<const CachedWorldVertex> page) {
            if (page.size() > maximum_cached_stroke_vertices - output.size()) return false;
            const std::size_t required = output.size() + page.size();
            if (output.capacity() < required) {
                output.reserve(std::min(maximum_cached_stroke_vertices,
                    std::max(required, output.capacity() * 2U)));
            }
            output.insert(output.end(), page.begin(), page.end());
            return true;
        });
    if (!complete) std::vector<CachedWorldVertex>{}.swap(output);
    return complete;
}

bool GpuRenderer::visit_indexed_stroke(
    const Stroke& stroke, const Style& style, const std::uint32_t detail,
    const double zoom, const Aabb& visible, const StrokeSegmentIndex& index,
    const std::function<bool(std::span<const CachedWorldVertex>)>& consume)
{
    stroke_page_vertices_.clear();
    stroke_page_vertices_.reserve(stroke_page_vertex_count);
    const std::function<bool(std::span<const CachedWorldVertex>)> consume_page = [&](const auto page) {
        stats_.peak_stroke_page_vertices = std::max(stats_.peak_stroke_page_vertices, page.size());
        return consume(page);
    };
    PagedStrokeGeometry output{stroke_page_vertices_, consume_page, visible};
    // Keep the callback alive for every page flush.
    try {
        if (stroke.points.size() < 2U) {
            tessellate_polyline(output, stroke.points, style, detail);
        } else {
            const double padding = std::max(style.stroke_width, 0.5) * 0.5;
            index.query({visible.min_x - padding, visible.min_y - padding,
                visible.max_x + padding, visible.max_y + padding}, stroke_segment_scratch_);
            for (std::size_t first = 0U; first < stroke_segment_scratch_.size();) {
                std::size_t last = first;
                while (last + 1U < stroke_segment_scratch_.size()
                    && stroke_segment_scratch_[last + 1U] == stroke_segment_scratch_[last] + 1U) ++last;
                const std::size_t begin = stroke_segment_scratch_[first] - 1U;
                const std::size_t end = stroke_segment_scratch_[last];
                simplify_stroke_for_rendering(
                    std::span<const Vec2d>{stroke.points}.subspan(begin, end - begin + 1U),
                    stroke_detail_tolerance(zoom), stroke_render_points_);
                tessellate_polyline(output, stroke_render_points_, style, detail);
                first = last + 1U;
            }
        }
        output.flush();
    } catch (const StrokePageStopped&) {
        stroke_page_vertices_.clear();
        return false;
    }
    return true;
}

void GpuRenderer::prune_cache(const ObjectId* const protected_id)
{
    const bool pressure = geometry_cache_bytes_ + geometry_metadata_bytes()
        > geometry_cache_budget || object_cache_.size() > maximum_cache_entries;
    const std::size_t byte_target = pressure ? geometry_cache_budget * 9U / 10U : geometry_cache_budget;
    const std::size_t entry_target = pressure ? maximum_cache_entries * 9U / 10U : maximum_cache_entries;
    std::size_t stale_removed = 0U;
    while (const auto oldest = geometry_recency_.oldest()) {
        if (protected_id != nullptr && *oldest == *protected_id) {
            if (geometry_recency_.size() == 1U) break;
            geometry_recency_.touch(*oldest);
            continue;
        }
        const auto cached = object_cache_.find(*oldest);
        const bool over_target = geometry_cache_bytes_ + geometry_metadata_bytes()
            > byte_target || object_cache_.size() > entry_target;
        const bool stale = cached->second.last_used_frame + 600U < stats_.frame;
        if (!over_target && (!stale || stale_removed >= 64U)) break;
        release_mesh(cached->second);
        if (cached->second.alternate) release_mesh(*cached->second.alternate);
        geometry_cache_bytes_ -= cached->second.capacity_bytes();
        object_cache_.erase(cached);
        geometry_recency_.erase(*oldest);
        ++stale_removed;
        ++total_evictions_;
    }
}

void GpuRenderer::trim_board_caches_for_home()
{
    release_mesh_storage();
    decltype(object_cache_){}.swap(object_cache_);
    geometry_recency_.clear();
    visibility_cache_.clear();
    std::vector<ObjectId>{}.swap(resident_scene_ids_);
    std::vector<DocumentChange>{}.swap(mesh_changes_);
    geometry_cache_bytes_ = 0U;
    scene_valid_ = false;
    draft_cache_active_ = false;
    draft_point_count_ = 0U;

    const auto reset_capacity = [](
        auto& values, const std::size_t retained_elements) {
        using Vector = std::decay_t<decltype(values)>;
        if (values.capacity() > retained_elements) {
            Vector replacement;
            replacement.reserve(retained_elements);
            values.swap(replacement);
        } else {
            values.clear();
        }
    };
    reset_capacity(
        scene_geometry_,
        initial_scene_vertex_capacity);
    reset_capacity(image_vertices_, 0U);
    reset_capacity(scene_draws_, 0U);
    reset_capacity(scene_stream_draws_, 0U);
    reset_capacity(scene_batch_geometry_, 0U);
    reset_capacity(scene_batch_draws_, 0U);
    reset_capacity(stroke_render_points_, 0U);
    reset_capacity(stroke_page_vertices_, 0U);
    reset_capacity(stroke_segment_scratch_, 0U);
    release_batch_slots();
    if (scene_stream_texture_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, scene_stream_texture_);
        scene_stream_texture_ = nullptr;
    }
    if (scene_stream_quad_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, scene_stream_quad_);
        scene_stream_quad_ = nullptr;
    }
    if (pixel_readback_surface_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, pixel_readback_surface_);
        pixel_readback_surface_ = nullptr;
    }
    if (pixel_readback_transfer_ != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, pixel_readback_transfer_);
        pixel_readback_transfer_ = nullptr;
    }
    pixel_readback_width_ = pixel_readback_height_ = 0U;
    pixel_readback_position_.reset();
    pixel_readback_result_.reset();
    scene_stream_width_ = scene_stream_height_ = 0U;
    scene_stream_capacity_width_ = scene_stream_capacity_height_ = 0U;
    reset_capacity(
        draft_gpu_geometry_,
        initial_draft_vertex_capacity);
    reset_capacity(
        geometry_,
        initial_overlay_vertex_capacity);
    reset_capacity(visible_objects_, 0U);
    reset_capacity(visible_object_ids_, 0U);
    reset_capacity(draft_shape_geometry_, 0U);
    reset_capacity(selection_preview_geometry_, 0U);
    reset_capacity(selection_preview_points_, 0U);
    reset_capacity(draft_render_points_, 0U);

    const auto reset_vertex_buffer =
        [&](SDL_GPUBuffer*& buffer,
            std::size_t& capacity,
            const std::size_t retained_vertices,
            const std::string_view label) {
        if (capacity <= retained_vertices) {
            return;
        }
        SDL_ReleaseGPUBuffer(device_, buffer);
        buffer = nullptr;
        capacity = 0U;
        static_cast<void>(ensure_vertex_buffer_capacity(
            buffer, capacity, retained_vertices, label));
    };
    reset_vertex_buffer(
        scene_vertex_buffer_,
        scene_vertex_capacity_,
        initial_scene_vertex_capacity,
        "GPU scene vertex-buffer trim");
    reset_vertex_buffer(
        draft_vertex_buffer_,
        draft_vertex_capacity_,
        initial_draft_vertex_capacity,
        "GPU draft vertex-buffer trim");
    reset_vertex_buffer(
        vertex_buffer_,
        overlay_vertex_capacity_,
        initial_overlay_vertex_capacity,
        "GPU overlay vertex-buffer trim");
    if (transfer_capacity_bytes_ > initial_transfer_capacity_bytes) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
        transfer_buffer_ = nullptr;
        transfer_capacity_bytes_ = 0U;
        ensure_transfer_capacity(initial_transfer_capacity_bytes);
    }
    for (const auto& [id, cached] : image_textures_) {
        static_cast<void>(id);
        for (const ImageTile& tile : cached.tiles) {
            SDL_ReleaseGPUTexture(device_, tile.texture);
        }
    }
    image_textures_.clear();
    image_texture_bytes_ = 0U;
    image_decode_cache_.clear();
}

GpuRenderer::CachedGeometry& GpuRenderer::cached_geometry_for(
    const Object& object, const Document& document,
    const Camera& camera, const Aabb& geometry_visible)
{
    const std::uint32_t detail =
        geometry_detail(object.geometry, object.style, camera.zoom());
    const auto* const stroke = std::get_if<Stroke>(&object.geometry);
    const int stroke_lod = stroke_detail_bucket(camera.zoom());
    const bool region_limited =
        stroke != nullptr && stroke->points.size() > 4'096U;
    const auto region_min_x = static_cast<std::int64_t>(
        std::floor(
            geometry_visible.min_x / SpatialChunkIndex::chunk_size));
    const auto region_min_y = static_cast<std::int64_t>(
        std::floor(
            geometry_visible.min_y / SpatialChunkIndex::chunk_size));
    const auto region_max_x = static_cast<std::int64_t>(
        std::floor(
            geometry_visible.max_x / SpatialChunkIndex::chunk_size));
    const auto region_max_y = static_cast<std::int64_t>(
        std::floor(
            geometry_visible.max_y / SpatialChunkIndex::chunk_size));
    auto& cached = object_cache_[object.id];
    const auto matches = [&](const CachedMesh& mesh) {
        // A finer stroke remains safe when zooming out. Never delay an
        // upgrade past the quarter-pixel tolerance when zooming in.
        return mesh.revision == object.revision && mesh.detail == detail
            && (stroke == nullptr || (mesh.stroke_detail >= stroke_lod
                && mesh.stroke_detail <= stroke_lod + 1))
            && (!region_limited || (mesh.region_limited
                && mesh.region_min_x <= region_min_x && mesh.region_min_y <= region_min_y
                && mesh.region_max_x >= region_max_x && mesh.region_max_y >= region_max_y));
    };
    if (!matches(cached) && cached.alternate && matches(*cached.alternate))
        std::swap(static_cast<CachedMesh&>(cached), *cached.alternate);
    if (!matches(cached)) {
        geometry_cache_bytes_ -= cached.capacity_bytes();
        if (cached.alternate) release_mesh(*cached.alternate);
        if (cached.revision == object.revision && !cached.vertices.empty())
            cached.alternate = std::move(static_cast<CachedMesh&>(cached));
        else { release_mesh(cached); cached.alternate.reset(); }
        static_cast<CachedMesh&>(cached) = {};
        if (region_limited) {
            bool index_built = false;
            const StrokeSegmentIndex* const segment_index =
                document.stroke_segment_index(
                    object.id, &index_built);
            if (segment_index == nullptr) {
                throw std::logic_error{
                    "Stroke object has no segment index"};
            }
            if (index_built) {
                ++stats_.segment_index_builds;
            }
            cached.paged = !tessellate_indexed_stroke(
                cached.vertices,
                *stroke,
                object.style,
                detail,
                camera.zoom(),
                {static_cast<double>(region_min_x) * SpatialChunkIndex::chunk_size,
                 static_cast<double>(region_min_y) * SpatialChunkIndex::chunk_size,
                 static_cast<double>(region_max_x + 1) * SpatialChunkIndex::chunk_size,
                 static_cast<double>(region_max_y + 1) * SpatialChunkIndex::chunk_size},
                *segment_index);
        } else if (stroke != nullptr) {
            cached.paged = false;
            simplify_stroke_for_rendering(
                stroke->points, stroke_detail_tolerance(camera.zoom()), stroke_render_points_);
            cached.vertices.clear();
            tessellate_polyline(cached.vertices, stroke_render_points_, object.style, detail);
        } else {
            cached.paged = false;
            tessellate_geometry(
                cached.vertices,
                object.geometry,
                object.style,
                detail);
        }
        geometry_cache_bytes_ += cached.capacity_bytes();
        cached.revision = object.revision;
        cached.detail = detail;
        cached.stroke_detail = stroke_lod;
        cached.region_limited = region_limited;
        cached.region_min_x = region_min_x;
        cached.region_min_y = region_min_y;
        cached.region_max_x = region_max_x;
        cached.region_max_y = region_max_y;
        ++stats_.tessellated_objects;
    } else {
        ++stats_.cache_hits;
    }
    cached.last_used_frame = stats_.frame;
    geometry_recency_.touch(object.id);
    if (geometry_cache_bytes_ + geometry_metadata_bytes() > geometry_cache_budget
        || object_cache_.size() > maximum_cache_entries) prune_cache(&object.id);
    return cached;
}

void GpuRenderer::build_board_geometry(
    const Camera& camera,
    const Document& document,
    const ObjectDraft* const preview,
    const Toolbar& toolbar,
    const Selection& selection,
    const SelectionPreview* const selection_preview,
    const DrawingCursor* const drawing_cursor)
{
    geometry_.clear();
    scene_active_this_frame_ = true;
    draft_active_this_frame_ = false;
    const auto camera_position = camera.position();
    const auto visible = camera.visible_world_bounds();
    constexpr double edge = Camera::board_half_extent;

    const double min_x = std::max(visible.min_x, -edge);
    const double min_y = std::max(visible.min_y, -edge);
    const double max_x = std::min(visible.max_x, edge);
    const double max_y = std::min(visible.max_y, edge);

    std::uint64_t signature = document.revision();
    const auto mix_signature = [&](const std::uint64_t value) {
        signature ^= value + 0x9E3779B97F4A7C15ULL
            + (signature << 6U) + (signature >> 2U);
    };
    const auto mix_double = [&](const double value) {
        mix_signature(std::bit_cast<std::uint64_t>(value));
    };
    const auto mix_color_signature = [&](const Color color) {
        mix_signature(
            static_cast<std::uint64_t>(color.red)
            | (static_cast<std::uint64_t>(color.green) << 8U)
            | (static_cast<std::uint64_t>(color.blue) << 16U)
            | (static_cast<std::uint64_t>(color.alpha) << 24U));
    };
    mix_double(camera_position.x);
    mix_double(camera_position.y);
    mix_double(camera.zoom());
    mix_double(camera.viewport().x);
    mix_double(camera.viewport().y);
    mix_signature(
        static_cast<std::uint64_t>(toolbar.background_style()));
    mix_color_signature(toolbar.background_color());
    if (toolbar.grid_color().has_value()) {
        mix_signature(1U);
        mix_color_signature(*toolbar.grid_color());
    } else {
        mix_signature(0U);
    }
    mix_signature(static_cast<std::uint64_t>(toolbar.theme()));
    mix_signature(static_cast<std::uint64_t>(toolbar.previous_theme()));
    mix_double(toolbar.theme_transition());
    if (selection_preview != nullptr) {
        mix_signature(1U);
        for (const auto id : selection_preview->ids) {
            mix_signature(
                static_cast<std::uint64_t>(ObjectIdHash{}(id)));
        }
    } else {
        mix_signature(0U);
    }
    const bool rebuild_scene =
        !scene_valid_ || signature != scene_signature_;
    scene_upload_pending_ = rebuild_scene;
    stats_.scene_rebuilt = rebuild_scene;
    if (rebuild_scene) {
        scene_geometry_.clear();
        resident_scene_ids_.clear();
        image_vertices_.clear();
        scene_draws_.clear();
        scene_stream_draws_.clear();
        scene_streamed_ = false;
        scene_rendered_images_ = 0U;
        scene_rendered_residents_ = 0U;
        // A skipped/minimized frame or failed submission must never make a
        // previous document view look like the newly rebuilt raster scene.
        scene_stream_width_ = scene_stream_height_ = 0U;

    const auto background_start = std::chrono::steady_clock::now();
    const RenderColor active_border_color = mix_color(
        toolbar.previous_theme() == Theme::light
            ? RenderColor{0.58F, 0.62F, 0.69F}
            : border_color,
        toolbar.theme() == Theme::light
            ? RenderColor{0.58F, 0.62F, 0.69F}
            : border_color,
        smooth_theme_transition(toolbar));

    const double border_width = 2.0 / camera.zoom();
    if (visible.min_x <= -edge + border_width) {
        append_quad(
            scene_geometry_, -edge, min_y, -edge + border_width, max_y,
            camera_position, active_border_color);
    }
    if (visible.max_x >= edge - border_width) {
        append_quad(
            scene_geometry_, edge - border_width, min_y, edge, max_y,
            camera_position, active_border_color);
    }
    if (visible.min_y <= -edge + border_width) {
        append_quad(
            scene_geometry_, min_x, -edge, max_x, -edge + border_width,
            camera_position, active_border_color);
    }
    if (visible.max_y >= edge - border_width) {
        append_quad(
            scene_geometry_, min_x, edge - border_width, max_x, edge,
            camera_position, active_border_color);
    }
    const auto background_end = std::chrono::steady_clock::now();
    scene_background_vertices_ = scene_geometry_.size();
    scene_total_vertices_ = scene_background_vertices_;
    scene_stream_draws_.push_back({SceneDraw::Kind::vector, 0U,
        static_cast<std::uint32_t>(scene_background_vertices_), nullptr, nullptr});
    stats_.background_milliseconds =
        std::chrono::duration<double, std::milli>(
            background_end - background_start).count();

    const auto query_start = std::chrono::steady_clock::now();
    if (visibility_cache_.query(document, visible, 128.0 / camera.zoom(),
            visible_objects_, visible_object_ids_)) ++stats_.visibility_query_reuses;
    const auto query_end = std::chrono::steady_clock::now();
    stats_.query_milliseconds =
        std::chrono::duration<double, std::milli>(
            query_end - query_start).count();
    stats_.visible_objects = visible_objects_.size();
    scene_visible_objects_ = visible_objects_.size();
    const auto objects_start = std::chrono::steady_clock::now();

    std::size_t vector_run_start = 0U;
    const auto flush_vector_run = [&]() {
        if (scene_streamed_) return;
        if (scene_geometry_.size() <= vector_run_start) return;
        scene_draws_.push_back({
            SceneDraw::Kind::vector,
            static_cast<std::uint32_t>(vector_run_start),
            static_cast<std::uint32_t>(scene_geometry_.size() - vector_run_start),
            nullptr,
        });
        vector_run_start = scene_geometry_.size();
    };
    for (const Object* const object : visible_objects_) {
        if (selection_preview != nullptr
            && selection_preview->contains(object->id)
            && !std::holds_alternative<Image>(object->geometry)) {
            continue;
        }
        const double screen_width =
            (object->bounds.max_x - object->bounds.min_x) * camera.zoom();
        const double screen_height =
            (object->bounds.max_y - object->bounds.min_y) * camera.zoom();
        if (screen_width < 0.35 && screen_height < 0.35) {
            continue;
        }
        if (const auto* image = std::get_if<Image>(&object->geometry)) {
            flush_vector_run();
            const std::size_t before = image_vertices_.size();
            const std::size_t first_draw = scene_draws_.size();
            append_image_draws(*image, camera);
            scene_stream_draws_.insert(scene_stream_draws_.end(),
                scene_draws_.begin() + static_cast<std::ptrdiff_t>(first_draw),
                scene_draws_.end());
            if (image_vertices_.size() > before) {
                ++stats_.rendered_objects;
                ++scene_rendered_images_;
            }
            vector_run_start = scene_geometry_.size();
            continue;
        }
        const auto existing = object_cache_.find(object->id);
        if (scene_streamed_ && (existing == object_cache_.end()
                || existing->second.resident.empty() || existing->second.revision != object->revision)) {
            // Build remaining meshes once, when their batch is consumed.
            // Filling the LRU first would evict and then retessellate them.
            scene_stream_draws_.push_back({SceneDraw::Kind::vector, 0U, 0U, nullptr, object});
            continue;
        }
        auto& cached = cached_geometry_for(*object, document, camera, visible);
        if (ensure_resident_mesh(cached, *object)) {
            resident_scene_ids_.push_back(object->id);
            flush_vector_run();
            const auto append_draw = [](std::vector<SceneDraw>& draws, const SceneDraw& draw) {
                if (!draws.empty() && draws.back().kind == SceneDraw::Kind::resident
                    && draws.back().buffer == draw.buffer && draws.back().origin == draw.origin
                    && draws.back().first_vertex + draws.back().vertex_count == draw.first_vertex)
                    draws.back().vertex_count += draw.vertex_count;
                else draws.push_back(draw);
            };
            for (const auto range : cached.resident) {
                const SceneDraw draw{SceneDraw::Kind::resident, range.first, range.count, nullptr,
                    object, mesh_pages_[range.page], cached.origin};
                append_draw(scene_stream_draws_, draw);
                if (!scene_streamed_) append_draw(scene_draws_, draw);
            }
            scene_total_vertices_ += cached.vertices.size();
            ++stats_.rendered_objects;
            ++scene_rendered_residents_;
            continue;
        }
        const std::size_t cached_vertices =
            cached.vertices.size();
        scene_stream_draws_.push_back({SceneDraw::Kind::vector, 0U,
            static_cast<std::uint32_t>(cached_vertices), nullptr, object});
        scene_total_vertices_ += cached_vertices;
        if (!scene_streamed_
            && (cached.paged || scene_geometry_.size() + cached_vertices > scene_batch_vertex_count)) {
            scene_streamed_ = true;
            scene_geometry_.resize(scene_background_vertices_);
            scene_draws_.clear();
        }
        if (!scene_streamed_) {
            append_cached_geometry(
                scene_geometry_, cached.vertices, camera_position);
        }
        if (cached_vertices != 0U) ++stats_.rendered_objects;
    }
    flush_vector_run();
    const auto objects_end = std::chrono::steady_clock::now();
    stats_.objects_milliseconds =
        std::chrono::duration<double, std::milli>(
            objects_end - objects_start).count();
        scene_signature_ = signature;
        scene_rendered_objects_ = stats_.rendered_objects;
        // A streamed scene is already captured in its raster target; its
        // mesh descriptors are rebuilt before the next raster regeneration.
        scene_valid_ = scene_streamed_ || !resident_scene_invalidated_;
    } else {
        stats_.visible_objects = scene_visible_objects_;
        if (!scene_streamed_) {
            // The draw list can outlive many UI-only frames. Pin every
            // participating mesh, including objects merged into one draw.
            for (const auto id : resident_scene_ids_) {
                const auto found = object_cache_.find(id);
                if (found == object_cache_.end() || found->second.resident.empty()) continue;
                found->second.last_submission = submitted_mesh_serial_ + 1U;
                found->second.last_used_frame = stats_.frame;
                geometry_recency_.touch(id);
            }
        }
        stats_.rendered_objects = scene_rendered_objects_;
    }

    const auto overlay_start = std::chrono::steady_clock::now();
    if (selection_preview != nullptr) {
        if (selection_preview->replacement.has_value()) {
            const Object& replacement = *selection_preview->replacement;
            if (replacement.bounds.intersects(visible)) {
                selection_preview_geometry_.clear();
                tessellate_geometry_bounded(
                    selection_preview_geometry_,
                    selection_preview_points_,
                    replacement.geometry,
                    replacement.style,
                    geometry_detail(
                        replacement.geometry,
                        replacement.style,
                        camera.zoom()),
                    maximum_overlay_content_vertices);
                if (can_append_vertices(
                        geometry_.size(),
                        selection_preview_geometry_.size(),
                        maximum_overlay_content_vertices)) {
                    append_cached_geometry(
                        geometry_,
                        selection_preview_geometry_,
                        camera_position);
                }
            }
        } else {
            for (const auto id : selection_preview->ids) {
                const Object* const object = document.find(id);
                if (object == nullptr
                    || !transformed_object_bounds(
                            *object, selection_preview->transform)
                            .intersects(visible)) {
                    continue;
                }
                const bool resizing =
                    std::abs(selection_preview->transform.scale_x - 1.0)
                        > 1.0e-12
                    || std::abs(
                        selection_preview->transform.scale_y - 1.0)
                        > 1.0e-12;
                if (resizing) {
                    // Scaling an already-tessellated mesh also scales its
                    // stroke thickness. The committed edit intentionally
                    // preserves style, so retessellate transformed centerline
                    // geometry to keep the drag preview visually exact.
                    tessellate_transformed_geometry(
                        selection_preview_geometry_,
                        selection_preview_points_,
                        object->geometry,
                        object->style,
                        camera.zoom(),
                        selection_preview->transform);
                    if (can_append_vertices(
                            geometry_.size(),
                            selection_preview_geometry_.size(),
                            maximum_overlay_content_vertices)) {
                        append_cached_geometry(
                            geometry_,
                            selection_preview_geometry_,
                            camera_position);
                    }
                    continue;
                }
                auto cached = object_cache_.find(id);
                if (cached == object_cache_.end()
                    || cached->second.revision != object->revision) {
                    selection_preview_geometry_.clear();
                    tessellate_geometry_bounded(
                        selection_preview_geometry_,
                        selection_preview_points_,
                        object->geometry,
                        object->style,
                        geometry_detail(
                            object->geometry,
                            object->style,
                            camera.zoom()),
                        maximum_overlay_content_vertices);
                    if (can_append_vertices(
                            geometry_.size(),
                            selection_preview_geometry_.size(),
                            maximum_overlay_content_vertices)) {
                        append_cached_geometry(
                            geometry_,
                            selection_preview_geometry_,
                            camera_position,
                            &selection_preview->transform);
                    }
                    continue;
                }
                cached->second.last_used_frame = stats_.frame;
                geometry_recency_.touch(id);
                const std::size_t cached_vertices =
                    cached->second.vertices.size();
                if (!can_append_vertices(
                        geometry_.size(),
                        cached_vertices,
                        maximum_overlay_content_vertices)) {
                    continue;
                }
                append_cached_geometry(
                    geometry_,
                    cached->second.vertices,
                    camera_position,
                    &selection_preview->transform);
            }
        }
    }
    if (preview != nullptr) {
        const std::uint32_t detail =
            geometry_detail(
                preview->geometry, preview->style, camera.zoom());
        if (const auto* const stroke =
                std::get_if<Stroke>(&preview->geometry)) {
            const std::size_t segment_budget = maximum_stroke_segments(
                detail, maximum_vertex_count);
            std::size_t render_stride = 1U;
            if (stroke->points.size() > segment_budget + 1U
                && polyline_exceeds_vertex_budget(
                    stroke->points, detail, maximum_vertex_count)) {
                render_stride = std::max<std::size_t>(
                    2U,
                    (stroke->points.size() - 1U + segment_budget - 1U)
                        / segment_budget);
            }
            const std::vector<Vec2d>* render_points = &stroke->points;
            if (render_stride > 1U) {
                draft_render_points_.clear();
                draft_render_points_.reserve(
                    (stroke->points.size() + render_stride - 1U)
                        / render_stride + 1U);
                for (std::size_t point = 0U;
                     point < stroke->points.size();
                     point += render_stride) {
                    draft_render_points_.push_back(stroke->points[point]);
                }
                if (!stroke->points.empty()
                    && draft_render_points_.back()
                        != stroke->points.back()) {
                    draft_render_points_.push_back(stroke->points.back());
                }
                render_points = &draft_render_points_;
            }
            // The final round cap is the only old geometry affected by new
            // samples. Replace it, append the new joins/segments and upload
            // that changed tail into the dedicated draft buffer.
            bool mesh_changed = false;
            std::size_t first_changed_vertex = 0U;
            const bool camera_changed =
                draft_gpu_camera_position_ != camera_position;
            CameraRelativeGeometry draft_output{
                draft_gpu_geometry_, camera_position};
            const bool reset =
                !draft_cache_active_
                || !draft_cache_is_stroke_
                || draft_generation_ != preview->generation
                || draft_detail_ != detail
                || draft_render_stride_ != render_stride
                || draft_revision_ > preview->revision
                || draft_point_count_ > render_points->size()
                || camera_changed
                || (draft_gpu_geometry_.empty()
                    && !render_points->empty())
                // Uniform decimation can change retained samples throughout
                // the path as the raw point count grows; rebuild instead of
                // assuming the sampled prefix is stable.
                || (render_stride > 1U
                    && draft_revision_ != preview->revision);
            bool extended = false;
            if (!reset
                && draft_revision_ != preview->revision
                && draft_point_count_ < render_points->size()) {
                const std::size_t appended_points =
                    render_points->size() - draft_point_count_;
                const std::size_t previous_end_cap_offset =
                    draft_end_cap_offset_;
                extended = extend_tessellated_polyline(
                    draft_output,
                    *render_points,
                    draft_point_count_,
                    draft_end_cap_offset_,
                    preview->style,
                    detail);
                if (extended) {
                    mesh_changed = true;
                    first_changed_vertex = previous_end_cap_offset;
                    stats_.draft_appended_points = appended_points;
                }
            }
            if (reset || (!extended
                          && draft_revision_ != preview->revision)) {
                draft_output.clear();
                tessellate_polyline(
                    draft_output,
                    *render_points,
                    preview->style,
                    detail);
                const std::size_t cap_values =
                    round_cap_value_count(detail);
                draft_end_cap_offset_ =
                    render_points->size() >= 2U
                        && draft_output.size() >= cap_values
                    ? draft_output.size() - cap_values
                    : draft_output.size();
                mesh_changed = true;
                first_changed_vertex = 0U;
                ++stats_.draft_full_rebuilds;
            }
            draft_generation_ = preview->generation;
            draft_revision_ = preview->revision;
            draft_detail_ = detail;
            draft_point_count_ = render_points->size();
            draft_render_stride_ = render_stride;
            draft_cache_active_ = true;
            draft_cache_is_stroke_ = true;
            draft_active_this_frame_ = true;

            const std::size_t draft_vertex_count =
                draft_output.size();
            if (mesh_changed) {
                if (draft_vertex_count > maximum_vertex_count) {
                    draft_output.resize(
                        maximum_vertex_count
                        - maximum_vertex_count % 3U);
                }
                draft_upload_first_vertex_ = draft_upload_pending_
                    ? std::min(
                        draft_upload_first_vertex_,
                        first_changed_vertex)
                    : first_changed_vertex;
                draft_upload_pending_ = true;
                draft_gpu_camera_position_ = camera_position;
            }
        } else {
            draft_upload_pending_ = false;
            if (!draft_cache_active_
                || draft_cache_is_stroke_
                || draft_generation_ != preview->generation
                || draft_revision_ != preview->revision
                || draft_detail_ != detail) {
                tessellate_geometry(
                    draft_shape_geometry_,
                    preview->geometry,
                    preview->style,
                    detail);
                draft_generation_ = preview->generation;
                draft_revision_ = preview->revision;
                draft_detail_ = detail;
                draft_cache_active_ = true;
                draft_cache_is_stroke_ = false;
            }
            if (can_append_vertices(
                    geometry_.size(),
                    draft_shape_geometry_.size(),
                    maximum_overlay_content_vertices)) {
                append_cached_geometry(
                    geometry_, draft_shape_geometry_, camera_position);
            }
        }
    } else {
        draft_cache_active_ = false;
        draft_point_count_ = 0U;
        draft_gpu_geometry_.clear();
        draft_upload_pending_ = false;
    }

    // Only copied drawing previews are present here. Keep UI, selection handles
    // and the pointer indicator on their own interface palette.
    const double preview_theme_amount = board_theme_amount(toolbar);
    if (preview_theme_amount > 0.0) {
        for (auto& vertex : geometry_) {
            const auto color = board_display_color(
                {vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]}, preview_theme_amount);
            vertex.color = {color.red, color.green, color.blue, color.alpha};
        }
    }
    append_selection_geometry(
        geometry_,
        geometry_spans_,
        camera,
        document,
        selection,
        selection_preview);
    if (drawing_cursor != nullptr) {
        constexpr std::uint32_t cursor_segments = 20U;
        constexpr std::size_t cursor_vertex_count =
            static_cast<std::size_t>(cursor_segments) * 6U;
        if (can_append_vertices(
                geometry_.size(),
                cursor_vertex_count,
                maximum_overlay_content_vertices)) {
            const double radius = std::clamp(
                drawing_cursor->diameter * 0.5, 2.0, 96.0);
            // A dark brush-sized core with a light rim stays visible over
            // both themes and shows exactly where a draw will begin.
            append_screen_circle(
                geometry_,
                drawing_cursor->screen_position,
                radius + 1.25,
                camera,
                {0.96F, 0.97F, 0.99F},
                cursor_segments);
            append_screen_circle(
                geometry_,
                drawing_cursor->screen_position,
                radius,
                camera,
                {0.06F, 0.075F, 0.10F},
                cursor_segments);
        }
    }
    Vec2d tooltip_extent{};
    if (const UiControl* described = toolbar.tooltip_control();
        described != nullptr && !described->tooltip.empty()) {
        static_cast<void>(prepare_text_frame(toolbar.viewport_width(),
            toolbar.viewport_height(), toolbar.scale()));
        tooltip_extent = tooltip_text_extent(toolbar, *described);
    }
    append_toolbar_geometry(geometry_, geometry_spans_, camera, toolbar,
        tooltip_extent.x, tooltip_extent.y, tooltip_allowed_this_frame_, tooltip_first_vertex_);
    trim_geometry_to_vertex_budget(geometry_, geometry_spans_);
    const auto overlay_end = std::chrono::steady_clock::now();
    stats_.overlay_ui_milliseconds =
        std::chrono::duration<double, std::milli>(
            overlay_end - overlay_start).count();

    prune_cache();
    // Scene draws retain raw texture handles. Prune only while rebuilding the
    // draw list, after every texture referenced by the new list was touched.
    if (rebuild_scene) prune_image_cache();

    if (scene_geometry_.size() > scene_batch_vertex_count) {
        throw std::runtime_error{"Scene exceeded its vertex budget"};
    }
    if (geometry_.size() > maximum_vertex_count) {
        throw std::runtime_error{"Overlay exceeded its vertex budget"};
    }
}

void GpuRenderer::collect_diagnostics()
{
    const auto properties = SDL_GetGPUDeviceProperties(device_);
    std::ostringstream output;
    output
        << "GPU driver=" << SDL_GetGPUDeviceDriver(device_)
        << ", device="
        << property_or_unknown(properties, SDL_PROP_GPU_DEVICE_NAME_STRING)
        << ", driver-version="
        << property_or_unknown(
               properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING)
        << ", drawing-antialiasing=";
    if (antialiasing_samples_ > 1) {
        output << antialiasing_samples_ << "x MSAA";
    } else {
        output << "off";
    }
    diagnostics_ = output.str();
}

void GpuRenderer::release() noexcept
{
    if (device_ == nullptr) {
        return;
    }

    static_cast<void>(SDL_WaitForGPUIdle(device_));

    for (const auto& [key, cached] : text_cache_) {
        static_cast<void>(key);
        TTF_DestroyText(cached.text);
    }
    text_cache_.clear();
    text_width_cache_.clear();
    fitted_text_cache_.clear();
    for (const auto& [key, cached] : thumbnail_textures_) {
        static_cast<void>(key);
        SDL_ReleaseGPUTexture(device_, cached.texture);
    }
    thumbnail_textures_.clear();
    thumbnail_upload_pixels_.clear();
    thumbnail_uploads_.clear();
    for (const auto& [id, cached] : image_textures_) {
        static_cast<void>(id);
        for (const ImageTile& tile : cached.tiles) {
            SDL_ReleaseGPUTexture(device_, tile.texture);
        }
    }
    image_textures_.clear();
    image_upload_pixels_.clear();
    image_uploads_.clear();
    image_decode_cache_.clear();
    if (text_engine_ != nullptr) {
        TTF_DestroyGPUTextEngine(text_engine_);
        text_engine_ = nullptr;
    }
    if (font_ != nullptr) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
    if (secondary_font_ != nullptr) {
        TTF_CloseFont(secondary_font_);
        secondary_font_ = nullptr;
    }
    if (bold_font_ != nullptr) {
        TTF_CloseFont(bold_font_);
        bold_font_ = nullptr;
    }
    if (title_font_ != nullptr) {
        TTF_CloseFont(title_font_);
        title_font_ = nullptr;
    }
    if (document_font_ != nullptr) {
        TTF_CloseFont(document_font_);
        document_font_ = nullptr;
    }
    if (caption_font_ != nullptr) {
        TTF_CloseFont(caption_font_);
        caption_font_ = nullptr;
    }
    TTF_Quit();

    if (text_pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, text_pipeline_);
        text_pipeline_ = nullptr;
    }
    if (image_pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, image_pipeline_);
        image_pipeline_ = nullptr;
    }
    if (image_pipeline_direct_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, image_pipeline_direct_);
        image_pipeline_direct_ = nullptr;
    }
    if (image_vertex_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, image_vertex_buffer_);
        image_vertex_buffer_ = nullptr;
    }
    if (image_sampler_ != nullptr) {
        SDL_ReleaseGPUSampler(device_, image_sampler_);
        image_sampler_ = nullptr;
    }
    if (text_vertex_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, text_vertex_buffer_);
        text_vertex_buffer_ = nullptr;
    }
    if (text_index_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, text_index_buffer_);
        text_index_buffer_ = nullptr;
    }
    if (text_transfer_buffer_ != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, text_transfer_buffer_);
        text_transfer_buffer_ = nullptr;
    }
    if (text_sampler_ != nullptr) {
        SDL_ReleaseGPUSampler(device_, text_sampler_);
        text_sampler_ = nullptr;
    }
    if (msaa_texture_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, msaa_texture_);
        msaa_texture_ = nullptr;
    }
    if (msaa_resolve_texture_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, msaa_resolve_texture_);
        msaa_resolve_texture_ = nullptr;
    }
    release_mesh_storage();
    if (retained_pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, retained_pipeline_);
        retained_pipeline_ = nullptr;
    }
    if (retained_pipeline_direct_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, retained_pipeline_direct_);
        retained_pipeline_direct_ = nullptr;
    }
    if (background_pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, background_pipeline_);
        background_pipeline_ = nullptr;
    }
    if (background_pipeline_direct_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, background_pipeline_direct_);
        background_pipeline_direct_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (blend_pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, blend_pipeline_);
        blend_pipeline_ = nullptr;
    }
    if (pipeline_direct_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_direct_);
        pipeline_direct_ = nullptr;
    }
    if (blend_pipeline_direct_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, blend_pipeline_direct_);
        blend_pipeline_direct_ = nullptr;
    }
    if (text_pipeline_direct_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_, text_pipeline_direct_);
        text_pipeline_direct_ = nullptr;
    }
    if (scene_vertex_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, scene_vertex_buffer_);
        scene_vertex_buffer_ = nullptr;
    }
    release_batch_slots();
    if (scene_stream_texture_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, scene_stream_texture_);
        scene_stream_texture_ = nullptr;
    }
    if (scene_stream_quad_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, scene_stream_quad_);
        scene_stream_quad_ = nullptr;
    }
    if (pixel_readback_surface_ != nullptr) {
        SDL_ReleaseGPUTexture(device_, pixel_readback_surface_);
        pixel_readback_surface_ = nullptr;
    }
    if (pixel_readback_transfer_ != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, pixel_readback_transfer_);
        pixel_readback_transfer_ = nullptr;
    }
    pixel_readback_width_ = pixel_readback_height_ = 0U;
    pixel_readback_position_.reset();
    pixel_readback_result_.reset();
    scene_stream_width_ = scene_stream_height_ = 0U;
    scene_stream_capacity_width_ = scene_stream_capacity_height_ = 0U;
    if (draft_vertex_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, draft_vertex_buffer_);
        draft_vertex_buffer_ = nullptr;
    }
    if (vertex_buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
        vertex_buffer_ = nullptr;
    }
    if (transfer_buffer_ != nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
        transfer_buffer_ = nullptr;
    }
    if (window_claimed_) {
        SDL_ReleaseWindowFromGPUDevice(device_, window_);
        window_claimed_ = false;
    }

    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
}

} // namespace sawer
