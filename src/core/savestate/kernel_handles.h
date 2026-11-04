// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

#include "core/savestate/archive.h"

namespace Kernel {
class KAutoObject;
class KHandleTable;
class KernelCore;
} // namespace Kernel

namespace Core::SaveState {

struct HandleSlot {
    u64 object_id{};
    u32 entry_info{};
    auto operator<=>(const HandleSlot&) const = default;
};

struct HandleTableState {
    u32 allocated_count{};
    u32 maximum_count{};
    u32 next_serial{1};
    s32 free_head{-1};
    std::vector<HandleSlot> slots;
    auto operator<=>(const HandleTableState&) const = default;
};

std::vector<u8> EncodeHandleTable(const HandleTableState& table);
HandleTableState DecodeHandleTable(std::span<const u8> bytes);

class HandleTableAccess {
public:
    // Both operations require a machine-wide quiescent boundary. IDs are assigned by the
    // checkpoint's kernel object registry, never by converting a host pointer to an integer.
    static HandleTableState Capture(const Kernel::KHandleTable& table,
                                    const std::function<u64(Kernel::KAutoObject*)>& identify);
    // Only a fresh candidate table may be reconstructed. Referenced objects must already
    // exist and remain owned by the candidate kernel throughout this operation.
    static void RestoreFresh(Kernel::KernelCore& kernel, Kernel::KHandleTable& table,
                             const HandleTableState& state,
                             const std::function<Kernel::KAutoObject*(u64)>& resolve);
};

} // namespace Core::SaveState
