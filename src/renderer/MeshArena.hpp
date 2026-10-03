#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sawer {

struct MeshRange final {
    std::uint32_t page{}, first{}, count{};
    friend bool operator==(const MeshRange&, const MeshRange&) = default;
};

// GPU-independent range allocation. Ranges become reusable only when the
// renderer has retired their last GPU consumer, never at logical deletion.
class MeshArena final {
public:
    static constexpr std::uint32_t page_vertices = 65'535U;
    static constexpr std::size_t maximum_pages = 64U;

    [[nodiscard]] std::optional<std::vector<MeshRange>> allocate(const std::size_t vertices)
    {
        if (vertices == 0U || vertices % 3U != 0U
            || vertices > maximum_pages * page_vertices) return std::nullopt;
        std::size_t available = 0U;
        for (const auto& page : pages_) for (const auto range : page) available += range.count;
        const std::size_t additional = vertices > available
            ? (vertices - available + page_vertices - 1U) / page_vertices : 0U;
        if (pages_.size() + additional > maximum_pages) return std::nullopt;
        for (std::size_t page = 0U; page < additional; ++page)
            pages_.push_back({{static_cast<std::uint32_t>(pages_.size()), 0U, page_vertices}});
        std::vector<MeshRange> result;
        std::size_t remaining = vertices;
        for (auto& page : pages_) {
            for (std::size_t index = 0U; index < page.size() && remaining != 0U;) {
                auto& free = page[index];
                const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(free.count, remaining));
                result.push_back({free.page, free.first, count});
                free.first += count;
                free.count -= count;
                remaining -= count;
                if (free.count == 0U) page.erase(page.begin() + static_cast<std::ptrdiff_t>(index));
                else ++index;
            }
            if (remaining == 0U) break;
        }
        return result;
    }

    void release(const std::span<const MeshRange> ranges)
    {
        for (const auto range : ranges) {
            auto& free = pages_.at(range.page);
            auto position = std::lower_bound(free.begin(), free.end(), range.first,
                [](const MeshRange a, const std::uint32_t b) { return a.first < b; });
            position = free.insert(position, range);
            if (position != free.begin() && (position - 1)->first + (position - 1)->count == position->first) {
                (position - 1)->count += position->count;
                position = free.erase(position) - 1;
            }
            if (position + 1 != free.end() && position->first + position->count == (position + 1)->first) {
                position->count += (position + 1)->count;
                free.erase(position + 1);
            }
        }
    }
    void clear() noexcept { decltype(pages_){}.swap(pages_); }
    [[nodiscard]] std::size_t pages() const noexcept { return pages_.size(); }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept
    {
        std::size_t bytes = pages_.capacity() * sizeof(decltype(pages_)::value_type);
        for (const auto& page : pages_) bytes += page.capacity() * sizeof(MeshRange);
        return bytes;
    }

private:
    std::vector<std::vector<MeshRange>> pages_;
};

} // namespace sawer
