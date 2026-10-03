#include "storage/SawerRecord.hpp"

#include "core/Crc32c.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace sawer {
namespace {

constexpr std::array<std::uint8_t, sawer_file_prologue_size> file_prologue{
    0x53U, 0x41U, 0x57U, 0x45U, 0x52U, 0x00U, 0x01U, 0x00U};
constexpr std::array<std::uint8_t, 4U> record_magic{0x53U, 0x57U, 0x52U, 0x46U};
constexpr std::uint8_t record_version = 1U;

[[noreturn]] void record_error(const std::string& message)
{
    throw std::runtime_error{"Sawer record error: " + message};
}

void write_u32_le(std::array<std::uint8_t, sawer_record_header_size>& bytes,
    const std::size_t offset, const std::uint32_t value)
{
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void write_u64_le(std::array<std::uint8_t, sawer_record_header_size>& bytes,
    const std::size_t offset, const std::uint64_t value)
{
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

[[nodiscard]] std::uint32_t read_u32_le(
    const std::array<std::uint8_t, sawer_record_header_size>& bytes,
    const std::size_t offset)
{
    std::uint32_t value{};
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64_le(
    const std::array<std::uint8_t, sawer_record_header_size>& bytes,
    const std::size_t offset)
{
    std::uint64_t value{};
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t crc32c(
    const std::array<std::uint8_t, sawer_record_header_size>& header,
    const std::vector<std::uint8_t>& metadata,
    const std::vector<std::uint8_t>& payload)
{
    std::uint32_t crc = crc32c_extend(0U,
        std::span<const std::uint8_t>{header}.subspan(4U, 24U));
    crc = crc32c_extend(crc, metadata);
    return crc32c_extend(crc, payload);
}

[[nodiscard]] bool read_exact(std::istream& input, std::uint8_t* const bytes,
    const std::size_t size)
{
    input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount()) == size;
}

[[nodiscard]] bool read_cbor_argument(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& offset,
    const std::uint8_t additional,
    std::uint64_t& value) noexcept
{
    if (additional < 24U) {
        value = additional;
        return true;
    }
    const std::size_t width = additional == 24U ? 1U
        : (additional == 25U ? 2U : (additional == 26U ? 4U
        : (additional == 27U ? 8U : 0U)));
    if (width == 0U || offset > bytes.size() || width > bytes.size() - offset) {
        return false;
    }
    value = 0U;
    for (std::size_t index = 0U; index < width; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    offset += width;
    return true;
}

[[nodiscard]] bool validate_cbor_value(
    const std::vector<std::uint8_t>& bytes,
    std::size_t& offset,
    const std::size_t depth) noexcept
{
    // Keep malformed metadata from recursively consuming unbounded parser
    // stack. This still exceeds the nesting required by Sawer's schema.
    if (depth > 128U || offset >= bytes.size()) return false;
    const std::uint8_t initial = bytes[offset++];
    const std::uint8_t major = initial >> 5U;
    const std::uint8_t additional = initial & 0x1fU;
    if (major == 6U || additional == 31U) return false;
    std::uint64_t argument{};
    if (!read_cbor_argument(bytes, offset, additional, argument)) return false;
    if (major <= 1U) return true;
    if (major == 2U || major == 3U) {
        return argument <= bytes.size() - offset
            && (offset += static_cast<std::size_t>(argument), true);
    }
    if (major == 4U || major == 5U) {
        // Avoid overflow and keep all claimed children within the physical
        // metadata buffer (each value has at least one initial byte).
        if (major == 5U
            && argument > std::numeric_limits<std::uint64_t>::max() / 2U) {
            return false;
        }
        const std::uint64_t values = major == 5U ? argument * 2U : argument;
        if (values > bytes.size() - offset) return false;
        for (std::uint64_t index = 0U; index < values; ++index) {
            if (!validate_cbor_value(bytes, offset, depth + 1U)) return false;
        }
        return true;
    }
    // Sawer's subset permits false, true, null, and finite-width IEEE 754
    // floats. nlohmann/json 3.12 emits the shortest exact float width for
    // deterministic CBOR, so float16/32 are valid alongside float64.
    return additional == 20U || additional == 21U || additional == 22U
        || additional == 25U || additional == 26U || additional == 27U;
}

void validate_cbor_subset(const std::vector<std::uint8_t>& bytes)
{
    std::size_t offset{};
    if (bytes.empty() || !validate_cbor_value(bytes, offset, 0U)
        || offset != bytes.size()) {
        record_error("metadata is outside the deterministic CBOR subset");
    }
}

} // namespace

void write_sawer_file_prologue(std::ostream& output)
{
    output.write(reinterpret_cast<const char*>(file_prologue.data()),
        static_cast<std::streamsize>(file_prologue.size()));
    if (!output) {
        record_error("cannot write file prologue");
    }
}

void validate_sawer_file_prologue(std::istream& input)
{
    std::array<std::uint8_t, sawer_file_prologue_size> bytes{};
    if (!read_exact(input, bytes.data(), bytes.size())) {
        record_error("missing or truncated file prologue");
    }
    if (bytes != file_prologue) {
        record_error("invalid file prologue");
    }
}

void write_sawer_record(std::ostream& output, const SawerRecord& record)
{
    const std::vector<std::uint8_t> metadata =
        nlohmann::json::to_cbor(record.metadata);
    if (metadata.size() > std::numeric_limits<std::uint32_t>::max()) {
        record_error("metadata is too large");
    }
    std::array<std::uint8_t, sawer_record_header_size> header{};
    std::copy(record_magic.begin(), record_magic.end(), header.begin());
    header[4] = record_version;
    header[5] = static_cast<std::uint8_t>(record.kind);
    write_u64_le(header, 8U, record.sequence);
    write_u32_le(header, 16U, static_cast<std::uint32_t>(metadata.size()));
    write_u64_le(header, 20U, static_cast<std::uint64_t>(record.payload.size()));
    write_u32_le(header, 28U, crc32c(header, metadata, record.payload));
    output.write(reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(metadata.data()),
        static_cast<std::streamsize>(metadata.size()));
    output.write(reinterpret_cast<const char*>(record.payload.data()),
        static_cast<std::streamsize>(record.payload.size()));
    if (!output) {
        record_error("cannot write record");
    }
}

SawerRecordReadResult read_sawer_record(
    std::istream& input,
    SawerRecord& record,
    const std::size_t maximum_metadata_bytes,
    const std::size_t maximum_payload_bytes)
{
    std::array<std::uint8_t, sawer_record_header_size> header{};
    if (!read_exact(input, header.data(), header.size())) {
        if (input.bad()) {
            record_error("I/O failure while reading header");
        }
        return input.gcount() == 0 ? SawerRecordReadResult::end_of_file
                                   : SawerRecordReadResult::truncated;
    }
    if (!std::equal(record_magic.begin(), record_magic.end(), header.begin())) {
        record_error("invalid frame magic");
    }
    if (header[4] != record_version) {
        record_error("unsupported frame version");
    }
    if (header[5] > static_cast<std::uint8_t>(SawerRecordKind::asset)
        || header[6] != 0U || header[7] != 0U) {
        record_error("invalid frame kind or flags");
    }
    const std::size_t metadata_size = read_u32_le(header, 16U);
    const std::uint64_t payload_size_u64 = read_u64_le(header, 20U);
    if (metadata_size > maximum_metadata_bytes
        || payload_size_u64 > maximum_payload_bytes
        || payload_size_u64 > std::numeric_limits<std::size_t>::max()) {
        record_error("frame size exceeds safety limit");
    }
    const std::size_t payload_size = static_cast<std::size_t>(payload_size_u64);
    std::vector<std::uint8_t> metadata(metadata_size);
    std::vector<std::uint8_t> payload(payload_size);
    if (!read_exact(input, metadata.data(), metadata.size())
        || !read_exact(input, payload.data(), payload.size())) {
        if (input.bad()) {
            record_error("I/O failure while reading body");
        }
        return SawerRecordReadResult::truncated;
    }
    if (read_u32_le(header, 28U) != crc32c(header, metadata, payload)) {
        throw SawerRecordCrcError{"Sawer record error: CRC32C mismatch"};
    }
    validate_cbor_subset(metadata);
    try {
        record.metadata = nlohmann::json::from_cbor(metadata, true, false);
    } catch (const std::exception&) {
        record_error("invalid CBOR metadata");
    }
    if (record.metadata.is_discarded() || !record.metadata.is_object()) {
        record_error("metadata is not a CBOR map");
    }
    record.kind = static_cast<SawerRecordKind>(header[5]);
    record.sequence = read_u64_le(header, 8U);
    record.payload = std::move(payload);
    return SawerRecordReadResult::record;
}

} // namespace sawer
