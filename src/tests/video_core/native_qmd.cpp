// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/savestate/archive.h"
#include "video_core/native_qmd.h"

namespace {
using Scheduler = Tegra::NativeQmdScheduler;

struct QmdFixture {
    QmdFixture() {
        Scheduler::ResetForNewSession();
    }
    ~QmdFixture() {
        Scheduler::ResetForNewSession();
    }
    Scheduler first;
    Scheduler second;
    Scheduler::Descriptor descriptor{};
    u32 releases{};

    std::string_view Signal(Scheduler& scheduler, u32 action) {
        return scheduler.Signal(
            0x10000, action,
            [&](u64 address, Scheduler::Descriptor& out) {
                REQUIRE(address == 0x10000);
                out = descriptor;
                return true;
            },
            [&](u64 address, u32 value) {
                CHECK(address == 0x9000);
                CHECK(value == 42);
                ++releases;
                return true;
            });
    }

    void InitializeDescriptor(u32 dependencies) {
        descriptor[4] = dependencies << 14;
        descriptor[18] = 0x30;
        descriptor[24] = 0x9000;
        descriptor[25] = (1U << 23) | (1U << 30);
        descriptor[26] = 42;
    }
};

TEST_CASE("QMD snapshot restores cross-channel dependencies without early releases",
          "[video_core][savestate][qmd]") {
    QmdFixture fixture;
    fixture.InitializeDescriptor(2);
    REQUIRE(fixture.Signal(fixture.first, 3).empty());
    CHECK(fixture.releases == 0);
    REQUIRE(fixture.Signal(fixture.second, 7).empty());
    CHECK(fixture.releases == 0);
    const auto saved = Scheduler::SaveState();
    Scheduler::ResetForNewSession();
    Scheduler::LoadState(saved);
    CHECK(Scheduler::SaveState() == saved);
    REQUIRE(fixture.Signal(fixture.second, 7).empty());
    CHECK(fixture.releases == 1);
}

TEST_CASE("QMD snapshot preserves decrements received before the descriptor is loaded",
          "[video_core][savestate][qmd]") {
    QmdFixture fixture;
    fixture.InitializeDescriptor(1);
    REQUIRE(fixture.Signal(fixture.second, 7).empty());
    const auto saved = Scheduler::SaveState();
    Scheduler::ResetForNewSession();
    Scheduler::LoadState(saved);
    REQUIRE(fixture.Signal(fixture.first, 3).empty());
    CHECK(fixture.releases == 1);
}

TEST_CASE("A new GPU session cannot inherit a previous game's deferred QMD counters",
          "[video_core][qmd]") {
    QmdFixture fixture;
    fixture.InitializeDescriptor(1);
    REQUIRE(fixture.Signal(fixture.first, 7).empty());
    Scheduler::ResetForNewSession();
    REQUIRE(fixture.Signal(fixture.second, 3).empty());
    CHECK(fixture.releases == 0);
    REQUIRE(fixture.Signal(fixture.first, 7).empty());
    CHECK(fixture.releases == 1);
}

TEST_CASE("Invalid QMD snapshots cannot partially change the live scheduler",
          "[video_core][savestate][qmd]") {
    QmdFixture fixture;
    fixture.InitializeDescriptor(1);
    REQUIRE(fixture.Signal(fixture.first, 3).empty());
    const auto saved = Scheduler::SaveState();
    for (size_t size = 0; size < saved.size(); ++size) {
        CHECK_THROWS_AS(Scheduler::LoadState(std::span{saved}.first(size)), Core::SaveState::Error);
        CHECK(Scheduler::SaveState() == saved);
    }
    auto invalid = saved;
    invalid[0] = 2;
    CHECK_THROWS_AS(Scheduler::LoadState(invalid), Core::SaveState::Error);
    invalid = saved;
    invalid[4] = 0xff;
    invalid[5] = 0xff;
    CHECK_THROWS_AS(Scheduler::LoadState(invalid), Core::SaveState::Error);
    invalid = saved;
    invalid[24] = 0x80;
    CHECK_THROWS_AS(Scheduler::LoadState(invalid), Core::SaveState::Error);
    invalid = saved;
    invalid.push_back(0);
    CHECK_THROWS_AS(Scheduler::LoadState(invalid), Core::SaveState::Error);
    CHECK(Scheduler::SaveState() == saved);
    REQUIRE(fixture.Signal(fixture.second, 7).empty());
    CHECK(fixture.releases == 1);
}
} // namespace
