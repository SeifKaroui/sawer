#include "input/VelocityGaussianStabilizer.hpp"

#include <algorithm>
#include <cmath>

namespace sawer {

void VelocityGaussianStabilizer::reset(
    const double sigma,
    const PointerSample initial_sample,
    const double coordinate_scale) noexcept
{
    const double safe_sigma = std::clamp(sigma, 0.05, 40.0);
    two_sigma_squared_ = 2.0 * safe_sigma * safe_sigma;
    coordinate_scale_ = std::max(coordinate_scale, 1.0e-9);
    samples_[0] = initial_sample;
    velocities_[0] = 0.0;
    count_ = 1U;
    last_timestamp_ = initial_sample.timestamp;
}

PointerSample VelocityGaussianStabilizer::push(PointerSample sample) noexcept
{
    const PointerSample previous = samples_[0];
    const std::uint64_t elapsed_ns = sample.timestamp > last_timestamp_
        ? sample.timestamp - last_timestamp_
        : 1'000'000U;
    const double elapsed_ms = std::max(
        static_cast<double>(elapsed_ns) / 1'000'000.0, 0.001);
    const double distance = std::hypot(
        sample.position.x - previous.position.x,
        sample.position.y - previous.position.y) * coordinate_scale_;
    const double velocity = distance / elapsed_ms;

    const std::size_t retained = std::min(count_, capacity - 1U);
    for (std::size_t index = retained; index > 0U; --index) {
        samples_[index] = samples_[index - 1U];
        velocities_[index] = velocities_[index - 1U];
    }
    samples_[0] = sample;
    velocities_[0] = velocity;
    count_ = retained + 1U;
    last_timestamp_ = std::max(last_timestamp_, sample.timestamp);

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_pressure = 0.0;
    double sum_weights = 0.0;
    double cumulative_velocity = 0.0;
    for (std::size_t index = 0U; index < count_; ++index) {
        const double weight = std::exp(
            -(cumulative_velocity * cumulative_velocity)
            / two_sigma_squared_);
        if (index > 0U && weight < 0.01) {
            count_ = index;
            break;
        }
        sum_x += samples_[index].position.x * weight;
        sum_y += samples_[index].position.y * weight;
        sum_pressure += static_cast<double>(samples_[index].pressure) * weight;
        sum_weights += weight;
        cumulative_velocity += velocities_[index];
    }

    sample.position = {sum_x / sum_weights, sum_y / sum_weights};
    sample.pressure = static_cast<float>(sum_pressure / sum_weights);
    return sample;
}

std::size_t VelocityGaussianStabilizer::sample_count() const noexcept
{
    return count_;
}

} // namespace sawer
