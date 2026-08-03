#include "input/PointerResampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sawer {
namespace {

PointerSample interpolate_sample(
    const PointerSample start,
    const PointerSample end,
    const double amount) noexcept
{
    PointerSample result = end;
    result.position = {
        start.position.x + (end.position.x - start.position.x) * amount,
        start.position.y + (end.position.y - start.position.y) * amount,
    };
    result.pressure = static_cast<float>(
        static_cast<double>(start.pressure)
        + (static_cast<double>(end.pressure)
           - static_cast<double>(start.pressure)) * amount);
    if (end.timestamp > start.timestamp) {
        const std::uint64_t elapsed = end.timestamp - start.timestamp;
        result.timestamp = start.timestamp + static_cast<std::uint64_t>(
            std::round(static_cast<double>(elapsed) * amount));
    }
    return result;
}

} // namespace

void PointerResampler::reset(const PointerSample initial_sample) noexcept
{
    previous_ = initial_sample;
    distance_to_next_pixels_ = spacing_pixels;
    initialized_ = true;
    output_count_ = 0U;
}

std::span<const PointerSample> PointerResampler::push(
    const PointerSample sample,
    const double coordinate_scale,
    const bool include_endpoint) noexcept
{
    output_count_ = 0U;
    if (!initialized_) {
        reset(sample);
        return {};
    }

    const double scale = std::max(coordinate_scale, 1.0e-9);
    const double delta_x = sample.position.x - previous_.position.x;
    const double delta_y = sample.position.y - previous_.position.y;
    const double segment_pixels = std::hypot(delta_x, delta_y) * scale;

    if (segment_pixels > 1.0e-12) {
        // A pointer warp or a long application stall must not manufacture an
        // unbounded number of hot-path samples. Uniformly span the exceptional
        // segment, including its endpoint, and resume normal spacing after it.
        const double requested = std::ceil(
            std::max(0.0, segment_pixels - distance_to_next_pixels_)
                / spacing_pixels)
            + 1.0;
        if (requested > static_cast<double>(capacity)) {
            for (std::size_t index = 0U; index < capacity; ++index) {
                output_[index] = interpolate_sample(
                    previous_,
                    sample,
                    static_cast<double>(index + 1U)
                        / static_cast<double>(capacity));
            }
            output_count_ = capacity;
            distance_to_next_pixels_ = spacing_pixels;
        } else {
            double distance = distance_to_next_pixels_;
            while (distance <= segment_pixels + 1.0e-9
                   && output_count_ < capacity) {
                output_[output_count_++] = interpolate_sample(
                    previous_, sample, distance / segment_pixels);
                distance += spacing_pixels;
            }
            distance_to_next_pixels_ =
                std::max(1.0e-9, distance - segment_pixels);
        }
    }

    if (include_endpoint
        && (output_count_ == 0U
            || output_[output_count_ - 1U].position != sample.position)) {
        if (output_count_ < capacity) {
            output_[output_count_++] = sample;
        } else {
            output_.back() = sample;
        }
        distance_to_next_pixels_ = spacing_pixels;
    }

    previous_ = sample;
    return {output_.data(), output_count_};
}

} // namespace sawer
