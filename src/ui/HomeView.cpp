#include "ui/HomeView.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <charconv>
#include <ctime>
#include <limits>
#include <utility>

namespace sawer {

std::string HomeView::format_date(
    const std::string_view timestamp, const std::chrono::year_month_day today)
{
    if (timestamp.size() != 16U || timestamp[4] != '-' || timestamp[7] != '-'
        || timestamp[10] != ' ' || timestamp[13] != ':') {
        return std::string{timestamp};
    }
    const auto number = [&](const std::size_t start, const std::size_t count) {
        int value = -1;
        const char* first = timestamp.data() + start;
        const auto parsed = std::from_chars(first, first + count, value);
        return parsed.ec == std::errc{} && parsed.ptr == first + count ? value : -1;
    };
    const int year = number(0U, 4U);
    const int month = number(5U, 2U);
    const int day = number(8U, 2U);
    const int hour = number(11U, 2U);
    const int minute = number(14U, 2U);
    const std::chrono::year_month_day date{
        std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (year < 1 || month < 1 || month > 12 || day < 1 || !date.ok()
        || !today.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return std::string{timestamp};
    }
    const auto age = std::chrono::sys_days{today} - std::chrono::sys_days{date};
    if (age == std::chrono::days{0}) return "Today " + std::string{timestamp.substr(11U)};
    if (age == std::chrono::days{1}) return "Yesterday " + std::string{timestamp.substr(11U)};
    constexpr std::array months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::to_string(day) + " " + months[static_cast<std::size_t>(month - 1)]
        + " " + std::to_string(year);
}

void HomeView::update(
    const double viewport_width,
    const double viewport_height,
    const double display_scale,
    const Theme theme,
    std::vector<HomeBoard> boards,
    std::string error_message,
    std::string status_message)
{
    const bool source_changed = boards_.size() != boards.size()
        || !std::equal(
            boards_.begin(), boards_.end(), boards.begin(), boards.end(),
            [](const HomeBoard& current, const HomeBoard& replacement) {
                return current.path == replacement.path;
            });
    if (source_changed) {
        top_row_ = 0U;
        animations_.clear();
        rename_hovers_.clear();
        focused_control_.reset();
    }
    const std::time_t now = std::time(nullptr);
    std::tm calendar{};
#ifdef _WIN32
    const bool have_today = localtime_s(&calendar, &now) == 0;
#else
    const bool have_today = localtime_r(&now, &calendar) != nullptr;
#endif
    const std::chrono::year_month_day today{
        std::chrono::year{calendar.tm_year + 1900},
        std::chrono::month{static_cast<unsigned>(calendar.tm_mon + 1)},
        std::chrono::day{static_cast<unsigned>(calendar.tm_mday)}};
    for (auto& board : boards) {
        board.display_date = have_today ? format_date(board.date, today) : board.date;
    }
    boards_ = std::move(boards);
    error_message_ = std::move(error_message);
    status_message_ = std::move(status_message);
    relayout(viewport_width, viewport_height, display_scale, theme);
}

void HomeView::relayout(
    const double viewport_width,
    const double viewport_height,
    const double display_scale,
    const Theme theme)
{
    viewport_width_ = std::max(viewport_width, 1.0);
    viewport_height_ = std::max(viewport_height, 1.0);
    const double available_scale = std::max(
        0.82,
        std::min(viewport_width_ / 760.0, viewport_height_ / 480.0));
    scale_ = std::min(
        std::clamp(display_scale, 0.82, 1.5),
        available_scale);
    if (theme_ != theme) {
        previous_theme_ = theme_;
        theme_ = theme;
        theme_transition_ = 0.0;
    }

    controls_.clear();
    panels_.clear();
    rename_buttons_.clear();
    header_bounds_ = {};
    heading_bounds_ = {};
    status_bounds_ = {};
    grid_top_ = 0.0;
    maximum_top_row_ = 0U;
    row_count_ = 0U;
    visible_rows_ = 1U;
    row_step_ = 0.0;

    const double outer = 40.0 * scale_;
    const double content_width = std::min(
        std::max(viewport_width_ - outer * 2.0, 1.0),
        1200.0 * scale_);
    content_bounds_ = {
        (viewport_width_ - content_width) * 0.5,
        28.0 * scale_,
        content_width,
        std::max(viewport_height_ - 56.0 * scale_, 1.0),
    };

    const auto finish_layout = [&]() {
        if (animations_.size() != controls_.size()) {
            animations_.assign(controls_.size(), UiAnimation{});
        }
        if (rename_hovers_.size() != rename_buttons_.size()) {
            rename_hovers_.assign(rename_buttons_.size(), 0.0);
        }
        pressed_control_.reset();
        pressed_rename_.reset();
    };

    controls_.reserve(boards_.size() + 3U);

    const double column_gap = 20.0 * scale_;
    const double row_gap = 20.0 * scale_;
    const bool compact_header = content_bounds_.width < 680.0 * scale_;
    const double header_height = (compact_header ? 112.0 : 56.0) * scale_;
    header_bounds_ = {
        content_bounds_.x,
        content_bounds_.y,
        content_bounds_.width,
        header_height,
    };
    heading_bounds_ = {
        content_bounds_.x,
        header_bounds_.y + (compact_header ? 2.0 : 6.0) * scale_,
        content_bounds_.width,
        44.0 * scale_,
    };
    if (!error_message_.empty() || !status_message_.empty()) {
        status_bounds_ = {
            heading_bounds_.x,
            header_bounds_.y + header_bounds_.height + 20.0 * scale_,
            std::min(heading_bounds_.width, 680.0 * scale_),
            54.0 * scale_,
        };
        grid_top_ =
            status_bounds_.y + status_bounds_.height + 20.0 * scale_;
    } else {
        grid_top_ =
            header_bounds_.y + header_bounds_.height + 28.0 * scale_;
    }

    // Board cards first, in board order, so board_at() and the renderer can
    // match a control to its board by index.
    const double usable_width = content_bounds_.width;
    const double minimum_card = 236.0 * scale_;
    columns_ = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(
            std::floor(
                (usable_width + column_gap)
                / (minimum_card + column_gap))));
    const double card_w = std::min(
        card_width * scale_,
        (usable_width
            - static_cast<double>(columns_ - 1U) * column_gap)
            / static_cast<double>(columns_));
    // Keep previews generous while reserving a readable metadata footer.
    const double card_h = std::clamp(
        card_w * 0.90 + 6.0 * scale_,
        218.0 * scale_,
        card_height * scale_);
    const double grid_width = static_cast<double>(columns_) * card_w
        + static_cast<double>(columns_ - 1U) * column_gap;
    const double grid_left = content_bounds_.x
        + std::max(0.0, (usable_width - grid_width) * 0.5);
    rename_buttons_.reserve(boards_.size());
    row_step_ = card_h + row_gap;
    row_count_ = boards_.empty()
        ? 0U
        : (boards_.size() + columns_ - 1U) / columns_;
    const double gallery_height = std::max(
        viewport_height_ - grid_top_ - 20.0 * scale_, card_h);
    visible_rows_ = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(
            std::floor((gallery_height + row_gap) / row_step_)));
    maximum_top_row_ = row_count_ > visible_rows_
        ? row_count_ - visible_rows_
        : 0U;
    top_row_ = std::min(top_row_, maximum_top_row_);

    const double chip = std::max(32.0 * scale_, 32.0);
    for (std::size_t index = 0U; index < boards_.size(); ++index) {
        const std::size_t row = index / columns_;
        const std::size_t column = index % columns_;
        const UiRect bounds{
            grid_left
                + static_cast<double>(column) * (card_w + column_gap),
            grid_top_
                + (static_cast<double>(row) - static_cast<double>(top_row_))
                    * row_step_,
            card_w,
            card_h,
        };
        controls_.push_back(UiControl{
            .action = UiAction::home_open_board,
            .bounds = bounds,
            .label = boards_[index].name,
            .tooltip = "Open board",
            .icon = UiIcon::none,
            .enabled = true,
            .selected = false,
            .accent = std::nullopt,
        });
        // Keep contextual actions in the footer, clear of the drawing.
        rename_buttons_.push_back(UiRect{
            bounds.x + bounds.width - chip - 10.0 * scale_,
            bounds.y + bounds.height - card_text_area * scale_
                + (card_text_area * scale_ - chip) * 0.5,
            chip,
            chip,
        });
    }

    // Fixed file actions live in the header, stored after the cards.
    const double button_height = 40.0 * scale_;
    const double action_gap = 8.0 * scale_;
    const double theme_width = button_height;
    const double preferred_open_width = 140.0 * scale_;
    const double preferred_new_width = 136.0 * scale_;
    const double width_scale = std::min(
        1.0,
        std::max(
            content_bounds_.width - theme_width - action_gap * 2.0,
            2.0)
            / (preferred_open_width + preferred_new_width));
    const double open_width = preferred_open_width * width_scale;
    const double new_width = preferred_new_width * width_scale;
    const double action_width =
        open_width + new_width + theme_width + action_gap * 2.0;
    const double action_x = compact_header
        ? header_bounds_.x
        : header_bounds_.x + header_bounds_.width - action_width;
    const double button_y = compact_header
        ? header_bounds_.y + header_bounds_.height - button_height
        : header_bounds_.y + (header_bounds_.height - button_height) * 0.5;
    controls_.push_back(UiControl{
        .action = UiAction::open_board,
        .bounds = {action_x, button_y, open_width, button_height},
        .label = "Open board",
        .tooltip = "Open a .sawer file",
        .icon = UiIcon::folder_open,
        .enabled = true,
        .selected = false,
        .accent = std::nullopt,
    });
    controls_.push_back(UiControl{
        .action = UiAction::home_new_board,
        .bounds = {
            action_x + open_width + action_gap,
            button_y,
            new_width,
            button_height,
        },
        .label = "New board",
        .tooltip = "Create a new board",
        .icon = UiIcon::file_new,
        .enabled = true,
        .selected = true,
        .accent = std::nullopt,
    });
    controls_.push_back(UiControl{
        .action = UiAction::toggle_theme,
        .bounds = {
            action_x + open_width + new_width + action_gap * 2.0,
            button_y,
            theme_width,
            button_height,
        },
        .label = {},
        .tooltip = "Light or dark theme  T",
        .icon = UiIcon::theme,
        .enabled = true,
        .selected = false,
        .accent = std::nullopt,
    });

    if (boards_.empty()) {
        const double empty_width = std::min(
            560.0 * scale_, content_bounds_.width);
        panels_.push_back({{
            content_bounds_.x
                + (content_bounds_.width - empty_width) * 0.5,
            grid_top_ + 16.0 * scale_,
            empty_width,
            164.0 * scale_,
        }});
    }
    finish_layout();
    if (focused_control_.has_value()
        && *focused_control_ >= controls_.size()) {
        focused_control_.reset();
    }
    ensure_focused_visible();
    hovered_date_.reset();
    date_hover_seconds_ = 0.0;
    if (pointer_) set_pointer(*pointer_);
}

void HomeView::set_pointer(const Vec2d point) noexcept
{
    pointer_ = point;
    const auto board = board_at(point);
    const auto date = board && !boards_[*board].editing
        && !boards_[*board].date.empty()
        && board_date_bounds(*board).contains(point) ? board : std::nullopt;
    if (date != hovered_date_) {
        hovered_date_ = date;
        date_hover_seconds_ = 0.0;
    }
}

void HomeView::clear_pointer() noexcept
{
    pointer_.reset();
    hovered_date_.reset();
    date_hover_seconds_ = 0.0;
    pressed_control_.reset();
    pressed_rename_.reset();
}

void HomeView::pointer_down(const Vec2d point) noexcept
{
    set_pointer(point);
    date_hover_seconds_ = 0.0;
    pressed_control_.reset();
    pressed_rename_ = rename_at(point);
    if (pressed_rename_.has_value()) {
        focused_control_ = pressed_rename_;
        return;
    }
    focused_control_.reset();
    for (std::size_t index = 0U; index < controls_.size(); ++index) {
        const UiRect bounds = visual_control_bounds(index);
        const bool hidden_board = index < boards_.size()
            && controls_[index].bounds.y
                + controls_[index].bounds.height <= grid_top_;
        if (!hidden_board && controls_[index].enabled
            && bounds.contains(point)) {
            pressed_control_ = index;
            focused_control_ = index;
            break;
        }
    }
}

bool HomeView::pointer_up(const Vec2d point) noexcept
{
    bool activated = false;
    if (pressed_rename_.has_value()) {
        activated = rename_at(point) == pressed_rename_;
    } else if (pressed_control_.has_value()) {
        const std::size_t index = *pressed_control_;
        activated = index < controls_.size()
            && controls_[index].enabled
            && visual_control_bounds(index).contains(point);
        if (activated && index < boards_.size()) {
            activated = !rename_at(point).has_value()
                && point.y >= grid_top_;
        }
    }
    pressed_control_.reset();
    pressed_rename_.reset();
    return activated;
}

void HomeView::scroll_rows(const int rows) noexcept
{
    if (rows == 0 || maximum_top_row_ == 0U || row_step_ <= 0.0) {
        return;
    }
    const auto requested = static_cast<long long>(top_row_)
        + static_cast<long long>(rows);
    const auto next = static_cast<std::size_t>(std::clamp<long long>(
        requested, 0LL, static_cast<long long>(maximum_top_row_)));
    if (next == top_row_) {
        return;
    }

    const double shift = (static_cast<double>(top_row_)
        - static_cast<double>(next)) * row_step_;
    for (std::size_t index = 0U; index < boards_.size(); ++index) {
        controls_[index].bounds.y += shift;
        rename_buttons_[index].y += shift;
    }
    top_row_ = next;
    hovered_date_.reset();
    date_hover_seconds_ = 0.0;
    if (pointer_) set_pointer(*pointer_);
    pressed_control_.reset();
    pressed_rename_.reset();
}

void HomeView::focus_next(const bool reverse) noexcept
{
    if (controls_.empty()) {
        focused_control_.reset();
        return;
    }
    if (!focused_control_.has_value()
        || *focused_control_ >= controls_.size()) {
        focused_control_ = reverse ? controls_.size() - 1U : 0U;
    } else if (reverse) {
        focused_control_ = *focused_control_ == 0U
            ? controls_.size() - 1U
            : *focused_control_ - 1U;
    } else {
        focused_control_ = (*focused_control_ + 1U) % controls_.size();
    }
    ensure_focused_visible();
}

void HomeView::clear_focus() noexcept
{
    focused_control_.reset();
}

void HomeView::play_entrance() noexcept
{
    open_seconds_ = 100.0;
}

double HomeView::entrance(const std::size_t index) const noexcept
{
    static_cast<void>(index);
    return 1.0;
}

double HomeView::reveal() const noexcept
{
    return 1.0;
}

double HomeView::control_offset(const std::size_t index) const noexcept
{
    static_cast<void>(index);
    return 0.0;
}

void HomeView::tick(const double elapsed_seconds) noexcept
{
    const double elapsed = std::clamp(elapsed_seconds, 0.0, 0.1);
    open_seconds_ += elapsed;
    if (hovered_date_ && !pressed_control_ && !pressed_rename_) {
        date_hover_seconds_ = std::min(0.45, date_hover_seconds_ + elapsed);
    }
    theme_transition_ = std::min(
        1.0,
        theme_transition_ + elapsed / 0.24);
    const auto approach = [elapsed](double& value, const double target,
                                    const double speed) {
        const double blend = 1.0 - std::exp(-speed * elapsed);
        value += (target - value) * blend;
        if (std::abs(value - target) < 0.001) value = target;
    };

    for (std::size_t index = 0U; index < controls_.size(); ++index) {
        const UiRect bounds = visual_control_bounds(index);
        const bool hidden_board = index < boards_.size()
            && controls_[index].bounds.y
                + controls_[index].bounds.height <= grid_top_;
        const bool hovered = !hidden_board && pointer_.has_value()
            && controls_[index].enabled
            && bounds.contains(*pointer_);
        approach(animations_[index].hover, hovered ? 1.0 : 0.0, 15.0);
        approach(
            animations_[index].press,
            hovered && pressed_control_ == index ? 1.0 : 0.0,
            pressed_control_ == index ? 24.0 : 18.0);
        approach(
            animations_[index].selected,
            controls_[index].selected ? 1.0 : 0.0,
            14.0);
    }
    for (std::size_t index = 0U; index < rename_buttons_.size(); ++index) {
        const bool hovered = pointer_.has_value()
            && pointer_->y >= grid_top_
            && rename_buttons_[index].y
                + rename_buttons_[index].height > grid_top_
            && visual_rename_bounds(index).contains(*pointer_);
        // Match the card-hover response so the contextual control's surface,
        // glyph, and hover color do not appear to run on separate clocks.
        approach(rename_hovers_[index], hovered ? 1.0 : 0.0, 15.0);
    }
}

std::optional<UiAction> HomeView::action_at(const Vec2d point) const noexcept
{
    for (std::size_t index = 0U; index < controls_.size(); ++index) {
        const UiRect bounds = visual_control_bounds(index);
        if (index < boards_.size()
            && (point.y < grid_top_
                || controls_[index].bounds.y
                    + controls_[index].bounds.height <= grid_top_)) {
            continue;
        }
        if (controls_[index].enabled && bounds.contains(point)) {
            return controls_[index].action;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> HomeView::board_at(const Vec2d point) const noexcept
{
    if (point.y < grid_top_) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < boards_.size(); ++index) {
        if (controls_[index].bounds.y + controls_[index].bounds.height
                > grid_top_
            && visual_control_bounds(index).contains(point)) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> HomeView::rename_at(const Vec2d point) const noexcept
{
    if (point.y < grid_top_) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < rename_buttons_.size(); ++index) {
        if (rename_buttons_[index].y + rename_buttons_[index].height
                > grid_top_
            && visual_rename_bounds(index).contains(point)) {
            return index;
        }
    }
    return std::nullopt;
}

const std::vector<UiRect>& HomeView::rename_buttons() const noexcept
{
    return rename_buttons_;
}

const std::vector<UiControl>& HomeView::controls() const noexcept
{
    return controls_;
}

const std::vector<UiPanel>& HomeView::panels() const noexcept
{
    return panels_;
}

const std::vector<HomeBoard>& HomeView::boards() const noexcept
{
    return boards_;
}

const UiControl* HomeView::hovered_control() const noexcept
{
    if (!pointer_.has_value()) {
        return nullptr;
    }
    for (std::size_t index = 0U; index < controls_.size(); ++index) {
        const UiRect bounds = visual_control_bounds(index);
        if (index < boards_.size()
            && (pointer_->y < grid_top_
                || controls_[index].bounds.y
                    + controls_[index].bounds.height <= grid_top_)) {
            continue;
        }
        if (controls_[index].enabled && bounds.contains(*pointer_)) {
            return &controls_[index];
        }
    }
    return nullptr;
}

const UiAnimation& HomeView::animation(const std::size_t index) const noexcept
{
    static constexpr UiAnimation empty{};
    return index < animations_.size() ? animations_[index] : empty;
}

double HomeView::rename_hover(const std::size_t index) const noexcept
{
    return index < rename_hovers_.size() ? rename_hovers_[index] : 0.0;
}

double HomeView::scale() const noexcept
{
    return scale_;
}

Theme HomeView::theme() const noexcept
{
    return theme_;
}

const UiControl* HomeView::focused_control() const noexcept
{
    if (!focused_control_.has_value()
        || *focused_control_ >= controls_.size()) {
        return nullptr;
    }
    return &controls_[*focused_control_];
}

std::optional<std::size_t> HomeView::focused_index() const noexcept
{
    return focused_control_;
}

bool HomeView::animating() const noexcept
{
    if ((hovered_date_ && date_hover_seconds_ < 0.45)
        || reveal() < 1.0 || theme_transition_ < 1.0
        || pressed_control_.has_value() || pressed_rename_.has_value()) {
        return true;
    }
    for (std::size_t index = 0U; index < controls_.size(); ++index) {
        if (entrance(index) < 1.0) {
            return true;
        }
        const UiRect bounds = visual_control_bounds(index);
        const bool hidden_board = index < boards_.size()
            && controls_[index].bounds.y
                + controls_[index].bounds.height <= grid_top_;
        const bool hovered = !hidden_board && pointer_.has_value()
            && controls_[index].enabled && bounds.contains(*pointer_);
        const UiAnimation& state = animations_[index];
        if (state.hover != (hovered ? 1.0 : 0.0)
            || state.press != 0.0
            || state.selected
                != (controls_[index].selected ? 1.0 : 0.0)) {
            return true;
        }
    }
    for (std::size_t index = 0U; index < rename_hovers_.size(); ++index) {
        const bool hovered = pointer_.has_value()
            && pointer_->y >= grid_top_
            && rename_buttons_[index].y
                + rename_buttons_[index].height > grid_top_
            && visual_rename_bounds(index).contains(*pointer_);
        if (rename_hovers_[index] != (hovered ? 1.0 : 0.0)) {
            return true;
        }
    }
    return false;
}

Theme HomeView::previous_theme() const noexcept
{
    return previous_theme_;
}

double HomeView::theme_transition() const noexcept
{
    return theme_transition_;
}

double HomeView::viewport_width() const noexcept
{
    return viewport_width_;
}

double HomeView::viewport_height() const noexcept
{
    return viewport_height_;
}

UiRect HomeView::content_bounds() const noexcept
{
    return content_bounds_;
}

UiRect HomeView::header_bounds() const noexcept
{
    return header_bounds_;
}

UiRect HomeView::heading_bounds() const noexcept
{
    return heading_bounds_;
}

UiRect HomeView::status_bounds() const noexcept
{
    return status_bounds_;
}

UiRect HomeView::board_name_bounds(const std::size_t index) const noexcept
{
    if (index >= boards_.size()) return {};
    const UiRect card = visual_control_bounds(index);
    const double x = card.x + 16.0 * scale_;
    return {
        x,
        card.y + card.height - card_text_area * scale_ + 12.0 * scale_,
        std::max(visual_rename_bounds(index).x - 10.0 * scale_ - x, 1.0),
        24.0 * scale_,
    };
}

UiRect HomeView::board_date_bounds(const std::size_t index) const noexcept
{
    UiRect date = board_name_bounds(index);
    if (date.width <= 0.0) return {};
    date.y += 28.0 * scale_;
    date.height = 18.0 * scale_;
    return date;
}

UiRect HomeView::board_preview_bounds(const std::size_t index) const noexcept
{
    if (index >= boards_.size()) return {};
    const UiRect card = visual_control_bounds(index);
    const double margin = card_preview_margin * scale_;
    const double width = std::max(card.width - margin * 2.0, 1.0);
    const double height = std::max(card.height - card_text_area * scale_ - margin * 2.0, 1.0);
    const double fit = std::min(width / BoardPreview::pixel_width,
        height / BoardPreview::pixel_height);
    const double preview_width = BoardPreview::pixel_width * fit;
    const double preview_height = BoardPreview::pixel_height * fit;
    return {card.x + (card.width - preview_width) * 0.5,
        card.y + (card.height - card_text_area * scale_ - preview_height) * 0.5,
        preview_width, preview_height};
}

std::optional<std::size_t> HomeView::date_tooltip() const noexcept
{
    return date_hover_seconds_ >= 0.45 && !pressed_control_ && !pressed_rename_
        ? hovered_date_ : std::nullopt;
}

UiRect HomeView::scrollbar_track() const noexcept
{
    if (maximum_top_row_ == 0U) {
        return {};
    }
    const double width = 4.0 * scale_;
    const double x = std::min(
        viewport_width_ - width - 6.0 * scale_,
        content_bounds_.x + content_bounds_.width + 14.0 * scale_);
    return {
        x,
        grid_top_,
        width,
        std::max(viewport_height_ - grid_top_ - 18.0 * scale_, 1.0),
    };
}

UiRect HomeView::scrollbar_thumb() const noexcept
{
    const UiRect track = scrollbar_track();
    if (track.height <= 1.0 || row_count_ == 0U) {
        return {};
    }
    const double minimum_height = std::min(34.0 * scale_, track.height);
    const double height = std::clamp(
        track.height * static_cast<double>(visible_rows_)
            / static_cast<double>(row_count_),
        minimum_height, track.height);
    const double travel = track.height - height;
    const double progress = maximum_top_row_ == 0U
        ? 0.0
        : static_cast<double>(top_row_)
            / static_cast<double>(maximum_top_row_);
    return {track.x, track.y + travel * progress, track.width, height};
}

std::string_view HomeView::error_message() const noexcept
{
    return error_message_;
}

std::string_view HomeView::status_message() const noexcept
{
    return status_message_;
}

UiRect HomeView::visual_control_bounds(const std::size_t index) const noexcept
{
    if (index >= controls_.size()) {
        return {};
    }
    UiRect bounds = controls_[index].bounds;
    bounds.y += control_offset(index);
    return bounds;
}

UiRect HomeView::visual_rename_bounds(const std::size_t index) const noexcept
{
    if (index >= rename_buttons_.size()) {
        return {};
    }
    UiRect bounds = rename_buttons_[index];
    bounds.y += control_offset(index);
    return bounds;
}

double HomeView::grid_top() const noexcept
{
    return grid_top_;
}

std::size_t HomeView::top_row() const noexcept
{
    return top_row_;
}

std::size_t HomeView::maximum_top_row() const noexcept
{
    return maximum_top_row_;
}

void HomeView::ensure_focused_visible() noexcept
{
    if (!focused_control_.has_value()
        || *focused_control_ >= boards_.size()
        || columns_ == 0U
        || maximum_top_row_ == 0U) {
        return;
    }
    const std::size_t row = *focused_control_ / columns_;
    std::size_t wanted = top_row_;
    if (row < top_row_) {
        wanted = row;
    } else if (row >= top_row_ + visible_rows_) {
        wanted = row - visible_rows_ + 1U;
    }
    wanted = std::min(wanted, maximum_top_row_);
    if (wanted == top_row_) {
        return;
    }
    const long long difference =
        static_cast<long long>(wanted)
        - static_cast<long long>(top_row_);
    scroll_rows(static_cast<int>(std::clamp<long long>(
        difference,
        static_cast<long long>(std::numeric_limits<int>::min()),
        static_cast<long long>(std::numeric_limits<int>::max()))));
}

} // namespace sawer
