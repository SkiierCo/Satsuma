// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon {

/// The value of a Switch 2 switch for one game: NAME_<program ID> (16 hex digits) wins over
/// NAME. Null when neither is set.
inline const char* Nx2GameSwitchValue(const char* name, u64 program_id) {
    const std::string per_game = fmt::format("{}_{:016X}", name, program_id);
    if (const char* value = std::getenv(per_game.c_str())) {
        return value;
    }
    return std::getenv(name);
}

/// An on/off switch ("1" on) that can be set per game. Several games can run in one session, so
/// the value is looked up again when the running game changes.
class Nx2GameSwitch {
public:
    explicit constexpr Nx2GameSwitch(const char* name_) : name{name_} {}

    bool Enabled() {
        const u64 program_id = Settings::GetCurrentProgramID();
        if (cached_program.load(std::memory_order_acquire) != program_id) {
            const char* const value = Nx2GameSwitchValue(name, program_id);
            cached.store(value != nullptr && value[0] == '1', std::memory_order_relaxed);
            cached_program.store(program_id, std::memory_order_release);
        }
        return cached.load(std::memory_order_relaxed);
    }

private:
    const char* name;
    std::atomic<u64> cached_program{~u64{0}};
    std::atomic<bool> cached{false};
};

} // namespace VideoCommon
