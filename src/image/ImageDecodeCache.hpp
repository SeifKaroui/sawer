#pragma once

#include "document/Object.hpp"
#include "image/ImageCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>

namespace sawer {

class ImageDecodeCache final {
public:
    explicit ImageDecodeCache(std::size_t byte_budget = 128U * 1024U * 1024U);
    ImageDecodeCache(ImageDecodeCache&& other) noexcept;
    ImageDecodeCache& operator=(ImageDecodeCache&& other) noexcept;
    ImageDecodeCache(const ImageDecodeCache&) = delete;
    ImageDecodeCache& operator=(const ImageDecodeCache&) = delete;
    [[nodiscard]] std::shared_ptr<const DecodedImage> find(const AssetId& id);
    void insert(AssetId id, std::shared_ptr<const DecodedImage> image);
    void clear() noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] std::size_t budget() const noexcept;

private:
    struct Entry final { std::shared_ptr<const DecodedImage> image; std::uint64_t used{}; };
    std::size_t budget_;
    std::size_t bytes_{};
    std::uint64_t clock_{};
    std::map<AssetId, Entry> entries_;
};

} // namespace sawer
