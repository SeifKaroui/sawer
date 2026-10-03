#include "storage/SawerRecord.hpp"
#include "core/Crc32c.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <array>
#include <random>

TEST_CASE("framed record bytes preserve the original CRC32C coverage")
{
    std::mt19937 random{1234U};
    for (const std::size_t size : {0U, 1U, 8U, 33U, 8192U}) {
        sawer::SawerRecord record{
            .kind = sawer::SawerRecordKind::asset,
            .sequence = 0x1020304050607080ULL,
            .metadata = {{"op", "asset_put"}, {"width", 128U}},
            .payload = std::vector<std::uint8_t>(size),
        };
        for (auto& byte : record.payload) byte = static_cast<std::uint8_t>(random());
        std::stringstream output{std::ios::in | std::ios::out | std::ios::binary};
        sawer::write_sawer_record(output, record);
        const std::string bytes = output.str();
        // Independent reference reproduces the previous on-disk checksum.
        std::uint32_t reference = 0xFFFFFFFFU;
        for (std::size_t index = 4U; index < bytes.size(); ++index) {
            if (index >= 28U && index < 32U) continue;
            reference ^= static_cast<std::uint8_t>(bytes[index]);
            for (unsigned bit = 0U; bit < 8U; ++bit)
                reference = (reference >> 1U)
                    ^ ((reference & 1U) != 0U ? 0x82F63B78U : 0U);
        }
        reference = ~reference;
        for (std::size_t index = 0U; index < 4U; ++index) {
            REQUIRE(static_cast<std::uint8_t>(bytes[28U + index])
                == static_cast<std::uint8_t>(reference >> (index * 8U)));
        }
        sawer::SawerRecord loaded;
        REQUIRE(sawer::read_sawer_record(output, loaded, 1024U, 8192U)
            == sawer::SawerRecordReadResult::record);
        REQUIRE(loaded.payload == record.payload);
        REQUIRE(loaded.metadata == record.metadata);
        REQUIRE(loaded.sequence == record.sequence);
    }
}

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
