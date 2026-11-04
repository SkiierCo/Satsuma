// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/engines/maxwell_3d.h"

TEST_CASE("Native inline constants preserve table selection and sequential word writes",
          "[video_core][native_root]") {
    Tegra::Engines::Maxwell3D::RootTables root;
    REQUIRE_FALSE(root.loaded);
    for (u32 table = 0; table < 8; ++table) {
        root.Select(table);
        for (u32 word = 0; word < 64; ++word)
            root.Load(0x10000000U + table * 64 + word);
        REQUIRE(root.offset == 256);
        const auto before = root.tables;
        root.Load(0xffffffff);
        REQUIRE(root.tables == before);
    }
    REQUIRE(root.loaded);
    for (u32 table = 0; table < 8; ++table)
        for (u32 word = 0; word < 64; ++word)
            REQUIRE(root.tables[table][word] == 0x10000000U + table * 64 + word);
    root.Select(3 | (12U << 8));
    root.Load(0x12345678);
    REQUIRE(root.tables[3][3] == 0x12345678);
    const auto before = root.tables;
    root.Select(2 | (3U << 8));
    root.Load(0xffffffff);
    REQUIRE(root.tables == before);
    REQUIRE(root.offset == 7);
}
