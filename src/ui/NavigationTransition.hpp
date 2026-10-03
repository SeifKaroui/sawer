#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace sawer {

// A short reveal of the destination over its neutral backdrop. Nothing moves,
// so the rendered controls and their hit regions remain aligned throughout.
class NavigationTransition final {
public:
    static constexpr double duration_seconds = 0.18;

    // The initial screen appears normally. Only an actual Home/board switch
    // starts a transition; loading completion and UI refreshes do not restart it.
    [[nodiscard]] bool update_view(const bool home) noexcept
    {
        if (home_ == home) return false;
        const bool changed = home_.has_value();
        home_ = home;
        if (changed) elapsed_ = 0.0;
        return changed;
    }

    void tick(const double elapsed_seconds) noexcept
    {
        if (std::isfinite(elapsed_seconds) && elapsed_seconds > 0.0) {
            elapsed_ = std::min(duration_seconds, elapsed_ + elapsed_seconds);
        }
    }

    [[nodiscard]] double opacity() const noexcept
    {
        const double progress = elapsed_ / duration_seconds;
        return 1.0 - progress * progress * (3.0 - 2.0 * progress);
    }

    [[nodiscard]] bool animating() const noexcept
    {
        return elapsed_ < duration_seconds;
    }

private:
    std::optional<bool> home_;
    double elapsed_{duration_seconds};
};

} // namespace sawer
