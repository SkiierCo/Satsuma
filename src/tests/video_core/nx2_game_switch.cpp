// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_game_switch.h"

namespace {

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

std::string ValueOf(const char* name, u64 program_id) {
    const char* value = VideoCommon::Nx2GameSwitchValue(name, program_id);
    return value == nullptr ? std::string{"<unset>"} : std::string{value};
}

} // Anonymous namespace

TEST_CASE("Nx2 per-game switches win over the global one", "[nx2_switches]") {
    constexpr const char* Name = "SATSUMA_TEST_GAME_SWITCH";
    constexpr const char* PerGame = "SATSUMA_TEST_GAME_SWITCH_0400486024DEC000";
    constexpr u64 Ocarina = 0x0400486024DEC000ULL;
    constexpr u64 Other = 0x0400C3F00006E000ULL;
    SetVariable(Name, nullptr);
    SetVariable(PerGame, nullptr);
    CHECK(ValueOf(Name, Ocarina) == "<unset>");

    SetVariable(PerGame, "1");
    CHECK(ValueOf(Name, Ocarina) == "1");
    CHECK(ValueOf(Name, Other) == "<unset>");

    SetVariable(Name, "0");
    CHECK(ValueOf(Name, Ocarina) == "1");
    CHECK(ValueOf(Name, Other) == "0");

    SetVariable(Name, nullptr);
    SetVariable(PerGame, nullptr);
}
