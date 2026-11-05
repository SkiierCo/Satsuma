// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/hle/kernel/svc_types.h"
#include "core/savestate/checkpoint.h"
#include "core/savestate/kernel_handles.h"
#include "core/savestate/objects.h"
#include "core/savestate/stream.h"

namespace Core {
class System;
}

namespace Kernel {
class KAutoObject;
class KProcess;
} // namespace Kernel

namespace Core::SaveState {

struct ThreadRecord {
    u64 id{};
    u64 thread_id{};
    bool is_main{};
    u64 argument{};
    u64 stack_top{};
    s32 base_priority{};
    s32 ideal_core{};
    u64 affinity_mask{};
    u64 tls_address{};
    ThreadHold hold{};
    bool paused_by_game{};
    Kernel::Svc::ThreadContext context{};
    // ConditionWait / AddressWait.
    u64 wait_key{};          // condition variable key or arbiter address
    u64 wait_address{};      // mutex address (ConditionWait)
    u32 wait_value{};        // the thread's own mutex tag (ConditionWait)
    u64 lock_owner{};        // object ID of the mutex owner it waits for, or 0 (in the cv tree)
    s64 timeout_ns{-1};      // remaining time, or -1 without a timeout
};

struct EventRecord {
    u64 event_id{};    // the writable KEvent, or 0 when only its readable side is referenced
    u64 readable_id{};
};

struct TransferMemoryRecord {
    u64 id{};
    u64 address{};
    u64 size{};
    u32 permission{};
};

struct KernelObjectsState {
    u64 process_id{};
    u64 process_object{};
    u64 resource_limit_object{};
    u64 next_thread_id{};
    std::vector<ThreadRecord> threads;
    std::vector<EventRecord> events;
    std::vector<TransferMemoryRecord> transfer_memories;
    // Every referenced readable event, game-created or provided by a service: (id, signaled).
    std::vector<std::pair<u64, bool>> event_states;
    HandleTableState handles;
};

void EncodeKernelObjects(Writer& w, const KernelObjectsState& state);
KernelObjectsState DecodeKernelObjects(Reader& r);

// Capture at a checkpoint, after services have provided the objects they own. Throws an Error
// naming any referenced object no component can restore.
KernelObjectsState CaptureKernelObjects(Core::System& system, Kernel::KProcess& process,
                                        ObjectIds& ids);

// Restore phase 1, after RestoreMappings: recreates the game's events, transfer memory and
// threads (not started) and binds the process and its resource limit.
class KernelRestore {
public:
    KernelRestore(Core::System& system, Kernel::KProcess& process,
                  const KernelObjectsState& state, ObjectTable& table);
    ~KernelRestore();

    void CreateObjects();
    // Phase 2, after services have bound their objects: handle table and event states.
    void Finish();
    // Last, after guest memory contents are restored: starts the threads in their saved holds.
    void StartThreads();

private:
    Core::System& system;
    Kernel::KProcess& process;
    const KernelObjectsState& state;
    ObjectTable& table;
    std::vector<Kernel::KAutoObject*> creator_references;
};

} // namespace Core::SaveState
