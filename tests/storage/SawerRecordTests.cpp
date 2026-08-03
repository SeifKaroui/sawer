#include "storage/SawerRecord.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

TEST_CASE("framed records round trip deterministic CBOR metadata")
{
    std::stringstream output{std::ios::in | std::ios::out | std::ios::binary};
    sawer::write_sawer_file_prologue(output);
    sawer::write_sawer_record(output, {
        .kind = sawer::SawerRecordKind::file_header,
        .sequence = 0U,
        .metadata = {{"format", "sawer"}},
        .payload = {},
    });
    output.seekg(0);
    sawer::validate_sawer_file_prologue(output);
    sawer::SawerRecord record;
    REQUIRE(sawer::read_sawer_record(output, record, 1024U, 0U)
        == sawer::SawerRecordReadResult::record);
    REQUIRE(record.metadata.at("format") == "sawer");
}
