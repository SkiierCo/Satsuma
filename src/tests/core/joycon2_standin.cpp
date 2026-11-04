// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "hid_core/hid_types.h"

TEST_CASE("Switch 1 controllers stand in for Joy-Con 2 only when a game asks for it",
          "[hid][joycon2]") {
    using namespace Core::HID;
    // Switch Sports at its Joy-Con 2 prompt: handheld and Joy-Con 2.
    const NpadStyleTag sports{static_cast<NpadStyleSet>(0x1002)};
    CHECK(sports.joycon2);
    for (const auto type : {NpadStyleIndex::Fullkey, NpadStyleIndex::JoyconDual,
                            NpadStyleIndex::JoyconLeft, NpadStyleIndex::JoyconRight}) {
        CHECK(StandsInForJoyCon2(sports, type, nullptr));
        CHECK_FALSE(StandsInForJoyCon2(sports, type, "0"));
    }
    CHECK_FALSE(StandsInForJoyCon2(sports, NpadStyleIndex::GameCube, nullptr));
    CHECK_FALSE(StandsInForJoyCon2(sports, NpadStyleIndex::None, nullptr));
    // Its menus (0x1f) and other games ask for Switch 1 styles only: nothing changes there.
    const NpadStyleTag menus{static_cast<NpadStyleSet>(0x1f)};
    CHECK_FALSE(StandsInForJoyCon2(menus, NpadStyleIndex::JoyconDual, nullptr));
}
