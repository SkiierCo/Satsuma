// SPDX-License-Identifier: GPL-3.0-or-later

// Tool, not run by default: tests.exe "[.nsoextract]" with SATSUMA_NSO_IN set to a game's NSO and
// SATSUMA_NSO_OUT to an output path. Writes the module as an uncompressed NSO (flags 0, each
// segment stored whole), so tools/nx2 can read Switch 2 modules whose segments use ZBIC.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"

TEST_CASE("Extract an NSO to an uncompressed one", "[.nsoextract]") {
    const char* const in_path = std::getenv("SATSUMA_NSO_IN");
    const char* const out_path = std::getenv("SATSUMA_NSO_OUT");
    REQUIRE(in_path != nullptr);
    REQUIRE(out_path != nullptr);
    std::ifstream in(in_path, std::ios::binary);
    const std::vector<u8> file{std::istreambuf_iterator<char>(in), {}};
    REQUIRE(file.size() >= 0x100);
    REQUIRE(std::memcmp(file.data(), "NSO0", 4) == 0);
    const auto word = [&](size_t offset) {
        u32 value;
        std::memcpy(&value, file.data() + offset, sizeof(value));
        return value;
    };
    u32 flags = word(0xc);
    std::vector<u8> out(file.begin(), file.begin() + 0x100);
    for (u32 i = 0; i < 3; ++i) {
        const u32 offset = word(0x10 + i * 0x10);
        const u32 size = word(0x10 + i * 0x10 + 8);
        const u32 stored = word(0x60 + i * 4);
        std::vector<u8> segment(size);
        if ((flags >> i) & 1) {
            REQUIRE(offset + u64{stored} <= file.size());
            const std::span<const u8> source{file.data() + offset, stored};
            const int result = Common::Compression::IsZBIC(source)
                                   ? Common::Compression::DecompressDataZBIC(segment, source)
                                   : Common::Compression::DecompressDataLZ4(
                                         segment.data(), segment.size(), source.data(), source.size());
            REQUIRE(result == static_cast<int>(size));
        } else {
            REQUIRE(offset + u64{size} <= file.size());
            std::memcpy(segment.data(), file.data() + offset, size);
        }
        const u32 new_offset = static_cast<u32>(out.size());
        std::memcpy(out.data() + 0x10 + i * 0x10, &new_offset, 4);
        std::memcpy(out.data() + 0x60 + i * 4, &size, 4);
        out.insert(out.end(), segment.begin(), segment.end());
    }
    flags = 0; // uncompressed, no segment hashes to check
    std::memcpy(out.data() + 0xc, &flags, 4);
    std::ofstream(out_path, std::ios::binary)
        .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}
