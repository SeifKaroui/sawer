#include "image/ImageCodec.hpp"
#include "image/Sha256.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace sawer {
namespace {
[[nodiscard]] std::uint16_t u16(const std::span<const std::uint8_t> b, const std::size_t o) {
    return static_cast<std::uint16_t>(b[o]) | (static_cast<std::uint16_t>(b[o + 1U]) << 8U);
}
[[nodiscard]] std::uint32_t u32(const std::span<const std::uint8_t> b, const std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1U]) << 8U)
        | (static_cast<std::uint32_t>(b[o + 2U]) << 16U) | (static_cast<std::uint32_t>(b[o + 3U]) << 24U);
}
[[noreturn]] void image_error(const char* const message) { throw std::runtime_error{"Image decode error: " + std::string{message}}; }

[[nodiscard]] bool is_png(const std::span<const std::uint8_t> bytes) noexcept
{
    constexpr std::array<std::uint8_t, 8U> signature{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    return bytes.size() >= signature.size()
        && std::equal(signature.begin(), signature.end(), bytes.begin());
}

[[nodiscard]] DecodedImage make_preview(
    const DecodedImage& source,
    const std::uint32_t maximum_dimension)
{
    if (maximum_dimension == 0U) image_error("invalid preview limit");
    const std::uint32_t largest = std::max(source.width, source.height);
    if (largest <= maximum_dimension) return source;
    const double scale = static_cast<double>(maximum_dimension)
        / static_cast<double>(largest);
    const auto width = static_cast<std::uint32_t>(std::max(
        1.0, std::floor(static_cast<double>(source.width) * scale)));
    const auto height = static_cast<std::uint32_t>(std::max(
        1.0, std::floor(static_cast<double>(source.height) * scale)));
    DecodedImage preview{width, height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U)};
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::uint32_t source_y = std::min(
            source.height - 1U,
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * source.height / height));
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::uint32_t source_x = std::min(
                source.width - 1U,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * source.width / width));
            const std::size_t from = (static_cast<std::size_t>(source_y) * source.width + source_x) * 4U;
            const std::size_t to = (static_cast<std::size_t>(y) * width + x) * 4U;
            std::copy_n(source.rgba.data() + from, 4U, preview.rgba.data() + to);
        }
    }
    return preview;
}
} // namespace

DecodedImage decode_bmp_rgba(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 54U || bytes[0] != 'B' || bytes[1] != 'M' || u32(bytes, 14U) < 40U) image_error("unsupported BMP header");
    const std::size_t data_offset = u32(bytes, 10U);
    const std::int32_t signed_width = static_cast<std::int32_t>(u32(bytes, 18U));
    const std::int32_t signed_height = static_cast<std::int32_t>(u32(bytes, 22U));
    const std::uint16_t bits = u16(bytes, 28U);
    if (signed_width <= 0 || signed_height == 0 || (bits != 24U && bits != 32U) || u32(bytes, 30U) != 0U) image_error("unsupported BMP pixels");
    const std::uint32_t width = static_cast<std::uint32_t>(signed_width);
    const std::uint32_t height = static_cast<std::uint32_t>(signed_height < 0 ? -static_cast<std::int64_t>(signed_height) : signed_height);
    if (width > 16'384U || height > 16'384U || static_cast<std::uint64_t>(width) * height > 64'000'000U) image_error("BMP dimensions exceed limit");
    const std::size_t source_stride = ((static_cast<std::size_t>(width) * bits + 31U) / 32U) * 4U;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (data_offset > bytes.size() || source_stride > (bytes.size() - data_offset) / height) image_error("truncated BMP pixels");
    DecodedImage image{width, height, std::vector<std::uint8_t>(pixels * 4U)};
    const bool top_down = signed_height < 0;
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::uint32_t source_y = top_down ? y : height - 1U - y;
        const auto* source = bytes.data() + data_offset + static_cast<std::size_t>(source_y) * source_stride;
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t src = static_cast<std::size_t>(x) * (bits / 8U);
            const std::size_t dst = (static_cast<std::size_t>(y) * width + x) * 4U;
            image.rgba[dst] = source[src + 2U]; image.rgba[dst + 1U] = source[src + 1U]; image.rgba[dst + 2U] = source[src]; image.rgba[dst + 3U] = bits == 32U ? source[src + 3U] : 255U;
        }
    }
    return image;
}

DecodedImage decode_image_rgba(const std::span<const std::uint8_t> bytes)
{
    constexpr std::size_t maximum_input_bytes = 64U * 1024U * 1024U;
    if (bytes.empty() || bytes.size() > maximum_input_bytes
        || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        image_error("encoded image exceeds limit");
    }
    if (bytes[0] == 'B' && bytes.size() >= 2U && bytes[1] == 'M') {
        return decode_bmp_rgba(bytes);
    }
    if (!is_png(bytes)) image_error("only PNG and BMP images are supported");
    int width{};
    int height{};
    int channels{};
    if (stbi_info_from_memory(
            bytes.data(), static_cast<int>(bytes.size()),
            &width, &height, &channels) == 0
        || width <= 0 || height <= 0) {
        image_error("unsupported or malformed image");
    }
    const std::uint32_t image_width = static_cast<std::uint32_t>(width);
    const std::uint32_t image_height = static_cast<std::uint32_t>(height);
    if (image_width > 16'384U || image_height > 16'384U
        || static_cast<std::uint64_t>(image_width) * image_height > 64'000'000U) {
        image_error("image dimensions exceed limit");
    }
    stbi_uc* const pixels = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        image_error("unsupported or malformed image");
    }
    if (static_cast<std::uint32_t>(width) != image_width
        || static_cast<std::uint32_t>(height) != image_height) {
        stbi_image_free(pixels);
        image_error("image dimensions changed during decode");
    }
    const std::size_t count = static_cast<std::size_t>(image_width) * image_height * 4U;
    DecodedImage image{image_width, image_height, {pixels, pixels + count}};
    stbi_image_free(pixels);
    return image;
}

namespace {
void append_png(void* const context, void* const data, const int size)
{
    if (size <= 0) return;
    auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
    const auto* const first = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), first, first + size);
}
} // namespace

std::vector<std::uint8_t> encode_png_rgba(const DecodedImage& image)
{
    if (image.width == 0U || image.height == 0U
        || image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U
        || image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        image_error("invalid RGBA image");
    }
    std::vector<std::uint8_t> png;
    if (stbi_write_png_to_func(
            append_png, &png, static_cast<int>(image.width),
            static_cast<int>(image.height), 4, image.rgba.data(),
            static_cast<int>(image.width * 4U)) == 0) {
        image_error("cannot encode PNG");
    }
    return png;
}

CanonicalImage canonicalize_image(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t maximum_preview_dimension)
{
    auto decoded = std::make_shared<DecodedImage>(decode_image_rgba(bytes));
    auto asset = std::make_shared<ImageAsset>();
    asset->pixel_width = decoded->width;
    asset->pixel_height = decoded->height;
    asset->png = encode_png_rgba(*decoded);
    asset->id = sha256(asset->png);
    asset->preview = encode_png_rgba(make_preview(*decoded, maximum_preview_dimension));
    return {std::move(asset), std::move(decoded)};
}
} // namespace sawer
