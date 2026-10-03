#include "input/AdaptiveStrokeFilter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sawer {

namespace {

constexpr double minimum_elapsed_seconds = 1.0 / 4'000.0;
constexpr double maximum_elapsed_seconds = 1.0 / 15.0;

} // namespace

void AdaptiveStrokeFilter::reset(
    const AdaptiveStrokeSettings settings,
    const PointerSample initial_sample,
    const double coordinate_scale) noexcept
{
    settings_ = {
        .strength = std::clamp(settings.strength, 0.0, 1.0),
        .max_trailing_pixels =
            std::clamp(settings.max_trailing_pixels, 0.0, 64.0),
    };
    raw_ = initial_sample;
    filtered_ = initial_sample;
    last_timestamp_ = initial_sample.timestamp;
    coordinate_scale_ = std::max(coordinate_scale, 1.0e-9);
    smoothed_speed_pixels_per_second_ = 0.0;
    last_elapsed_seconds_ = 1.0 / 125.0;
    sample_count_ = 1U;
    initialized_ = true;
}

double AdaptiveStrokeFilter::smoothing_alpha(
    const double cutoff_hz,
    const double elapsed_seconds) noexcept
{
    const double time_constant =
        1.0 / (2.0 * std::numbers::pi * std::max(cutoff_hz, 1.0e-6));
    return elapsed_seconds / (time_constant + elapsed_seconds);
}

PointerSample AdaptiveStrokeFilter::push(PointerSample sample) noexcept
{
    if (!initialized_) {
        reset({}, sample);
        return sample;
    }

    double elapsed_seconds = last_elapsed_seconds_;
    if (sample.timestamp > last_timestamp_) {
        elapsed_seconds = std::clamp(
            static_cast<double>(sample.timestamp - last_timestamp_)
                / 1'000'000'000.0,
            minimum_elapsed_seconds,
            maximum_elapsed_seconds);
    }
    last_elapsed_seconds_ = elapsed_seconds;

    const double raw_distance_pixels = std::hypot(
        sample.position.x - raw_.position.x,
        sample.position.y - raw_.position.y) * coordinate_scale_;
    const double raw_speed = raw_distance_pixels / elapsed_seconds;
    const double velocity_alpha = smoothing_alpha(12.0, elapsed_seconds);
    smoothed_speed_pixels_per_second_ += velocity_alpha
        * (raw_speed - smoothed_speed_pixels_per_second_);

    // Stronger profiles use a lower resting cutoff and adapt less aggressively
    // to speed. The responsive profile reaches roughly 28 Hz at 1000 px/s.
    const double minimum_cutoff =
        11.0 + (3.0 - 11.0) * settings_.strength;
    const double speed_coefficient =
        0.024 + (0.007 - 0.024) * settings_.strength;
    const double cutoff = minimum_cutoff
        + speed_coefficient * smoothed_speed_pixels_per_second_;
    const double position_alpha = smoothing_alpha(cutoff, elapsed_seconds);

    filtered_.position.x += position_alpha
        * (sample.position.x - filtered_.position.x);
    filtered_.position.y += position_alpha
        * (sample.position.y - filtered_.position.y);
    filtered_.pressure = static_cast<float>(
        static_cast<double>(filtered_.pressure) + position_alpha
            * (static_cast<double>(sample.pressure)
               - static_cast<double>(filtered_.pressure)));

    const double trailing_x = sample.position.x - filtered_.position.x;
    const double trailing_y = sample.position.y - filtered_.position.y;
    const double trailing_pixels =
        std::hypot(trailing_x, trailing_y) * coordinate_scale_;
    if (trailing_pixels > settings_.max_trailing_pixels
        && trailing_pixels > 1.0e-12) {
        const double retained = settings_.max_trailing_pixels
            / trailing_pixels;
        filtered_.position = {
            sample.position.x - trailing_x * retained,
            sample.position.y - trailing_y * retained,
        };
    }

    filtered_.source = sample.source;
    filtered_.timestamp = sample.timestamp;
    filtered_.buttons = sample.buttons;
    raw_ = sample;
    last_timestamp_ = std::max(last_timestamp_, sample.timestamp);
    ++sample_count_;
    return filtered_;
}

PointerSample AdaptiveStrokeFilter::finish(const PointerSample sample) noexcept
{
    if (!initialized_) {
        reset({}, sample);
        return sample;
    }
    raw_ = sample;
    filtered_ = sample;
    last_timestamp_ = std::max(last_timestamp_, sample.timestamp);
    ++sample_count_;
    return filtered_;
}

double AdaptiveStrokeFilter::trailing_distance_pixels() const noexcept
{
    if (!initialized_) {
        return 0.0;
    }
    return std::hypot(
        raw_.position.x - filtered_.position.x,
        raw_.position.y - filtered_.position.y) * coordinate_scale_;
}

std::size_t AdaptiveStrokeFilter::sample_count() const noexcept
{
    return sample_count_;
}

} // namespace sawer
