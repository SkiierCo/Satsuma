// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string_view>
#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/hle/service/vi/display_timing.h"

TEST_CASE("Display refresh targets do not alter the configured speed limit",
          "[video_core][display_timing]") {
    const auto speed = Settings::SpeedLimit();
    CHECK(Service::VI::ParseDisplayRefreshRate("60") == 60);
    CHECK(Service::VI::ParseDisplayRefreshRate("120") == 120);
    CHECK(Settings::SpeedLimit() == speed);
    CHECK(Service::VI::DisplayFramePeriod().count() ==
          1'000'000'000LL / Service::VI::DisplayRefreshRate());
}

TEST_CASE("Invalid display targets retain the normal 60 Hz cadence", "[video_core][display_timing]") {
    for (const auto text : std::array<std::string_view, 9>{
             "", "0", "-120", "29", "241", "120junk", "120.0", "invalid", "9999999999999"}) {
        CHECK(Service::VI::ParseDisplayRefreshRate(text) == 60);
    }
}

TEST_CASE("Presents follow the swap interval and refresh rate the game asks for",
          "[video_core][display_timing]") {
    using Service::VI::GamePresentTiming;
    using Service::VI::PresentPeriod;
    // Fire Emblem: Fortune's Weave after its logos: two 60 Hz vblanks per frame.
    const auto fe = GamePresentTiming(0x2003c, 120);
    CHECK(fe.swap_interval == 2);
    CHECK(fe.refresh_rate == 60);
    CHECK(PresentPeriod(fe, 100).count() == 1'000'000'000LL / 30);
    // Its logos: every 60 Hz vblank.
    CHECK(PresentPeriod(GamePresentTiming(0x1003c, 120), 100).count() == 1'000'000'000LL / 60);
    // A game asking for 120 Hz gets it on a 120 Hz display, and the display caps a faster request.
    CHECK(PresentPeriod(GamePresentTiming(0x10078, 120), 100).count() == 1'000'000'000LL / 120);
    CHECK(GamePresentTiming(0x100f0, 120).refresh_rate == 120);
    CHECK(GamePresentTiming(0x10078, 60).refresh_rate == 60);
    // A variable refresh target between the fixed rates (90 FPS).
    CHECK(PresentPeriod(GamePresentTiming(0x1005a, 120), 100).count() == 1'000'000'000LL / 90);
}

TEST_CASE("Presents without a requested rate keep the display cadence",
          "[video_core][display_timing]") {
    using Service::VI::GamePresentTiming;
    for (const u32 word : {0U, 0x10000U, 0x1001dU, 0x100f1U}) {
        const auto timing = GamePresentTiming(word, 120);
        CHECK(timing.refresh_rate == 120);
        CHECK(timing.swap_interval == 1);
    }
    // The interval stays within 1..4 vblanks, and EDEN_NX2_SWAP_INTERVAL overrides it.
    CHECK(GamePresentTiming(0x9003c, 120).swap_interval == 4);
    CHECK(GamePresentTiming(0x2003c, 120, 1).swap_interval == 1);
}

TEST_CASE("Present pacing scales with the speed limit", "[video_core][display_timing]") {
    using Service::VI::GamePresentTiming;
    using Service::VI::PresentPeriod;
    const auto timing = GamePresentTiming(0x1003c, 120);
    CHECK(PresentPeriod(timing, 200).count() * 2 == PresentPeriod(timing, 100).count());
    CHECK(PresentPeriod(timing, 0).count() > 0);
}
