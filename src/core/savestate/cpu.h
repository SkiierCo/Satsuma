// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/arm/arm_interface.h"
#include "core/savestate/archive.h"

namespace Core::SaveState {

// A guest thread's architectural registers, not a dump of a CPU backend or host fiber.
// Only capture at a machine-wide quiescent point, after scheduled-out contexts are saved.
struct ThreadRegisters {
    u64 process_id{};
    u64 thread_id{};
    Architecture architecture{};
    u64 tpidrro{};
    Kernel::Svc::ThreadContext context{};
};

std::vector<u8> EncodeThreadRegisters(const ThreadRegisters& thread);
ThreadRegisters DecodeThreadRegisters(std::span<const u8> bytes);

} // namespace Core::SaveState
