#pragma once

#include "document/ObjectId.hpp"

#include <optional>
#include <unordered_map>
#include <utility>

namespace sawer {

// ID links remain valid across copying, moving and hash-table rehashing.
// Hits and removals do not allocate or scan the cache.
class ObjectRecency final {
public:
    ObjectRecency() = default;
    ObjectRecency(const ObjectRecency&) = default;
    ObjectRecency& operator=(const ObjectRecency&) = default;
    ObjectRecency(ObjectRecency&& other) noexcept { *this = std::move(other); }
    ObjectRecency& operator=(ObjectRecency&& other) noexcept
    {
        if (this == &other) return *this;
        links_ = std::move(other.links_);
        oldest_ = std::exchange(other.oldest_, std::nullopt);
        newest_ = std::exchange(other.newest_, std::nullopt);
        other.links_.clear();
        return *this;
    }
    void touch(const ObjectId id)
    {
        auto [entry, inserted] = links_.try_emplace(id);
        if (!inserted) unlink(id, entry->second);
        entry->second = {newest_, std::nullopt};
        if (newest_) links_.at(*newest_).next = id;
        else oldest_ = id;
        newest_ = id;
    }

    void erase(const ObjectId id) noexcept
    {
        const auto entry = links_.find(id);
        if (entry == links_.end()) return;
        unlink(id, entry->second);
        links_.erase(entry);
    }

    void clear() noexcept
    {
        decltype(links_){}.swap(links_);
        oldest_.reset();
        newest_.reset();
    }

    [[nodiscard]] std::optional<ObjectId> oldest() const noexcept { return oldest_; }
    [[nodiscard]] std::size_t size() const noexcept { return links_.size(); }
    [[nodiscard]] std::optional<ObjectId> next(const ObjectId id) const noexcept
    {
        const auto found = links_.find(id);
        return found == links_.end() ? std::nullopt : found->second.next;
    }
    [[nodiscard]] std::size_t memory_bytes() const noexcept
    {
        if (links_.empty()) return 0U;
        return links_.bucket_count() * sizeof(void*) + links_.size()
            * (sizeof(decltype(links_)::value_type) + 2U * sizeof(void*));
    }

private:
    struct Links final { std::optional<ObjectId> previous; std::optional<ObjectId> next; };
    void unlink(const ObjectId, const Links& links) noexcept
    {
        if (links.previous) links_.find(*links.previous)->second.next = links.next;
        else oldest_ = links.next;
        if (links.next) links_.find(*links.next)->second.previous = links.previous;
        else newest_ = links.previous;
    }
    std::unordered_map<ObjectId, Links, ObjectIdHash> links_;
    std::optional<ObjectId> oldest_;
    std::optional<ObjectId> newest_;
};

} // namespace sawer
