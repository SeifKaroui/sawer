#pragma once

#include "input/DrawingSettings.hpp"
#include "input/PointerSample.hpp"

#include <cstddef>

namespace sawer {

// A causal, rate-independent pointer filter with a hard screen-space latency
// bound. Speed raises the low-pass cutoff so deliberate fast motion remains
// responsive, while slow motion receives stronger jitter correction.
class AdaptiveStrokeFilter final {
public:
    void reset(
        AdaptiveStrokeSettings settings,
        PointerSample initial_sample,
        double coordinate_scale = 1.0) noexcept;

    [[nodiscard]] PointerSample push(PointerSample sample) noexcept;

    // Mouse-up is a document-semantic endpoint. It bypasses the low-pass step
    // so the incremental curve can end at the exact release position without
    // rerunning or replacing the preceding stroke.
    [[nodiscard]] PointerSample finish(PointerSample sample) noexcept;

    [[nodiscard]] double trailing_distance_pixels() const noexcept;
    [[nodiscard]] std::size_t sample_count() const noexcept;

private:
    [[nodiscard]] static double smoothing_alpha(
        double cutoff_hz,
        double elapsed_seconds) noexcept;

    AdaptiveStrokeSettings settings_{};
    PointerSample raw_{};
    PointerSample filtered_{};
    std::uint64_t last_timestamp_{};
    double coordinate_scale_{1.0};
    double smoothed_speed_pixels_per_second_{};
    double last_elapsed_seconds_{1.0 / 125.0};
    std::size_t sample_count_{};
    bool initialized_{};
};

} // namespace sawer
