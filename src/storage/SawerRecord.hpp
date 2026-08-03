#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace sawer {

enum class SawerRecordKind : std::uint8_t {
    file_header = 0,
    operation = 1,
    asset = 2,
};

struct SawerRecord final {
    SawerRecordKind kind{SawerRecordKind::operation};
    std::uint64_t sequence{};
    nlohmann::json metadata;
    std::vector<std::uint8_t> payload;
};

enum class SawerRecordReadResult {
    end_of_file,
    record,
    truncated,
};

// A CRC mismatch is distinguished from structural and CBOR failures so the
// board reader can recover a fully written but torn final append only.
class SawerRecordCrcError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t sawer_record_header_size = 32U;
inline constexpr std::size_t sawer_file_prologue_size = 8U;

void write_sawer_file_prologue(std::ostream& output);
void validate_sawer_file_prologue(std::istream& input);
void write_sawer_record(std::ostream& output, const SawerRecord& record);
[[nodiscard]] SawerRecordReadResult read_sawer_record(
    std::istream& input,
    SawerRecord& record,
    std::size_t maximum_metadata_bytes,
    std::size_t maximum_payload_bytes);

} // namespace sawer
