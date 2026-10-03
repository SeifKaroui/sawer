#include "image/ImageDecodeCache.hpp"

#include <algorithm>
#include <utility>

namespace sawer {
namespace {
std::size_t image_bytes(const std::shared_ptr<const DecodedImage>& image) { return image ? image->rgba.size() : 0U; }
} // namespace
ImageDecodeCache::ImageDecodeCache(const std::size_t byte_budget) : budget_{byte_budget} {}
ImageDecodeCache::ImageDecodeCache(ImageDecodeCache&& other) noexcept
    : budget_{other.budget_}
    , bytes_{std::exchange(other.bytes_, 0U)}
    , clock_{std::exchange(other.clock_, 0U)}
{
    entries_.swap(other.entries_);
}
ImageDecodeCache& ImageDecodeCache::operator=(ImageDecodeCache&& other) noexcept {
    if (this != &other) {
        clear();
        budget_ = other.budget_;
        bytes_ = std::exchange(other.bytes_, 0U);
        clock_ = std::exchange(other.clock_, 0U);
        entries_.swap(other.entries_);
    }
    return *this;
}
std::shared_ptr<const DecodedImage> ImageDecodeCache::find(const AssetId& id) {
    const auto it = entries_.find(id); if (it == entries_.end()) return {};
    it->second.used = ++clock_; return it->second.image;
}
void ImageDecodeCache::insert(AssetId id, std::shared_ptr<const DecodedImage> image) {
    const auto old = entries_.find(id); if (old != entries_.end()) { bytes_ -= image_bytes(old->second.image); entries_.erase(old); }
    const std::size_t size = image_bytes(image); if (!image || size > budget_) return;
    while (bytes_ + size > budget_ && !entries_.empty()) {
        const auto victim = std::ranges::min_element(entries_, {}, [](const auto& pair) { return pair.second.used; });
        bytes_ -= image_bytes(victim->second.image); entries_.erase(victim);
    }
    bytes_ += size; entries_.emplace(std::move(id), Entry{std::move(image), ++clock_});
}
void ImageDecodeCache::clear() noexcept { entries_.clear(); bytes_ = 0U; }
std::size_t ImageDecodeCache::bytes() const noexcept { return bytes_; }
std::size_t ImageDecodeCache::budget() const noexcept { return budget_; }
} // namespace sawer
