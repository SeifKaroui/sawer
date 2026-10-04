#include "ui/BoardPreview.hpp"

#include "document/Document.hpp"
#include "document/Object.hpp"
#include "image/ImageCodec.hpp"
#include "renderer/BoardTheme.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sawer {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double raster_margin = 6.0;
// A 0.5-pixel radius produces a coverage-weighted one-pixel hairline. The
// previous 0.65 minimum made distant thin strokes look visibly heavier.
constexpr double minimum_stroke_radius = 0.5;
constexpr std::uint8_t fill_coverage = 255U;
constexpr std::uint8_t stroke_coverage = 255U;

class CoverageRaster final {
public:
    CoverageRaster()
        : pixels_(
            static_cast<std::size_t>(BoardPreview::pixel_width)
                * BoardPreview::pixel_height,
            0U)
    {
        touched_.reserve(pixels_.size());
    }

    void disk(
        const Vec2d center,
        const double radius,
        const std::uint8_t coverage)
    {
        const double clamped_radius =
            std::clamp(radius, minimum_stroke_radius, 5.0);
        const int min_x = std::max(
            0, static_cast<int>(std::floor(
                   center.x - clamped_radius - 0.5)));
        const int min_y = std::max(
            0, static_cast<int>(std::floor(
                   center.y - clamped_radius - 0.5)));
        const int max_x = std::min(
            static_cast<int>(BoardPreview::pixel_width) - 1,
            static_cast<int>(std::ceil(
                center.x + clamped_radius + 0.5)));
        const int max_y = std::min(
            static_cast<int>(BoardPreview::pixel_height) - 1,
            static_cast<int>(std::ceil(
                center.y + clamped_radius + 0.5)));
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const double dx = static_cast<double>(x) + 0.5 - center.x;
                const double dy = static_cast<double>(y) + 0.5 - center.y;
                const double edge =
                    clamped_radius + 0.5 - std::hypot(dx, dy);
                if (edge <= 0.0) {
                    continue;
                }
                const auto alpha = static_cast<std::uint8_t>(std::lround(
                    static_cast<double>(coverage)
                    * std::min(edge, 1.0)));
                blend(x, y, alpha);
            }
        }
    }

    void segment(
        const Vec2d first,
        const Vec2d second,
        const double radius,
        const std::uint8_t coverage = stroke_coverage)
    {
        const double dx = second.x - first.x;
        const double dy = second.y - first.y;
        const auto steps = static_cast<std::size_t>(std::max(
            1.0, std::ceil(std::max(std::abs(dx), std::abs(dy)))));
        for (std::size_t step = 0U; step <= steps; ++step) {
            const double amount =
                static_cast<double>(step) / static_cast<double>(steps);
            disk(
                {first.x + dx * amount, first.y + dy * amount},
                radius,
                coverage);
        }
    }

    void filled_rectangle(
        const Vec2d first,
        const Vec2d second,
        const std::uint8_t coverage)
    {
        const int min_x = std::clamp(
            static_cast<int>(std::floor(std::min(first.x, second.x))),
            0,
            static_cast<int>(BoardPreview::pixel_width) - 1);
        const int min_y = std::clamp(
            static_cast<int>(std::floor(std::min(first.y, second.y))),
            0,
            static_cast<int>(BoardPreview::pixel_height) - 1);
        const int max_x = std::clamp(
            static_cast<int>(std::ceil(std::max(first.x, second.x))),
            0,
            static_cast<int>(BoardPreview::pixel_width) - 1);
        const int max_y = std::clamp(
            static_cast<int>(std::ceil(std::max(first.y, second.y))),
            0,
            static_cast<int>(BoardPreview::pixel_height) - 1);
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                blend(x, y, coverage);
            }
        }
    }

    void filled_ellipse(
        const Vec2d first,
        const Vec2d second,
        const std::uint8_t coverage)
    {
        const double center_x = (first.x + second.x) * 0.5;
        const double center_y = (first.y + second.y) * 0.5;
        const double radius_x = std::max(std::abs(second.x - first.x) * 0.5, 0.5);
        const double radius_y = std::max(std::abs(second.y - first.y) * 0.5, 0.5);
        const int min_x = std::max(
            0, static_cast<int>(std::floor(center_x - radius_x)));
        const int min_y = std::max(
            0, static_cast<int>(std::floor(center_y - radius_y)));
        const int max_x = std::min(
            static_cast<int>(BoardPreview::pixel_width) - 1,
            static_cast<int>(std::ceil(center_x + radius_x)));
        const int max_y = std::min(
            static_cast<int>(BoardPreview::pixel_height) - 1,
            static_cast<int>(std::ceil(center_y + radius_y)));
        for (int y = min_y; y <= max_y; ++y) {
            const double normalized_y =
                (static_cast<double>(y) + 0.5 - center_y) / radius_y;
            const double remaining =
                1.0 - normalized_y * normalized_y;
            if (remaining < 0.0) {
                continue;
            }
            const double extent = radius_x * std::sqrt(remaining);
            const int row_min_x = std::max(
                min_x,
                static_cast<int>(std::floor(center_x - extent)));
            const int row_max_x = std::min(
                max_x,
                static_cast<int>(std::ceil(center_x + extent)));
            for (int x = row_min_x; x <= row_max_x; ++x) {
                blend(x, y, coverage);
            }
        }
    }

    void composite(
        const Color color,
        std::vector<std::uint8_t>& rgba,
        const bool clear = true) noexcept
    {
        for (const std::size_t pixel : touched_) {
            const std::uint32_t source_alpha =
                (static_cast<std::uint32_t>(color.alpha) * pixels_[pixel]
                    + 127U)
                / 255U;
            const std::size_t offset = pixel * 4U;
            const std::uint32_t destination_alpha = rgba[offset + 3U];
            const std::uint32_t inverse_source = 255U - source_alpha;
            const std::uint32_t output_alpha = source_alpha
                + (destination_alpha * inverse_source + 127U) / 255U;
            if (output_alpha > 0U) {
                const auto blend_channel =
                    [&](const std::uint8_t source,
                        const std::uint8_t destination) {
                        const std::uint32_t numerator =
                            static_cast<std::uint32_t>(source) * source_alpha
                            + (static_cast<std::uint32_t>(destination)
                                   * destination_alpha * inverse_source
                                + 127U)
                                / 255U;
                        return static_cast<std::uint8_t>(
                            (numerator + output_alpha / 2U) / output_alpha);
                    };
                rgba[offset] = blend_channel(color.red, rgba[offset]);
                rgba[offset + 1U] =
                    blend_channel(color.green, rgba[offset + 1U]);
                rgba[offset + 2U] =
                    blend_channel(color.blue, rgba[offset + 2U]);
                rgba[offset + 3U] =
                    static_cast<std::uint8_t>(output_alpha);
            }
            if (clear) pixels_[pixel] = 0U;
        }
        if (clear) touched_.clear();
    }

private:
    void blend(
        const int x,
        const int y,
        const std::uint8_t coverage) noexcept
    {
        // Rounded zero coverage must not enqueue a pixel repeatedly; both
        // palette composites consume the same deduplicated coverage list.
        if (coverage == 0U) return;
        const std::size_t index =
            static_cast<std::size_t>(y) * BoardPreview::pixel_width
            + static_cast<std::size_t>(x);
        auto& pixel = pixels_[index];
        if (pixel == 0U) {
            touched_.push_back(index);
        }
        pixel = std::max(pixel, coverage);
    }

    std::vector<std::uint8_t> pixels_;
    std::vector<std::size_t> touched_;
};

[[nodiscard]] bool composite_image_proxy(
    const Image& image,
    const Vec2d first,
    const Vec2d second,
    BoardPreview& preview)
{
    if (!image.asset || image.asset->preview.empty()) return false;
    DecodedImage proxy;
    try {
        proxy = decode_image_rgba(image.asset->preview);
    } catch (const std::exception&) {
        return false;
    }
    const double width = second.x - first.x;
    const double height = second.y - first.y;
    if (std::abs(width) < 1.0e-9 || std::abs(height) < 1.0e-9) return false;
    const int min_x = std::clamp(static_cast<int>(std::floor(std::min(first.x, second.x))),
        0, static_cast<int>(BoardPreview::pixel_width) - 1);
    const int max_x = std::clamp(static_cast<int>(std::ceil(std::max(first.x, second.x))),
        0, static_cast<int>(BoardPreview::pixel_width) - 1);
    const int min_y = std::clamp(static_cast<int>(std::floor(std::min(first.y, second.y))),
        0, static_cast<int>(BoardPreview::pixel_height) - 1);
    const int max_y = std::clamp(static_cast<int>(std::ceil(std::max(first.y, second.y))),
        0, static_cast<int>(BoardPreview::pixel_height) - 1);
    for (int y = min_y; y <= max_y; ++y) {
        const double v = (static_cast<double>(y) + 0.5 - first.y) / height;
        if (v < 0.0 || v >= 1.0) continue;
        const auto source_y = std::min(proxy.height - 1U,
            static_cast<std::uint32_t>(v * proxy.height));
        for (int x = min_x; x <= max_x; ++x) {
            const double u = (static_cast<double>(x) + 0.5 - first.x) / width;
            if (u < 0.0 || u >= 1.0) continue;
            const auto source_x = std::min(proxy.width - 1U,
                static_cast<std::uint32_t>(u * proxy.width));
            const std::size_t source_offset =
                (static_cast<std::size_t>(source_y) * proxy.width + source_x) * 4U;
            const std::size_t target_offset =
                (static_cast<std::size_t>(y) * BoardPreview::pixel_width
                    + static_cast<std::size_t>(x)) * 4U;
            const std::uint32_t source_alpha = proxy.rgba[source_offset + 3U];
            if (source_alpha == 0U) continue;
            for (auto* const pixels : {&preview.rgba, &preview.dark_rgba}) {
                auto& destination = *pixels;
                const std::uint32_t destination_alpha = destination[target_offset + 3U];
                const std::uint32_t inverse = 255U - source_alpha;
                const std::uint32_t output_alpha = source_alpha
                    + (destination_alpha * inverse + 127U) / 255U;
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    const std::uint32_t numerator =
                        static_cast<std::uint32_t>(proxy.rgba[source_offset + channel]) * source_alpha
                        + (static_cast<std::uint32_t>(destination[target_offset + channel])
                            * destination_alpha * inverse + 127U) / 255U;
                    destination[target_offset + channel] = static_cast<std::uint8_t>(
                        (numerator + output_alpha / 2U) / output_alpha);
                }
                destination[target_offset + 3U] = static_cast<std::uint8_t>(output_alpha);
            }
        }
    }
    return true;
}

} // namespace

BoardPreview rasterize_board_preview(const Document& document)
{
    BoardPreview preview;
    const auto objects = document.all_objects();
    if (objects.empty()) {
        return preview;
    }

    Aabb bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest(),
    };
    for (const Object* const object : objects) {
        bounds.min_x = std::min(bounds.min_x, object->bounds.min_x);
        bounds.min_y = std::min(bounds.min_y, object->bounds.min_y);
        bounds.max_x = std::max(bounds.max_x, object->bounds.max_x);
        bounds.max_y = std::max(bounds.max_y, object->bounds.max_y);
    }
    if (bounds.max_x <= bounds.min_x) {
        bounds.min_x -= 1.0;
        bounds.max_x += 1.0;
    }
    if (bounds.max_y <= bounds.min_y) {
        bounds.min_y -= 1.0;
        bounds.max_y += 1.0;
    }

    const double board_width = bounds.max_x - bounds.min_x;
    const double board_height = bounds.max_y - bounds.min_y;
    const double drawable_width =
        static_cast<double>(BoardPreview::pixel_width) - raster_margin * 2.0;
    const double drawable_height =
        static_cast<double>(BoardPreview::pixel_height) - raster_margin * 2.0;
    const double fit =
        std::min(drawable_width / board_width, drawable_height / board_height);
    const double origin_x =
        raster_margin + (drawable_width - board_width * fit) * 0.5;
    const double origin_y =
        raster_margin + (drawable_height - board_height * fit) * 0.5;
    const auto map = [&](const Vec2d point) {
        return Vec2d{
            origin_x + (point.x - bounds.min_x) * fit,
            origin_y + (point.y - bounds.min_y) * fit,
        };
    };
    const auto stroke_radius = [&](const Style& style) {
        return std::clamp(
            style.stroke_width * fit * 0.5,
            minimum_stroke_radius,
            5.0);
    };

    CoverageRaster raster;
    preview.rgba.resize(
        static_cast<std::size_t>(BoardPreview::pixel_width)
            * BoardPreview::pixel_height * 4U,
        0U);
    preview.dark_rgba.resize(preview.rgba.size(), 0U);
    const auto composite = [&](const Color color) {
        raster.composite(color, preview.rgba, false);
        raster.composite(board_display_color(color, 1.0), preview.dark_rgba);
    };
    constexpr std::size_t ellipse_segments = 36U;
    for (const Object* const object : objects) {
        if (const auto* const line = std::get_if<Line>(&object->geometry)) {
            raster.segment(
                map(line->start),
                map(line->end),
                stroke_radius(object->style));
            composite(object->style.stroke);
        } else if (const auto* const stroke =
                       std::get_if<Stroke>(&object->geometry)) {
            if (stroke->points.empty()) {
                continue;
            }
            if (stroke->points.size() == 1U) {
                raster.disk(
                    map(stroke->points.front()),
                    stroke_radius(object->style),
                    stroke_coverage);
                composite(object->style.stroke);
                continue;
            }
            Vec2d previous = map(stroke->points.front());
            for (std::size_t point = 1U; point < stroke->points.size(); ++point) {
                const Vec2d next = map(stroke->points[point]);
                const bool final_point = point + 1U == stroke->points.size();
                if (!final_point
                    && std::abs(next.x - previous.x) < 0.125
                    && std::abs(next.y - previous.y) < 0.125) {
                    continue;
                }
                raster.segment(
                    previous, next, stroke_radius(object->style));
                previous = next;
            }
            composite(object->style.stroke);
        } else if (const auto* const rectangle =
                       std::get_if<RectangleShape>(&object->geometry)) {
            const Vec2d first = map(rectangle->first);
            const Vec2d second = map(rectangle->second);
            if (object->style.fill.has_value()) {
                raster.filled_rectangle(first, second, fill_coverage);
                composite(*object->style.fill);
            }
            const double radius = stroke_radius(object->style);
            const Vec2d top_left{
                std::min(first.x, second.x), std::min(first.y, second.y)};
            const Vec2d bottom_right{
                std::max(first.x, second.x), std::max(first.y, second.y)};
            const Vec2d top_right{bottom_right.x, top_left.y};
            const Vec2d bottom_left{top_left.x, bottom_right.y};
            raster.segment(top_left, top_right, radius);
            raster.segment(top_right, bottom_right, radius);
            raster.segment(bottom_right, bottom_left, radius);
            raster.segment(bottom_left, top_left, radius);
            composite(object->style.stroke);
        } else if (const auto* const ellipse =
                       std::get_if<Ellipse>(&object->geometry)) {
            const Vec2d first = map(ellipse->first);
            const Vec2d second = map(ellipse->second);
            if (object->style.fill.has_value()) {
                raster.filled_ellipse(first, second, fill_coverage);
                composite(*object->style.fill);
            }
            const Vec2d center{
                (first.x + second.x) * 0.5,
                (first.y + second.y) * 0.5,
            };
            const double radius_x = std::abs(second.x - first.x) * 0.5;
            const double radius_y = std::abs(second.y - first.y) * 0.5;
            Vec2d previous{center.x + radius_x, center.y};
            for (std::size_t segment = 1U;
                 segment <= ellipse_segments;
                 ++segment) {
                const double angle =
                    2.0 * pi * static_cast<double>(segment)
                    / static_cast<double>(ellipse_segments);
                const Vec2d next{
                    center.x + std::cos(angle) * radius_x,
                    center.y + std::sin(angle) * radius_y,
                };
                raster.segment(
                    previous, next, stroke_radius(object->style));
                previous = next;
            }
            composite(object->style.stroke);
        } else if (const auto* const image =
                       std::get_if<Image>(&object->geometry)) {
            const Vec2d first = map(image->first);
            const Vec2d second = map(image->second);
            const bool composited =
                composite_image_proxy(*image, first, second, preview);
            // Missing or corrupt proxies are intentionally non-fatal. Keep
            // their placement visible without touching the raw full image.
            if (!composited) {
                raster.filled_rectangle(first, second, fill_coverage);
                composite({104U, 122U, 158U, 120U});
            }
        }
    }

    preview.has_content = true;
    return preview;
}

void write_board_preview_pixels(const BoardPreview& preview,
    const std::uint8_t dark_amount, const std::span<std::uint8_t> destination)
{
    if (destination.size() != preview.rgba.size()
        || preview.dark_rgba.size() != preview.rgba.size()) {
        throw std::invalid_argument{"Invalid themed preview pixel payload"};
    }
    for (std::size_t index = 0; index < destination.size(); ++index) {
        destination[index] = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(preview.rgba[index]) * (255U - dark_amount)
                + static_cast<std::uint32_t>(preview.dark_rgba[index]) * dark_amount
                + 127U) / 255U);
    }
}

} // namespace sawer
