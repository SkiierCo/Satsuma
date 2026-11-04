// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs/vfs.h"

TEST_CASE("Unknown system archives return unavailable without formatting a null name",
          "[core][system_archive]") {
    for (const u64 id : {0ULL, 0x04008e001f098000ULL, std::numeric_limits<u64>::max()}) {
        CHECK_FALSE(FileSys::SystemArchive::SynthesizeSystemArchive(id));
    }
    CHECK_FALSE(FileSys::SystemArchive::SynthesizeSystemArchive(0x0100000000000800ULL));
    CHECK(FileSys::SystemArchive::SynthesizeSystemArchive(0x0100000000000809ULL));
}

TEST_CASE("Switch 2 system data without firmware mounts as an empty archive",
          "[core][system_archive]") {
    for (const u64 id : {0x0400000000000800ULL, 0x0400000000000810ULL, 0x0400000000000849ULL,
                         0x0400000000000FFFULL}) {
        const auto archive = FileSys::SystemArchive::SynthesizeSystemArchive(id);
        REQUIRE(archive);
        const auto root = FileSys::ExtractRomFS(archive);
        REQUIRE(root);
        CHECK(root->GetFiles().empty());
        CHECK(root->GetSubdirectories().empty());
    }
    CHECK_FALSE(FileSys::SystemArchive::SynthesizeSystemArchive(0x0400000000001000ULL));
}
