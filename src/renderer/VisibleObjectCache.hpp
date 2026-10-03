#pragma once

#include "document/Document.hpp"

#include <algorithm>
#include <limits>

namespace sawer {

class VisibleObjectCache final {
public:
    // Candidates are stable IDs in document draw order, never borrowed pointers.
    // Returning false means the spatial index was queried on this call.
    [[nodiscard]] bool query(const Document& document, const Aabb view, const double margin,
        std::vector<const Object*>& result, std::vector<ObjectId>& scratch)
    {
        const bool contained = view.min_x >= region_.min_x && view.min_y >= region_.min_y
            && view.max_x <= region_.max_x && view.max_y <= region_.max_y;
        if (valid_ && owner_ == &document && identity_ == document.cache_identity() && contained
            && revision_ != document.revision()) {
            const bool complete = document.changes_since(revision_, changes_);
            if (!complete || changes_.size() > 32U) valid_ = false;
            else {
                for (const auto& change : changes_) std::erase(candidates_, change.id);
                for (const auto& change : changes_) {
                    std::erase(candidates_, change.id);
                    const auto* object = document.find(change.id);
                    if (object == nullptr || !object->bounds.intersects(region_)) continue;
                    const auto position = std::lower_bound(candidates_.begin(), candidates_.end(), change.id,
                        [&](const ObjectId a, const ObjectId b) {
                            const auto* first = document.find(a);
                            const auto* second = document.find(b);
                            return first->z_order < second->z_order
                                || (first->z_order == second->z_order && a < b);
                        });
                    candidates_.insert(position, change.id);
                }
                revision_ = document.revision();
                if (candidates_.size() > maximum_candidates) valid_ = false;
            }
        }
        if (valid_ && owner_ == &document && identity_ == document.cache_identity()
            && revision_ == document.revision() && contained) {
            result.clear();
            for (const auto id : candidates_) {
                const auto* object = document.find(id);
                if (object != nullptr && object->bounds.intersects(view)) result.push_back(object);
            }
            return true;
        }
        region_ = {view.min_x - margin, view.min_y - margin,
            view.max_x + margin, view.max_y + margin};
        document.query(region_, result, scratch);
        valid_ = result.size() <= maximum_candidates;
        candidates_.clear();
        if (valid_) {
            candidates_.reserve(result.size());
            for (const auto* object : result) candidates_.push_back(object->id);
        }
        owner_ = &document;
        identity_ = document.cache_identity();
        revision_ = document.revision();
        std::erase_if(result, [&](const Object* object) { return !object->bounds.intersects(view); });
        return false;
    }

    void clear() noexcept
    {
        std::vector<ObjectId>{}.swap(candidates_);
        std::vector<DocumentChange>{}.swap(changes_);
        valid_ = false;
        owner_ = nullptr;
    }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept
    { return candidates_.capacity() * sizeof(ObjectId) + changes_.capacity() * sizeof(DocumentChange); }

private:
    static constexpr std::size_t maximum_candidates = 20'000U;
    const Document* owner_{};
    ObjectId identity_;
    std::vector<DocumentChange> changes_;
    std::uint64_t revision_{};
    Aabb region_{};
    std::vector<ObjectId> candidates_;
    bool valid_{};
};

} // namespace sawer
