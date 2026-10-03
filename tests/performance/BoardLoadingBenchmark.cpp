#include "core/Crc32c.hpp"
#include "document/Object.hpp"
#include "image/ImageDecodeCache.hpp"
#include "image/Sha256.hpp"
#include "storage/BoardFile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(const Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t peak_process_bytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters,
            static_cast<DWORD>(sizeof(counters)))) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#elif defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0)
        return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
    return 0U;
}

std::uint32_t original_crc32c(const std::span<const std::uint8_t> bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0x82F63B78U : 0U);
    }
    return ~crc;
}

template<class Function>
double measure_crc(Function function, const std::vector<std::uint8_t>& input)
{
    double best = 1e9;
    for (unsigned repeat = 0U; repeat < 3U; ++repeat) {
        const auto start = Clock::now();
        const auto crc = function(input);
        best = std::min(best, elapsed_ms(start));
        // Observable checks prevent removal of the timed computation.
        if (crc != sawer::crc32c_extend_portable(0U, input))
            throw std::runtime_error{"checksum implementations disagree"};
    }
    return best;
}

void require_insert(sawer::Document& document, sawer::Object object)
{
    if (!document.insert(std::move(object))) throw std::runtime_error{"invalid fixture"};
}

void measure_board(const char* name, const sawer::Document& original)
{
    const auto path = std::filesystem::temp_directory_path()
        / ("sawer-loading-benchmark-" + sawer::ObjectId::random().to_string() + ".sawer");
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{path};
    static_cast<void>(sawer::BoardFileSession::create(path, original));
    for (const bool reuse : {false, true}) {
        std::vector<double> open_times;
        std::vector<double> preparation_times;
        std::size_t misses = 0U;
        std::size_t retained = 0U;
        for (unsigned repeat = 0U; repeat < 3U; ++repeat) {
            sawer::Document loaded;
            sawer::ImageDecodeCache cache;
            bool recovered = false;
            auto start = Clock::now();
            static_cast<void>(sawer::BoardFileSession::open(path, loaded, recovered,
                reuse ? &cache : nullptr));
            open_times.push_back(elapsed_ms(start));
            if (recovered || loaded.size() != original.size())
                throw std::runtime_error{"fixture did not round trip"};
            const auto objects = loaded.all_objects();
            misses = 0U;
            start = Clock::now();
            for (const auto* object : objects) {
                const auto* image = std::get_if<sawer::Image>(&object->geometry);
                if (image == nullptr) continue;
                auto decoded = cache.find(image->asset->id);
                if (!decoded) {
                    decoded = std::make_shared<const sawer::DecodedImage>(
                        sawer::decode_image_rgba(image->asset->png));
                    cache.insert(image->asset->id, decoded);
                    ++misses;
                }
                if (decoded->width != image->asset->pixel_width)
                    throw std::runtime_error{"decoded pixels changed"};
            }
            preparation_times.push_back(elapsed_ms(start));
            retained = cache.bytes();
            if (retained > cache.budget()) throw std::runtime_error{"cache exceeded budget"};
        }
        std::sort(open_times.begin(), open_times.end());
        std::sort(preparation_times.begin(), preparation_times.end());
        std::printf("board=%s reuse=%d file_bytes=%llu objects=%zu median_open_ms=%.3f "
                    "median_image_preparation_ms=%.3f render_decode_misses=%zu retained_decoded_bytes=%zu\n",
            name, reuse ? 1 : 0, static_cast<unsigned long long>(std::filesystem::file_size(path)),
            original.size(), open_times[1], preparation_times[1], misses, retained);
    }
}

} // namespace

int main()
{
    try {
        std::mt19937 random{1234U};
        std::vector<std::uint8_t> bytes(16U * 1024U * 1024U);
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
        const double original_ms = measure_crc(original_crc32c, bytes);
        const double portable_ms = measure_crc([](const auto& input) {
            return sawer::crc32c_extend_portable(0U, input);
        }, bytes);
        const double selected_ms = measure_crc([](const auto& input) {
            return sawer::crc32c_extend(0U, input);
        }, bytes);
        std::printf("crc bytes=%zu original_ms=%.3f portable_ms=%.3f selected_ms=%.3f\n",
            bytes.size(), original_ms, portable_ms, selected_ms);

        sawer::Document vectors;
        for (std::uint64_t index = 0U; index < 100'000U; ++index) {
            const auto x = static_cast<double>(index % 1000U) * 100.0;
            const auto y = static_cast<double>(index / 1000U) * 100.0;
            require_insert(vectors, sawer::Object::make_line(sawer::ObjectId::from_u64(index + 1U),
                static_cast<std::int64_t>(index), {{x, y}, {x + 20.0, y + 10.0}}, {}));
        }
        measure_board("100k-vectors", vectors);
        vectors.clear();
        sawer::Document strokes;
        for (std::uint64_t index = 0U; index < 100U; ++index) {
            sawer::Stroke stroke;
            stroke.points.reserve(10'000U);
            for (unsigned point = 0U; point < 10'000U; ++point)
                stroke.points.push_back({static_cast<double>(point), static_cast<double>(index) * 20.0});
            require_insert(strokes, sawer::Object::make_stroke(sawer::ObjectId::from_u64(index + 1U),
                static_cast<std::int64_t>(index), std::move(stroke), {}));
        }
        measure_board("1m-stroke-points", strokes);
        strokes.clear();
        sawer::Document images;
        for (std::uint64_t index = 0U; index < 4U; ++index) {
            sawer::DecodedImage pixels{1024U, 1024U, std::vector<std::uint8_t>(4U * 1024U * 1024U)};
            for (auto& byte : pixels.rgba) byte = static_cast<std::uint8_t>(random());
            auto asset = std::make_shared<sawer::ImageAsset>();
            asset->pixel_width = pixels.width;
            asset->pixel_height = pixels.height;
            asset->png = sawer::encode_png_rgba(pixels);
            asset->id = sawer::sha256(asset->png);
            for (std::uint64_t placement = 0U; placement < 8U; ++placement) {
                const auto id = index * 8U + placement + 1U;
                require_insert(images, sawer::Object::make_image(sawer::ObjectId::from_u64(id),
                    static_cast<std::int64_t>(id), {asset, {0.0, 0.0}, {100.0, 100.0}}));
            }
        }
        measure_board("4-images-32-placements", images);
        // Process-wide peak includes fixture generation and validation's
        // temporary decode/encode buffers, not just the retained pixel cache.
        std::printf("process_peak_resident_bytes=%llu\n",
            static_cast<unsigned long long>(peak_process_bytes()));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "board-loading benchmark: %s\n", error.what());
        return 1;
    }
    return 0;
}
