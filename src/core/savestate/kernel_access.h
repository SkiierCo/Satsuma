// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>

#include "common/common_types.h"
#include "core/savestate/kernel_state.h"
#include "core/savestate/memory.h"

namespace Core {
class System;
}

namespace Kernel {
class KAutoObject;
class KProcess;
class KThread;
class KTransferMemory;
} // namespace Kernel

namespace Core::SaveState {

// The only code with access to kernel internals that have no public interface. Every function
// requires a checkpoint (capture) or a process that has been loaded but not started (restore).
class KernelAccess {
public:
    static ProcessLayout ReadLayout(const Kernel::KProcess& process);
    static void WriteLayout(Core::System& system, Kernel::KProcess& process,
                            const ProcessLayout& layout);

    static std::vector<TlsPageRecord> ReadTlsPages(const Kernel::KProcess& process);
    // Removes the thread-local pages created while loading (the process-local region).
    static void ReleaseFreshThreadLocalPages(Core::System& system, Kernel::KProcess& process);
    // Maps the saved thread-local pages and reserves the saved regions, including plr_address.
    static void RestoreThreadLocalPages(Core::System& system, Kernel::KProcess& process,
                                        const std::vector<TlsPageRecord>& pages,
                                        u64 plr_address);

    // Maps the main thread's stack at a fixed address, with the memory reservation
    // KProcess::Run makes for it.
    static void MapMainStack(Core::System& system, Kernel::KProcess& process, u64 address,
                             u64 size);

    // Initializes the handle table and marks the process running, as KProcess::Run does.
    static void PrepareProcessForThreads(Core::System& system, Kernel::KProcess& process);

    // Thread state other than registers and hold (filled by the caller).
    static void ReadThread(Core::System& system, const Kernel::KThread& thread,
                           ThreadRecord& record,
                           const std::function<u64(Kernel::KAutoObject*)>& identify);
    // Creates a user thread in the Initialized state with the saved ID, TLS slot, priority,
    // affinity and registers.
    static Kernel::KThread* CreateThread(Core::System& system, Kernel::KProcess& process,
                                         const ThreadRecord& record);
    // Starts a created thread in its saved hold: runnable, waiting on a condition variable or
    // address, terminated, or not started. lock_owner is the mutex owner for a waiter that
    // was already signalled.
    static void StartThread(Core::System& system, Kernel::KProcess& process,
                            Kernel::KThread& thread, const ThreadRecord& record,
                            Kernel::KThread* lock_owner);

    static TransferMemoryRecord ReadTransferMemory(const Kernel::KTransferMemory& memory);
};

} // namespace Core::SaveState
