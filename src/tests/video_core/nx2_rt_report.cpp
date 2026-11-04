// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_rt_report.h"

TEST_CASE("Render target reports count draws, skips and clears per window", "[video_core]") {
    namespace Report = VideoCommon::Nx2RtReport;
    static_cast<void>(Report::Take());
    const std::array<f32, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
    Report::NoteDraw();
    Report::NoteTarget(0x8105fa0000, 0x58ee1000, 1280, 720, 0xd2, 4, false, black, 0x1234);
    Report::NoteDraw();
    Report::NoteTarget(0x8105fa0000, 0x58ee1000, 1280, 720, 0xd2, 4, false, black, 0x5678);
    Report::NoteSkipped();
    Report::NoteClear();
    Report::NoteTarget(0x8100000000, 0x52f41000, 3840, 2160, 0xd1, 4, true, black, 0);

    const auto window = Report::Take();
    CHECK(window.draws == 2);
    CHECK(window.skipped == 1);
    CHECK(window.clears == 1);
    REQUIRE(window.targets.size() == 2);
    const auto& scene = window.targets.at(0x8105fa0000);
    CHECK(scene.draws == 2);
    CHECK(scene.first_ps == 0x1234);
    CHECK(scene.clears == 0);
    const auto& output = window.targets.at(0x8100000000);
    CHECK(output.clears == 1);
    CHECK(output.last_clear[3] == 1.0f);

    // The next window starts empty.
    CHECK(Report::Take().targets.empty());
}
