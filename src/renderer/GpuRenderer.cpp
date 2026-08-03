#include "renderer/GpuRenderer.hpp"

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
        || action == UiAction::color_target_stroke
        || action == UiAction::color_target_fill
        || action == UiAction::fill_none
        || (action >= UiAction::width_thin
            && action <= UiAction::width_increase)
        || (action >= UiAction::roundness_square
            && action <= UiAction::roundness_full)
        || (action >= UiAction::stabilization_off
            && action <= UiAction::stabilization_strong);
}

std::optional<UiRect> tooltip_bounds(
    const Toolbar& toolbar, const UiControl* const control) noexcept
{
    if (control == nullptr || control->tooltip.empty()) {
        return std::nullopt;
    }
    const double scale = toolbar.scale();
    const double margin = 6.0 * scale;
    const double spacing = 12.0 * scale;
    const double width = std::min(
        toolbar.viewport_width() - margin * 2.0,
        static_cast<double>(control->tooltip.size()) * 7.4 * scale
            + 24.0 * scale);
    const double height = 30.0 * scale;

    const bool left_rail =
        control->bounds.x < 80.0 * scale
        && control->bounds.y >= toolbar.height();
    if (left_rail) {
        // The open contextual sidecar already identifies the active tool.
        // Suppressing that tool's tooltip avoids covering both surfaces.
        if (control->selected && is_tool_action(control->action)
            && toolbar.find(UiAction::width_cycle) != nullptr) {
            return std::nullopt;
        }
        const double x = control->bounds.x + control->bounds.width
            + 10.0 * scale;
        const double y = std::clamp(
            control->bounds.y + (control->bounds.height - height) * 0.5,
            margin,
            std::max(margin, toolbar.viewport_height() - height - margin));
        return UiRect{x, y, width, height};
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
    return mix_palette(
        interface_palette(home.previous_theme() == Theme::light),
        interface_palette(home.theme() == Theme::light),
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
    spans.push_back(GeometrySpan{vertex, value});
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
        // Lucide "folder-open".
        svg_line(6.0, 14.0, 7.5, 11.1);
        svg_arc(7.5, 11.1, 9.24, 10.0, 2.0, 2.0, false, true);
        svg_line(9.24, 10.0, 20.0, 10.0);
        svg_arc(20.0, 10.0, 21.94, 12.5, 2.0, 2.0, false, true);
        svg_line(21.94, 12.5, 20.4, 18.5);
        svg_arc(20.4, 18.5, 18.45, 20.0, 2.0, 2.0, false, true);
        svg_line(18.45, 20.0, 4.0, 20.0);
        svg_arc(4.0, 20.0, 2.0, 18.0, 2.0, 2.0, false, true);
        svg_line(2.0, 18.0, 2.0, 5.0);
        svg_arc(2.0, 5.0, 4.0, 3.0, 2.0, 2.0, false, true);
        svg_line(4.0, 3.0, 7.9, 3.0);
        svg_arc(7.9, 3.0, 9.59, 3.9, 2.0, 2.0, false, true);
        svg_line(9.59, 3.9, 10.4, 5.1);
        svg_arc(10.4, 5.1, 12.07, 6.0, 2.0, 2.0, false, false);
        svg_line(12.07, 6.0, 18.0, 6.0);
        svg_arc(18.0, 6.0, 20.0, 8.0, 2.0, 2.0, false, true);
        svg_line(20.0, 8.0, 20.0, 10.0);
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
    case UiIcon::background:
        // Lucide "grid-2x2".
        svg_line(12.0, 3.0, 12.0, 21.0);
        svg_line(3.0, 12.0, 21.0, 12.0);
        rounded_box(-9.0, -9.0, 18.0, 18.0, 2.0);
        break;
    case UiIcon::custom_color: {
        // Lucide "pipette" converted from its 24-unit SVG path to Sawer's
        // native rounded line geometry. SVG endpoint arcs are flattened here
        // so the source silhouette is preserved without runtime SVG loading.
        svg_line(12.0, 9.0, 3.586, 17.414);
        svg_arc(3.586, 17.414, 3.0, 18.828, 2.0, 2.0, false, false);
        svg_line(3.0, 18.828, 3.0, 20.172);
        svg_arc(3.0, 20.172, 2.414, 21.586, 2.0, 2.0, false, true);
        svg_arc(2.414, 21.586, 3.828, 21.0, 2.0, 2.0, false, true);
        svg_line(3.828, 21.0, 5.172, 21.0);
        svg_arc(5.172, 21.0, 6.586, 20.414, 2.0, 2.0, false, true);
        svg_line(6.586, 20.414, 15.0, 12.0);

        svg_line(18.0, 9.0, 18.4, 9.4);
        svg_arc(18.4, 9.4, 15.4, 12.4, 1.0, 1.0, true, true);
        svg_line(15.4, 12.4, 11.6, 8.6);
        svg_arc(11.6, 8.6, 14.6, 5.6, 1.0, 1.0, true, true);
        svg_line(14.6, 5.6, 15.0, 6.0);
        svg_line(15.0, 6.0, 18.4, 2.6);
        svg_arc(18.4, 2.6, 21.4, 5.6, 1.0, 1.0, true, true);
        svg_line(21.4, 5.6, 18.0, 9.0);

        svg_line(2.0, 22.0, 2.414, 21.586);
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

// The grid is anchored to fixed canvas coordinates with a constant world-space
// spacing, so its cells/dots keep their position and scale with zoom: larger
// when zooming in, smaller when zooming out.
constexpr double grid_world_spacing = 50.0;

// Draws the board's background pattern (dots, squares, rules, diamonds, ...)
// into the world-space geometry buffer. Line colors are derived from the
// chosen background color's luminance so any swatch stays legible.
void append_background_pattern(
    std::vector<GeometryVertex>& output,
    const Vec2d camera_position,
    const BackgroundStyle style,
    const RenderColor background,
    const std::optional<Color> custom_grid_color,
    const double min_x,
    const double min_y,
    const double max_x,
    const double max_y,
    const double step,
    const double zoom)
{
    if (style == BackgroundStyle::solid) {
        return;
    }

    const float luminance = 0.299F * background.red
        + 0.587F * background.green + 0.114F * background.blue;
    const RenderColor ink = custom_grid_color.has_value()
        ? to_render_color(*custom_grid_color)
        : (luminance > 0.5F
            ? RenderColor{0.0F, 0.0F, 0.0F}
            : RenderColor{1.0F, 1.0F, 1.0F});
    const RenderColor minor = mix_color(
        background, ink, custom_grid_color.has_value() ? 0.65 : 0.15);
    const RenderColor major = mix_color(
        background, ink, custom_grid_color.has_value() ? 0.84 : 0.32);
    const RenderColor axis = mix_color(
        background, ink, custom_grid_color.has_value() ? 1.0 : 0.48);
    const RenderColor dots = mix_color(
        background, ink, custom_grid_color.has_value() ? 0.78 : 0.26);

    // Widths and dot sizes are in world units so the pattern scales with zoom
    // instead of staying a fixed number of screen pixels.
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
        const double r = gstep * 0.045;
        // Dots are round; when they shrink below a couple of screen pixels a
        // filled quad is indistinguishable from a circle and far cheaper, which
        // also keeps the vertex count bounded when zoomed far out.
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
        dot_lattice(step);
        break;
    case BackgroundStyle::square:
        vertical_lines(step, true);
        horizontal_lines(step, true, true);
        break;
    case BackgroundStyle::graph:
        vertical_lines(step / 5.0, true);
        horizontal_lines(step / 5.0, true, true);
        break;
    case BackgroundStyle::hybrid:
        vertical_lines(step, false);
        horizontal_lines(step, false, true);
        dot_lattice(step);
        break;
    case BackgroundStyle::diamond:
        diagonal_family(1.0, step);
        diagonal_family(-1.0, step);
        break;
    case BackgroundStyle::wide_rule:
        horizontal_lines(step, false, false);
        break;
    case BackgroundStyle::narrow_rule:
        horizontal_lines(step * 0.5, false, false);
        break;
    case BackgroundStyle::triangle:
        horizontal_lines(step, false, true);
        diagonal_family(1.7320508, step * 2.0);
        diagonal_family(-1.7320508, step * 2.0);
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
        UiRect bounds = home.status_bounds();
        bounds.y += panel_shift;
        append_screen_rounded_rect(
            output, bounds, 7.0 * scale, camera,
            mix_color(
                card_border,
                danger,
                (previous_light ? 0.48 : 0.62)
                    + ((light ? 0.48 : 0.62)
                        - (previous_light ? 0.48 : 0.62))
                        * theme_amount));
        UiRect inside = bounds;
        inside.x += 1.0 * scale;
        inside.y += 1.0 * scale;
        inside.width -= 2.0 * scale;
        inside.height -= 2.0 * scale;
        append_screen_rounded_rect(
            output, inside, 6.0 * scale, camera,
            mix_color(
                previous_light
                    ? RenderColor{1.0F, 0.955F, 0.958F}
                    : RenderColor{0.17F, 0.075F, 0.085F},
                light
                    ? RenderColor{1.0F, 0.955F, 0.958F}
                    : RenderColor{0.17F, 0.075F, 0.085F},
                theme_amount));

        const Vec2d center{
            bounds.x + 21.0 * scale,
            bounds.y + bounds.height * 0.5,
        };
        append_screen_circle(
            output, center, 11.0 * scale, camera, danger, 24U);
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
        const double hover = animation.hover;
        UiRect bounds = control.bounds;
        bounds.y += home.control_offset(index);
        if (board_card
            && (control.bounds.y + control.bounds.height <= home.grid_top()
                || bounds.y > home.viewport_height() + vertical_margin)) {
            continue;
        }
        const double corner = 8.0 * scale;
        UiRect surface_bounds = bounds;
        if (board_card) {
            surface_bounds.height -= HomeView::card_text_area * scale;
        }
        if (focused_control == &control) {
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

        // Surfaces fade into the neutral canvas without elevation.
        const auto entering = [&](const RenderColor color) {
            return mix_color(
                backdrop, color, 0.3 + 0.7 * entrance);
        };

        const RenderColor border = control.selected
            ? entering(accent)
            : entering(mix_color(card_border, accent, hover * 0.32));
        append_screen_rounded_rect(
            output, surface_bounds, corner, camera, border);

        UiRect inner = surface_bounds;
        inner.x += 1.0 * scale;
        inner.y += 1.0 * scale;
        inner.width -= 2.0 * scale;
        inner.height -= 2.0 * scale;
        if (control.selected) {
            const double active_state = std::clamp(
                hover * 0.72 + animation.press * 0.62, 0.0, 1.0);
            const RenderColor fill =
                mix_color(accent, accent_hover, active_state);
            append_screen_rounded_rect(
                output, inner, corner - 1.0 * scale, camera,
                entering(fill));
        } else {
            const RenderColor idle_surface =
                board_card ? preview_surface : card_surface;
            RenderColor fill =
                mix_color(idle_surface, hovered, hover * 0.72);
            fill = mix_color(
                fill, palette.accent_soft, animation.press * 0.22);
            append_screen_rounded_rect(
                output, inner, corner - 1.0 * scale, camera,
                entering(fill));
        }

        if (board_card) {
            const UiRect preview = inner;

            const RenderColor dot_color = entering(palette.preview_dots);
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
                        std::max(0.95 * scale, 0.95),
                        camera, dot_color, 10U);
                }
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
            const RenderColor icon_color = control.selected
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
        const double card_hover = home.animation(index).hover;
        const bool card_focused =
            index < controls.size() && focused_control == &controls[index];
        const double visibility = std::clamp(
            card_hover * 0.78 + hover + (card_focused ? 1.0 : 0.0),
            0.0,
            1.0);
        set_geometry_alpha(spans, output, visibility);
        append_screen_rounded_rect(
            output, chip, 6.0 * scale, camera,
            mix_color(card_border, accent, hover * 0.5));
        UiRect chip_inner = chip;
        chip_inner.x += 1.0 * scale;
        chip_inner.y += 1.0 * scale;
        chip_inner.width -= 2.0 * scale;
        chip_inner.height -= 2.0 * scale;
        append_screen_rounded_rect(
            output, chip_inner, 5.0 * scale, camera,
            mix_color(card_surface, hovered, hover * 0.8));
        append_icon(
            output, pencil, chip_inner, camera,
            mix_color(text, accent, hover * 0.8), scale);
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
            camera, mix_color(card_border, accent, 0.34));
        set_geometry_alpha(spans, output, 1.0);
    }
}

void append_toolbar_geometry(
    std::vector<GeometryVertex>& output,
    std::vector<GeometrySpan>& spans,
    const Camera& camera,
    const Toolbar& toolbar)
{
    const bool light = toolbar.theme() == Theme::light;
    const bool previous_light =
        toolbar.previous_theme() == Theme::light;
    const double theme_amount = smooth_theme_transition(toolbar);
    const double scale = toolbar.scale();
    const double reveal = toolbar.reveal();
    const double settings_reveal = toolbar.settings_reveal();
    const bool style_color_modal = toolbar.style_color_editor_open();
    constexpr double modal_background_alpha = 0.38;
    const double background_chrome_alpha =
        style_color_modal ? modal_background_alpha : 1.0;
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

    // Entrance: chrome nearest the top slides down while lower controls rise;
    // both settle as reveal approaches one.
    const auto slide = [&](const double y) {
        const double offset = (1.0 - reveal) * 18.0 * scale;
        return y < toolbar.viewport_height() * 0.5 ? -offset : offset;
    };
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
    if (style_color_modal
        && settings_panel_index < toolbar.panels().size()) {
        modal_bounds = shifted(
            toolbar.panels()[settings_panel_index].bounds);
        modal_bounds->y +=
            (1.0 - settings_reveal) * 10.0 * scale;
    }
    const auto occluded_by_modal = [&](const UiRect bounds) {
        return modal_bounds.has_value()
            && bounds.x < modal_bounds->x + modal_bounds->width
            && bounds.x + bounds.width > modal_bounds->x
            && bounds.y < modal_bounds->y + modal_bounds->height
            && bounds.y + bounds.height > modal_bounds->y;
    };

    // A short bridge makes the vertical context rail read as a sidecar
    // belonging to the selected tool without merging the control groups.
    if (toolbar.find(UiAction::width_cycle) != nullptr
        && toolbar.panels().size() >= 3U) {
        set_geometry_alpha(spans, output, background_chrome_alpha);
        constexpr std::array tool_actions{
            UiAction::select,
            UiAction::hand,
            UiAction::pencil,
            UiAction::line,
            UiAction::rectangle,
            UiAction::ellipse,
        };
        const UiControl* active_tool = nullptr;
        for (const UiAction action : tool_actions) {
            const UiControl* const control = toolbar.find(action);
            if (control != nullptr && control->selected) {
                active_tool = control;
                break;
            }
        }
        if (active_tool != nullptr) {
            const UiRect tool_panel = shifted(toolbar.panels()[1U].bounds);
            const UiRect style_panel = shifted(toolbar.panels()[2U].bounds);
            const UiRect active_bounds = shifted(active_tool->bounds);
            const double bridge_left =
                tool_panel.x + tool_panel.width - 1.0 * scale;
            const double bridge_right = style_panel.x + 1.0 * scale;
            const double bridge_width = bridge_right - bridge_left;
            const double center_y =
                active_bounds.y + active_bounds.height * 0.5;
            if (bridge_width > 0.0) {
                append_screen_quad(
                    output,
                    {
                        bridge_left,
                        center_y - 5.0 * scale,
                        bridge_width,
                        10.0 * scale,
                    },
                    camera,
                    panel_border);
                append_screen_quad(
                    output,
                    {
                        bridge_left,
                        center_y - 4.0 * scale,
                        bridge_width,
                        8.0 * scale,
                    },
                    camera,
                    panel);
            }
        }
        set_geometry_alpha(spans, output, 1.0);
    }

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
        if (settings_panel) {
            bounds.y += (1.0 - settings_reveal) * 10.0 * scale;
        }
        if (application_bar) {
            set_geometry_alpha(spans, output, panel_alpha);
            append_screen_quad(
                output, bounds, camera, palette.background);
            append_screen_quad(
                output,
                {
                    bounds.x,
                    bounds.y + bounds.height - std::max(scale, 1.0),
                    bounds.width,
                    std::max(scale, 1.0),
                },
                camera, panel_border);
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
        const double panel_corner =
            (settings_panel ? 10.0 : 8.0) * scale;
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

    set_geometry_alpha(spans, output, background_chrome_alpha);
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

    const UiControl* const hovered_control = toolbar.hovered_control();
    const UiControl* const focused_control = toolbar.focused_control();
    for (const auto& control : toolbar.controls()) {
        const bool in_settings = is_settings_control(toolbar, &control);
        if (!in_settings
            && occluded_by_modal(shifted(control.bounds))) {
            continue;
        }
        const double control_alpha = in_settings
            ? settings_reveal
            : background_chrome_alpha;
        if (control_alpha <= 0.01) {
            continue;
        }
        const double settings_rise = in_settings
            ? (1.0 - settings_reveal) * 10.0 * scale
            : 0.0;
        if (control.action == UiAction::custom_hue_field) {
            UiRect field = shifted(control.bounds);
            field.y += settings_rise;
            set_geometry_alpha(spans, output, control_alpha);
            append_screen_rounded_rect(
                output, field, 8.0 * scale, camera, panel_border);
            // HSV hue is exactly piecewise linear across its six sectors.
            // Interpolating the sector endpoints removes the visible 36-band
            // approximation without increasing the geometry budget.
            constexpr std::uint32_t hue_sectors = 6U;
            const double inset = 2.0 * scale;
            const double width = field.width - inset * 2.0;
            for (std::uint32_t sector = 0U;
                 sector < hue_sectors;
                 ++sector) {
                const double first = static_cast<double>(sector)
                    / static_cast<double>(hue_sectors);
                const double second = static_cast<double>(sector + 1U)
                    / static_cast<double>(hue_sectors);
                append_screen_quad_gradient_horizontal(
                    output,
                    {field.x + inset + width * first,
                     field.y + inset,
                     width * (second - first),
                     field.height - inset * 2.0},
                    camera,
                    hsv_render_color(first, 1.0, 1.0),
                    hsv_render_color(second, 1.0, 1.0));
            }
            const Vec2d marker{
                field.x + inset + width * toolbar.custom_hue(),
                field.y + field.height * 0.5,
            };
            append_screen_circle(
                output, marker, 6.2 * scale, camera,
                {0.07F, 0.08F, 0.11F}, 24U);
            append_screen_circle(
                output, marker, 5.0 * scale, camera,
                {1.0F, 1.0F, 1.0F}, 20U);
            append_screen_circle(
                output, marker, 2.8 * scale, camera,
                hsv_render_color(toolbar.custom_hue(), 1.0, 1.0), 20U);
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
            const double inset = 2.0 * scale;
            const double width = field.width - inset * 2.0;
            const double height = field.height - inset * 2.0;
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
                        {field.x + inset
                                + width * static_cast<double>(column)
                                    / static_cast<double>(columns),
                         field.y + inset
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
                field.x + inset + width * toolbar.custom_saturation(),
                field.y + inset + height * (1.0 - toolbar.custom_value()),
            };
            append_screen_circle(
                output, marker, 7.0 * scale, camera,
                {0.07F, 0.08F, 0.11F}, 24U);
            append_screen_circle(
                output, marker, 5.7 * scale, camera,
                {1.0F, 1.0F, 1.0F}, 20U);
            append_screen_circle(
                output, marker, 3.6 * scale, camera,
                to_render_color(toolbar.custom_color()), 20U);
            set_geometry_alpha(spans, output, 1.0);
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
        const bool no_fill_control =
            control.action == UiAction::fill_none;
        const bool custom_color_control =
            control.action == UiAction::edit_stroke_custom
            && control.accent.has_value();
        const bool soft_property =
            uses_soft_property_selection(control.action);
        const bool history_control =
            control.action == UiAction::undo
            || control.action == UiAction::redo;
        const bool top_bar_control =
            control.bounds.y < toolbar.height();
        const bool quiet_chrome = control.label.empty()
            && !in_settings;
        RenderColor background = information
            ? panel
            : (control.enabled
                ? (top_bar_control
                    ? top_bar_button
                    : (quiet_chrome && !history_control ? panel : button))
                : (history_control
                    ? (top_bar_control ? top_bar_button : button)
                    : disabled));
        background = mix_color(background, hovered, animation.hover);
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

        const double corner = 7.0 * scale;
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
        if (custom_color_control) {
            background = mix_color(
                panel_border, accent, animation.selected);
        }
        const bool outlined_property_control =
            color_well || no_fill_control;
        if (outlined_property_control && animation.selected > 0.02) {
            append_screen_rounded_rect(
                output,
                bounds,
                corner,
                camera,
                mix_color(panel_border, accent, animation.selected));
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

        if (custom_color_control) {
            UiRect inner = bounds;
            inner.x += 2.0 * scale;
            inner.y += 2.0 * scale;
            inner.width -= 4.0 * scale;
            inner.height -= 4.0 * scale;
            append_screen_rounded_rect(
                output,
                inner,
                std::max(2.0 * scale, corner - 2.0 * scale),
                camera,
                to_render_color(*control.accent));
        } else if (swatch_control) {
            const Vec2d center{
                bounds.x + bounds.width * 0.5,
                bounds.y + bounds.height * 0.5,
            };
            // Selected colors use the same cobalt outline language as board
            // focus without turning the whole swatch tile into a blue button.
            const double swatch_radius =
                std::min(bounds.width, bounds.height) * 0.30;
            if (animation.selected > 0.02) {
                append_screen_circle(
                    output, center,
                    swatch_radius + 4.0 * scale,
                    camera, accent, 28U);
                append_screen_circle(
                    output, center,
                    swatch_radius + 2.0 * scale,
                    camera, background, 28U);
            } else {
                append_screen_circle(
                    output, center,
                    swatch_radius + 1.25 * scale,
                    camera, panel_border, 28U);
            }
            append_screen_circle(
                output, center, swatch_radius, camera,
                to_render_color(*control.accent), 28U);
        }
        if (color_well) {
            const Vec2d center{
                bounds.x + 9.0 * scale,
                bounds.y + bounds.height * 0.5,
            };
            append_screen_circle(
                output, center, 4.5 * scale, camera, panel_border, 20U);
            if (control.accent.has_value()) {
                append_screen_circle(
                    output, center, 3.25 * scale, camera,
                    to_render_color(*control.accent), 20U);
            } else {
                append_screen_circle(
                    output, center, 3.25 * scale, camera, panel, 20U);
                append_screen_line(
                    output,
                    {
                        center.x - 2.25 * scale,
                        center.y + 2.25 * scale,
                    },
                    {
                        center.x + 2.25 * scale,
                        center.y - 2.25 * scale,
                    },
                    1.25 * scale,
                    camera,
                    muted);
            }
        }

        if (control.icon != UiIcon::none) {
            RenderColor icon_color = information
                ? muted
                : (control.enabled ? text : muted);
            if (animation.selected > 0.45) {
                icon_color = soft_property
                    ? accent
                    : RenderColor{0.98F, 0.99F, 1.0F};
            }
            if (custom_color_control) {
                const Color color = *control.accent;
                const double luminance =
                    (0.2126 * static_cast<double>(color.red)
                     + 0.7152 * static_cast<double>(color.green)
                     + 0.0722 * static_cast<double>(color.blue))
                    / 255.0;
                icon_color = luminance > 0.58
                    ? RenderColor{0.07F, 0.09F, 0.12F}
                    : RenderColor{0.98F, 0.99F, 1.0F};
            }
            append_icon(
                output, control, bounds, camera, icon_color, scale);
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

    if (toolbar.dirty() && !toolbar.panels().empty()
        && toolbar.panels().back().bounds.width > 1.0
        && toolbar.panels().back().bounds.height > 1.0) {
        const UiRect status = shifted(toolbar.panels().back().bounds);
        if (!occluded_by_modal(status)) {
            const Vec2d center{
                status.x + 15.0 * scale,
                status.y + status.height * 0.5,
            };
            // Keep the unsaved marker quiet and static once the surrounding UI
            // settles so an idle document does not require continuous frames.
            constexpr double pulse = 0.5;
            set_geometry_alpha(
                spans, output,
                (0.10 + 0.14 * pulse) * background_chrome_alpha);
            append_screen_circle(
                output, center, 7.0 * scale, camera,
                {0.97F, 0.45F, 0.35F}, 22U);
            set_geometry_alpha(
                spans, output, background_chrome_alpha);
            append_screen_circle(
                output, center, (3.1 + 0.5 * pulse) * scale, camera,
                {0.97F, 0.42F, 0.32F}, 18U);
            set_geometry_alpha(spans, output, 1.0);
        }
    }

    const UiControl* const tooltip_focused_control =
        toolbar.focused_tooltip_control();
    const UiControl* const described_control =
        hovered_control != nullptr
        ? hovered_control
        : tooltip_focused_control;
    const double described_visibility = hovered_control != nullptr
        ? toolbar.animation(hovered_control->action).hover
        : (tooltip_focused_control != nullptr ? 1.0 : 0.0);
    if (described_control != nullptr && described_visibility > 0.08) {
        const double visibility = described_visibility;
        if (const auto bounds = tooltip_bounds(toolbar, described_control)) {
            UiRect animated = shifted(*bounds);
            animated.y += (1.0 - visibility) * 4.0 * scale;
            append_soft_shadow(
                output, spans, animated, 8.0 * scale, camera, shadow,
                scale, 0.7 * visibility);
            set_geometry_alpha(spans, output, 0.96 * visibility);
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

GpuRenderer::GpuRenderer(SDL_Window& window)
    : window_{&window}
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
    const DrawingCursor* const drawing_cursor)
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
    stats_.visible_objects = 0U;
    stats_.tessellated_objects = 0U;
    stats_.cache_hits = 0U;
    stats_.segment_index_builds = 0U;
    stats_.scene_upload_bytes = 0U;
    stats_.draft_upload_bytes = 0U;
    stats_.geometry_upload_bytes = 0U;
    stats_.text_upload_bytes = 0U;
    stats_.thumbnail_upload_bytes = 0U;
    stats_.image_upload_bytes = 0U;
    stats_.draw_calls = 0U;
    stats_.text_draw_calls = 0U;
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
    const auto build_end = std::chrono::steady_clock::now();
    stats_.build_milliseconds = std::chrono::duration<double, std::milli>(
        build_end - build_start).count();
    stats_.emitted_vertices =
        geometry_.size()
        + (scene_active_this_frame_
            ? scene_geometry_.size()
            : 0U)
        + (draft_active_this_frame_
            ? draft_gpu_geometry_.size()
            : 0U)
        + (scene_active_this_frame_ ? image_vertices_.size() : 0U);
    stats_.cache_entries = object_cache_.size();
    stats_.cache_bytes = geometry_cache_bytes_;
    stats_.evictions = total_evictions_;

    const auto staging_start = std::chrono::steady_clock::now();
    const std::size_t scene_vertex_count =
        scene_geometry_.size();
    if (scene_active_this_frame_
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
    if (scene_active_this_frame_ && scene_upload_pending_) {
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
    const std::size_t total_geometry_upload_bytes =
        static_cast<std::size_t>(image_geometry_transfer_offset)
        + image_geometry_bytes;
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
    SDL_GPUCommandBuffer* const command_buffer =
        SDL_AcquireGPUCommandBuffer(device_);
    if (command_buffer == nullptr) {
        throw_sdl("GPU command-buffer acquisition");
    }
    CommandBufferGuard command_buffer_guard{command_buffer};
    const auto command_acquire_end = std::chrono::steady_clock::now();
    stats_.command_acquire_milliseconds =
        std::chrono::duration<double, std::milli>(
            command_acquire_end - command_acquire_start).count();

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
    for (const auto& upload : image_uploads_) {
        if (upload.mip_levels > 1U) {
            SDL_GenerateMipmapsForGPUTexture(command_buffer, upload.texture);
        }
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
        color_target.texture = swapchain_texture;
        color_target.store_op = SDL_GPU_STOREOP_STORE;
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

    if (scene_active_this_frame_ && !scene_draws_.empty()) {
        const SDL_GPUBufferBinding scene_binding{
            scene_vertex_buffer_, 0U};
        const SDL_GPUBufferBinding image_binding{
            image_vertex_buffer_, 0U};
        SceneDraw::Kind bound_kind = SceneDraw::Kind::vector;
        bool has_bound_kind = false;
        for (const SceneDraw& draw : scene_draws_) {
            if (draw.vertex_count == 0U) continue;
            if (!has_bound_kind || draw.kind != bound_kind) {
                if (draw.kind == SceneDraw::Kind::vector) {
                    SDL_BindGPUGraphicsPipeline(render_pass, opaque_pipeline);
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
        SDL_BindGPUGraphicsPipeline(render_pass, opaque_pipeline);
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

    const SDL_GPUBufferBinding vertex_binding{vertex_buffer_, 0U};
    SDL_BindGPUVertexBuffers(render_pass, 0U, &vertex_binding, 1U);
    const auto total_vertices =
        static_cast<Uint32>(geometry_.size());
    SDL_GPUGraphicsPipeline* bound_pipeline = nullptr;
    float bound_constant = -1.0F;
    for (std::size_t index = 0U; index < geometry_spans_.size(); ++index) {
        const GeometrySpan& span = geometry_spans_[index];
        const Uint32 end_vertex = index + 1U < geometry_spans_.size()
            ? geometry_spans_[index + 1U].first_vertex
            : total_vertices;
        if (end_vertex <= span.first_vertex) {
            continue;
        }
        SDL_GPUGraphicsPipeline* const wanted = span.alpha >= 0.999F
            ? opaque_pipeline
            : translucent_pipeline;
        if (wanted != bound_pipeline) {
            SDL_BindGPUGraphicsPipeline(render_pass, wanted);
            bound_pipeline = wanted;
            // Blend constants may be pipeline-local dynamic state on some
            // backends; force a refresh after every rebind.
            bound_constant = -1.0F;
        }
        if (wanted == translucent_pipeline && span.alpha != bound_constant) {
            SDL_SetGPUBlendConstants(
                render_pass,
                SDL_FColor{span.alpha, span.alpha, span.alpha, span.alpha});
            bound_constant = span.alpha;
        }
        SDL_DrawGPUPrimitives(
            render_pass,
            end_vertex - span.first_vertex,
            1U,
            span.first_vertex,
            0U);
        ++stats_.draw_calls;
    }
    if (!text_vertices_.empty()) {
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
        SDL_BindGPUGraphicsPipeline(render_pass, glyph_pipeline);
        SDL_BindGPUVertexBuffers(
            render_pass, 0U, &text_vertex_binding, 1U);
        SDL_BindGPUIndexBuffer(
            render_pass,
            &text_index_binding,
            SDL_GPU_INDEXELEMENTSIZE_16BIT);
        for (const auto& batch : text_batches_) {
            const SDL_GPUTextureSamplerBinding binding{
                batch.texture, text_sampler_};
            SDL_BindGPUFragmentSamplers(
                render_pass, 0U, &binding, 1U);
            SDL_DrawGPUIndexedPrimitives(
                render_pass,
                batch.index_count,
                1U,
                batch.first_index,
                batch.vertex_offset,
                0U);
            ++stats_.text_draw_calls;
        }
    }
    SDL_EndGPURenderPass(render_pass);
    if (msaa_active) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = msaa_resolve_texture_;
        blit.source.w = drawable_width;
        blit.source.h = drawable_height;
        blit.destination.texture = swapchain_texture;
        blit.destination.w = drawable_width;
        blit.destination.h = drawable_height;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command_buffer, &blit);
    }
    const auto command_record_end = std::chrono::steady_clock::now();
    stats_.command_record_milliseconds =
        std::chrono::duration<double, std::milli>(
            command_record_end - command_record_start).count();

    const auto submit_start = std::chrono::steady_clock::now();
    if (!SDL_SubmitGPUCommandBuffer(command_buffer_guard.release())) {
        throw_sdl("GPU command-buffer submission");
    }
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
    scene_upload_pending_ = false;
    draft_upload_pending_ = false;

    return true;
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
        if (SDL_GPUTextureSupportsSampleCount(
                device_, color_target.format, preferred_sample_count)) {
            antialiasing_samples_ = 4;
        } else if (SDL_GPUTextureSupportsSampleCount(
                       device_, color_target.format, fallback_sample_count)) {
            antialiasing_samples_ = 2;
            log::write(
                log::Level::warning,
                "GPU does not support the preferred 4x MSAA drawing target; "
                "using 2x MSAA");
        } else {
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
    const std::uint32_t width, const std::uint32_t height)
{
    if (antialiasing_samples_ == 1 || width == 0U || height == 0U) {
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
        assets::source_sans_3_regular,
        assets::source_sans_3_regular_size);
    if (font_stream == nullptr) {
        throw_sdl("Embedded font stream creation");
    }
    font_ = TTF_OpenFontIO(font_stream, true, 15.0F);
    if (font_ == nullptr) {
        throw_sdl("Embedded Source Sans 3 Regular font opening");
    }
    TTF_SetFontHinting(font_, TTF_HINTING_LIGHT);

    SDL_IOStream* const bold_font_stream = SDL_IOFromConstMem(
        assets::source_sans_3_medium,
        assets::source_sans_3_medium_size);
    if (bold_font_stream == nullptr) {
        throw_sdl("Embedded bold font stream creation");
    }
    bold_font_ = TTF_OpenFontIO(bold_font_stream, true, 15.0F);
    if (bold_font_ == nullptr) {
        throw_sdl("Embedded Source Sans 3 Medium font opening");
    }
    TTF_SetFontHinting(bold_font_, TTF_HINTING_LIGHT);

    SDL_IOStream* const title_font_stream = SDL_IOFromConstMem(
        assets::source_sans_3_semibold,
        assets::source_sans_3_semibold_size);
    if (title_font_stream == nullptr) {
        throw_sdl("Embedded title font stream creation");
    }
    title_font_ = TTF_OpenFontIO(title_font_stream, true, 28.0F);
    if (title_font_ == nullptr) {
        throw_sdl("Embedded Source Sans 3 Semibold font opening");
    }
    TTF_SetFontHinting(title_font_, TTF_HINTING_LIGHT);

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

void GpuRenderer::queue_text(
    const std::string_view text,
    const double x,
    const double y,
    const std::array<float, 4> color,
    const TextStyle style)
{
    if (text.empty()) return;
    if (color[3] <= 0.004F) return;
    std::string key;
    key.reserve(text.size() + 1U);
    key.push_back(
        style == TextStyle::title ? 'T'
                                  : (style == TextStyle::bold ? 'B' : 'R'));
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
        TTF_Font* const selected_font = style == TextStyle::title
            ? title_font_
            : (style == TextStyle::bold ? bold_font_ : font_);
        TTF_Text* const created = TTF_CreateText(
            text_engine_, selected_font, text.data(), text.size());
        if (created == nullptr) {
            throw_sdl("Text shaping");
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
    key.push_back(
        style == TextStyle::title ? 'T'
                                  : (style == TextStyle::bold ? 'B' : 'R'));
    key.append(text);
    if (const auto found = text_width_cache_.find(key);
        found != text_width_cache_.end()) {
        return found->second;
    }

    TTF_Font* const selected_font = style == TextStyle::title
        ? title_font_
        : (style == TextStyle::bold ? bold_font_ : font_);
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
    key.push_back(
        style == TextStyle::title ? 'T'
                                  : (style == TextStyle::bold ? 'B' : 'R'));
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
    constexpr double base_text_size = 15.0;
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
        if (!TTF_SetFontSize(
                bold_font_, static_cast<float>(desired_raster_size))) {
            throw_sdl("DPI-aware bold font sizing");
        }
        constexpr double title_text_size = 28.0;
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
    constexpr double modal_background_alpha = 0.38;
    const double chrome_reveal = reveal
        * std::clamp(background_opacity, 0.0, 1.0)
        * (toolbar.style_color_editor_open()
            ? modal_background_alpha
            : 1.0);
    std::optional<UiRect> modal_bounds;
    if (toolbar.style_color_editor_open()
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

    // Mirror the geometry entrance: top chrome slides down, the bottom bar
    // rises, and labels fade in alongside their panels.
    const auto slide = [&](const double y) {
        const double offset = (1.0 - reveal) * 18.0 * scale;
        return y < toolbar.viewport_height() * 0.5 ? -offset : offset;
    };
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
        const double text_x = status.x
            + (toolbar.dirty() ? 27.0 : 13.0) * scale;
        const double baseline = centered_text_top(status.y, status.height);
        const std::string raw_filename{toolbar.filename()};
        std::string filename{raw_filename};
        if (const UiControl* const rename =
                toolbar.find(UiAction::rename_board)) {
            const double available = std::max(
                12.0 * scale, rename->bounds.x - text_x - 8.0 * scale);
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
            } else if (!fits(filename)) {
                while (!filename.empty()
                       && !fits(filename + "...")) {
                    filename.erase(previous_codepoint(
                        filename, filename.size()));
                }
                filename.append("...");
                queue_text(filename, text_x, baseline, status_text);
            } else {
                queue_text(filename, text_x, baseline, status_text);
            }
        } else {
            queue_text(filename, text_x, baseline, status_text);
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
        }
    }

    if (const UiControl* const save_as = toolbar.find(UiAction::save_as);
        save_as != nullptr && !save_as->label.empty()) {
        int label_width = 0;
        int label_height = 0;
        if (!TTF_GetStringSize(
                font_, save_as->label.data(), save_as->label.size(),
                &label_width, &label_height)) {
            throw_sdl("Save-label measurement");
        }
        static_cast<void>(label_height);
        queue_text(
            save_as->label,
            save_as->bounds.x
                + (save_as->bounds.width
                    - static_cast<double>(label_width)
                        * text_geometry_scale_) * 0.5,
            centered_text_top(save_as->bounds.y, save_as->bounds.height)
                + slide(save_as->bounds.y),
            faded(text, base_text_alpha(save_as->bounds)),
            TextStyle::bold);
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
            faded(muted, base_text_alpha(zoom->bounds)));
    }

    constexpr std::array context_label_actions{
        UiAction::color_target_stroke,
        UiAction::color_target_fill,
        UiAction::width_cycle,
        UiAction::stabilization_off,
        UiAction::stabilization_light,
        UiAction::stabilization_default,
        UiAction::stabilization_strong,
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
        const bool color_well =
            action == UiAction::color_target_stroke
            || action == UiAction::color_target_fill;
        const double label_area_x = color_well
            ? control->bounds.x + 17.0 * scale
            : control->bounds.x;
        const double label_area_width = color_well
            ? std::max(0.0, control->bounds.width - 20.0 * scale)
            : control->bounds.width;
        const bool soft_selected =
            control->selected && uses_soft_property_selection(action);
        queue_text(
            control->label,
            label_area_x
                + (label_area_width
                    - static_cast<double>(label_width)
                        * text_geometry_scale_) * 0.5,
            centered_text_top(
                control->bounds.y, control->bounds.height)
                + slide(control->bounds.y),
            faded(
                !control->enabled
                    ? muted
                    : (soft_selected
                        ? text_color(palette.primary)
                        : (control->selected ? selected_text : text)),
                base_text_alpha(control->bounds)),
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
                    base_text_alpha({
                        first->bounds.x,
                        first->bounds.y - font_line_height - 2.0 * scale,
                        100.0 * scale,
                        font_line_height + 3.0 * scale,
                    })));
        };
    context_section_label(UiAction::color_white, "Color");
    context_section_label(UiAction::width_thin, "Width");
    context_section_label(UiAction::roundness_square, "Corners");
    context_section_label(
        UiAction::stabilization_off, "Stabilization");

    if (toolbar.settings_open()) {
        // Settings labels fade and rise with the flyout geometry.
        const double settings_alpha = toolbar.settings_reveal();
        const auto settings_shift = [&](const double y) {
            return slide(y) + (1.0 - settings_alpha) * 10.0 * scale;
        };
        const std::array<float, 4> settings_text = faded(text, settings_alpha);
        const std::array<float, 4> settings_muted =
            faded(muted, settings_alpha);
        const std::array<float, 4> settings_selected =
            faded(selected_text, settings_alpha);
        const UiControl* const back = toolbar.find(UiAction::settings_close);
        if (back != nullptr) {
            std::string_view title = "Canvas settings";
            if (toolbar.settings_page() == SettingsPage::canvas) {
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
                back->bounds.x + back->bounds.width + 10.0 * scale,
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
            section_label(UiAction::bg_color_0, "Background color");
            section_label(UiAction::grid_color_auto, "Grid color");
            section_label(UiAction::grid_solid, "Grid pattern");
        } else if (toolbar.settings_page() == SettingsPage::color_editor) {
            section_label(UiAction::custom_hue_field, "Hue");
            section_label(
                UiAction::custom_sv_field, "Saturation / brightness");
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
            UiAction::stabilization_off,
            UiAction::stabilization_light,
            UiAction::stabilization_default,
            UiAction::stabilization_strong,
            UiAction::zoom_reset,
            UiAction::zoom_fit_content,
            UiAction::zoom_fit_selection,
            UiAction::grid_color_auto,
            UiAction::custom_color_done,
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
            queue_text(
                control->label,
                control->bounds.x
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
        }

    }

    const UiControl* const hovered = toolbar.hovered_control();
    const UiControl* const focused = toolbar.focused_tooltip_control();
    const UiControl* const described =
        hovered != nullptr ? hovered : focused;
    const double described_visibility = hovered != nullptr
        ? toolbar.animation(hovered->action).hover
        : (focused != nullptr ? 1.0 : 0.0);
    if (described != nullptr && described_visibility > 0.08) {
        if (const auto bounds = tooltip_bounds(toolbar, described)) {
            const double visibility = described_visibility;
            const double offset =
                (1.0 - visibility) * 4.0 * scale + slide(bounds->y);
            const std::array<float, 4> tooltip_text =
                text_color(palette.background);
            queue_text(
                described->tooltip,
                bounds->x + 12.0 * scale,
                centered_text_top(bounds->y + offset, bounds->height),
                faded(tooltip_text, visibility));
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
    const double thumbnail_horizontal_margin =
        HomeView::card_preview_margin * scale;
    const double thumbnail_top_margin = 38.0 * scale;
    const double thumbnail_bottom_margin =
        HomeView::card_preview_margin * scale;
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
        const UiRect thumbnail{
            card.x + thumbnail_horizontal_margin,
            card.y + thumbnail_top_margin,
            card.width - thumbnail_horizontal_margin * 2.0,
            card.height - HomeView::card_text_area * scale
                - thumbnail_top_margin - thumbnail_bottom_margin,
        };
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
        std::to_string(home.boards().size()),
        heading.x + measure(heading_text, TextStyle::title) + 12.0 * scale,
        centered_text_top(heading.y, heading.height) + header_shift,
        faded(muted, reveal));

    if (!home.error_message().empty()
        && home.status_bounds().width > 1.0) {
        UiRect status = home.status_bounds();
        status.y += panel_shift;
        const double text_x = status.x + 42.0 * scale;
        const double budget = std::max(
            status.width - 54.0 * scale, 1.0);
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
                home.entrance(index)));
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
    const double text_area = HomeView::card_text_area * scale;
    for (std::size_t index = 0U; index < boards.size(); ++index) {
        const UiControl& card = home.controls()[index];
        const bool card_active =
            card.selected || home.focused_control() == &card;
        const std::array<float, 4> card_name_color =
            card_active ? on_accent : text;
        std::array<float, 4> card_date_color =
            card_active ? on_accent : muted;
        if (card_active) {
            card_date_color[3] = 0.92F;
        }
        const double entrance = home.entrance(index);
        const double offset = home.control_offset(index);
        const double card_top = card.bounds.y + offset;
        if (card.bounds.y + card.bounds.height <= home.grid_top()
            || card_top > home.viewport_height() + vertical_margin) {
            continue;
        }
        const double name_x = card.bounds.x;
        const double budget = card.bounds.width;
        const double name_y = card.bounds.y + card.bounds.height
            - text_area + 6.0 * scale + offset;
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
                    card_active
                        ? palette.on_primary
                        : palette.primary);
                queue_text(
                    before, name_x, name_y,
                    faded(card_name_color, entrance), TextStyle::bold);
                queue_text(
                    selected, name_x + before_width, name_y,
                    faded(
                        card_active
                            ? text_color(palette.primary)
                            : on_accent,
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
                    card_active
                        ? palette.on_primary
                        : palette.focus);
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
            fit(boards[index].date, budget),
            name_x,
            name_y + font_line_height + 2.0 * scale,
            faded(card_date_color, entrance));
    }
}

void GpuRenderer::tessellate_indexed_stroke(
    std::vector<CachedWorldVertex>& output,
    const Stroke& stroke,
    const Style& style,
    const std::uint32_t detail,
    const double zoom,
    const Aabb& visible,
    const StrokeSegmentIndex& index) const
{
    output.clear();
    if (stroke.points.size() < 2U) {
        tessellate_polyline(output, stroke.points, style, detail);
        return;
    }

    const std::size_t stride = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(std::ceil(
            (0.65 / std::max(zoom, 1.0e-9))
            / index.average_segment_length())));

    std::vector<std::uint32_t> segments;
    index.query(visible, segments);
    const std::size_t segment_budget = maximum_stroke_segments(
        detail, maximum_vertex_count);
    const std::size_t budget_stride = std::max<std::size_t>(
        1U,
        (segments.size() + segment_budget - 1U) / segment_budget);
    std::vector<Vec2d> run;
    std::uint32_t previous = 0U;
    std::size_t stride_candidate = 0U;
    const auto flush_run = [&]() {
        if (run.size() >= 2U) {
            tessellate_polyline(output, run, style, detail);
        }
        run.clear();
    };
    for (const auto segment : segments) {
        if (stride != 1U && segment % stride != 0U) {
            continue;
        }
        if (stride_candidate++ % budget_stride != 0U) {
            continue;
        }
        if (segment == 0U || segment >= stroke.points.size()) {
            continue;
        }
        if (!run.empty()
            && static_cast<std::size_t>(segment - previous)
                > stride * budget_stride * 2U) {
            flush_run();
        }
        if (run.empty()) {
            const std::size_t sampled_stride = stride * budget_stride;
            const std::size_t start = segment > sampled_stride
                ? static_cast<std::size_t>(segment) - sampled_stride
                : static_cast<std::size_t>(segment - 1U);
            run.push_back(stroke.points[start]);
        }
        run.push_back(stroke.points[segment]);
        previous = segment;
    }
    flush_run();
}

void GpuRenderer::prune_cache()
{
    const std::size_t total_bytes = geometry_cache_bytes_;
    if (stats_.frame % 120U != 0U
        && total_bytes <= geometry_cache_budget
        && object_cache_.size() <= maximum_cache_entries) {
        return;
    }

    std::vector<std::pair<std::uint64_t, ObjectId>> candidates;
    candidates.reserve(object_cache_.size());
    for (const auto& [id, cached] : object_cache_) {
        if (cached.last_used_frame + 600U < stats_.frame
            || total_bytes > geometry_cache_budget
            || object_cache_.size() > maximum_cache_entries) {
            candidates.emplace_back(cached.last_used_frame, id);
        }
    }
    std::ranges::sort(candidates, {}, &std::pair<std::uint64_t, ObjectId>::first);
    std::size_t current_bytes = geometry_cache_bytes_;
    std::size_t current_entries = object_cache_.size();
    for (const auto& [last_used, id] : candidates) {
        const bool stale = last_used + 600U < stats_.frame;
        if (!stale && current_bytes <= geometry_cache_budget
            && current_entries <= maximum_cache_entries) {
            break;
        }
        if (const auto cached = object_cache_.find(id);
            cached != object_cache_.end()) {
            const std::size_t bytes =
                cached->second.vertices.capacity()
                * sizeof(CachedWorldVertex);
            geometry_cache_bytes_ -= bytes;
            current_bytes -= bytes;
            object_cache_.erase(cached);
            --current_entries;
        }
        ++total_evictions_;
    }
}

void GpuRenderer::trim_board_caches_for_home()
{
    decltype(object_cache_){}.swap(object_cache_);
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
        image_vertices_.clear();
        scene_draws_.clear();

    const auto background_start = std::chrono::steady_clock::now();
    const RenderColor active_board_color =
        to_render_color(toolbar.background_color());
    const RenderColor active_border_color = mix_color(
        toolbar.previous_theme() == Theme::light
            ? RenderColor{0.58F, 0.62F, 0.69F}
            : border_color,
        toolbar.theme() == Theme::light
            ? RenderColor{0.58F, 0.62F, 0.69F}
            : border_color,
        smooth_theme_transition(toolbar));
    append_quad(
        scene_geometry_, min_x, min_y, max_x, max_y, camera_position,
        active_board_color);

    append_background_pattern(
        scene_geometry_,
        camera_position,
        toolbar.background_style(),
        active_board_color,
        toolbar.grid_color(),
        min_x,
        min_y,
        max_x,
        max_y,
        grid_world_spacing,
        camera.zoom());

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
    stats_.background_milliseconds =
        std::chrono::duration<double, std::milli>(
            background_end - background_start).count();

    const auto query_start = std::chrono::steady_clock::now();
    document.query(visible, visible_objects_, visible_object_ids_);
    const auto query_end = std::chrono::steady_clock::now();
    stats_.query_milliseconds =
        std::chrono::duration<double, std::milli>(
            query_end - query_start).count();
    stats_.visible_objects = visible_objects_.size();
    scene_visible_objects_ = visible_objects_.size();
    const auto objects_start = std::chrono::steady_clock::now();
    const auto cached_geometry_for =
        [&](const Object& object,
            const Aabb& geometry_visible) -> CachedGeometry& {
        const std::uint32_t detail =
            geometry_detail(object.geometry, object.style, camera.zoom());
        const auto* const stroke = std::get_if<Stroke>(&object.geometry);
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
        const bool region_matches = !region_limited
            || (cached.region_limited
                && cached.region_min_x == region_min_x
                && cached.region_min_y == region_min_y
                && cached.region_max_x == region_max_x
                && cached.region_max_y == region_max_y);
        if (cached.revision != object.revision || cached.detail != detail
            || !region_matches) {
            geometry_cache_bytes_ -=
                cached.vertices.capacity()
                * sizeof(CachedWorldVertex);
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
                tessellate_indexed_stroke(
                    cached.vertices,
                    *stroke,
                    object.style,
                    detail,
                    camera.zoom(),
                    geometry_visible,
                    *segment_index);
            } else {
                tessellate_geometry(
                    cached.vertices,
                    object.geometry,
                    object.style,
                    detail);
            }
            geometry_cache_bytes_ +=
                cached.vertices.capacity()
                * sizeof(CachedWorldVertex);
            cached.revision = object.revision;
            cached.detail = detail;
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
        return cached;
    };

    std::size_t vector_run_start = 0U;
    const auto flush_vector_run = [&]() {
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
            append_image_draws(*image, camera);
            vector_run_start = scene_geometry_.size();
            continue;
        }
        auto& cached = cached_geometry_for(*object, visible);
        const std::size_t cached_vertices =
            cached.vertices.size();
        if (scene_geometry_.size() + cached_vertices
            > maximum_vertex_count) {
            continue;
        }
        append_cached_geometry(
            scene_geometry_, cached.vertices, camera_position);
    }
    flush_vector_run();
    const auto objects_end = std::chrono::steady_clock::now();
    stats_.objects_milliseconds =
        std::chrono::duration<double, std::milli>(
            objects_end - objects_start).count();
        scene_signature_ = signature;
        scene_valid_ = true;
    } else {
        stats_.visible_objects = scene_visible_objects_;
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
    append_toolbar_geometry(geometry_, geometry_spans_, camera, toolbar);
    trim_geometry_to_vertex_budget(geometry_, geometry_spans_);
    const auto overlay_end = std::chrono::steady_clock::now();
    stats_.overlay_ui_milliseconds =
        std::chrono::duration<double, std::milli>(
            overlay_end - overlay_start).count();

    prune_cache();
    // Scene draws retain raw texture handles. Prune only while rebuilding the
    // draw list, after every texture referenced by the new list was touched.
    if (rebuild_scene) prune_image_cache();

    if (scene_geometry_.size() > maximum_vertex_count) {
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
    if (bold_font_ != nullptr) {
        TTF_CloseFont(bold_font_);
        bold_font_ = nullptr;
    }
    if (title_font_ != nullptr) {
        TTF_CloseFont(title_font_);
        title_font_ = nullptr;
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
