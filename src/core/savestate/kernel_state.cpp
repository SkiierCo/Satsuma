// SPDX-License-Identifier: GPL-3.0-or-later

#include <unordered_map>

#include <fmt/format.h>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/kernel.h"
#include "core/savestate/kernel_access.h"
#include "core/savestate/kernel_state.h"

namespace Core::SaveState {

namespace {

constexpr u32 KernelObjectsTag = Tag("KOBJ");
constexpr u32 KernelObjectsVersion = 1;

void EncodeContext(Writer& w, const Kernel::Svc::ThreadContext& c) {
    for (u64 v : c.r) {
        w.U64(v);
    }
    w.U64(c.fp);
    w.U64(c.lr);
    w.U64(c.sp);
    w.U64(c.pc);
    w.U32(c.pstate);
    for (const auto& v : c.v) {
        w.U64(v[0]);
        w.U64(v[1]);
    }
    w.U32(c.fpcr);
    w.U32(c.fpsr);
    w.U64(c.tpidr);
}

Kernel::Svc::ThreadContext DecodeContext(Reader& r) {
    Kernel::Svc::ThreadContext c{};
    for (u64& v : c.r) {
        v = r.U64();
    }
    c.fp = r.U64();
    c.lr = r.U64();
    c.sp = r.U64();
    c.pc = r.U64();
    c.pstate = r.U32();
    for (auto& v : c.v) {
        v[0] = r.U64();
        v[1] = r.U64();
    }
    c.fpcr = r.U32();
    c.fpsr = r.U32();
    c.tpidr = r.U64();
    return c;
}

std::string Describe(Kernel::KAutoObject* object) {
    return object->GetTypeName();
}

} // namespace

void EncodeKernelObjects(Writer& w, const KernelObjectsState& s) {
    w.Section(KernelObjectsTag, KernelObjectsVersion);
    w.U64(s.process_id);
    w.U64(s.process_object);
    w.U64(s.resource_limit_object);
    w.U64(s.next_thread_id);
    w.U64(s.threads.size());
    for (const auto& t : s.threads) {
        w.U64(t.id);
        w.U64(t.thread_id);
        w.Bool(t.is_main);
        w.U64(t.argument);
        w.U64(t.stack_top);
        w.S32(t.base_priority);
        w.S32(t.ideal_core);
        w.U64(t.affinity_mask);
        w.U64(t.tls_address);
        w.U8(static_cast<u8>(t.hold));
        w.Bool(t.paused_by_game);
        EncodeContext(w, t.context);
        w.U64(t.wait_key);
        w.U64(t.wait_address);
        w.U32(t.wait_value);
        w.U64(t.lock_owner);
        w.S64(t.timeout_ns);
    }
    w.U64(s.events.size());
    for (const auto& e : s.events) {
        w.U64(e.event_id);
        w.U64(e.readable_id);
    }
    w.U64(s.transfer_memories.size());
    for (const auto& t : s.transfer_memories) {
        w.U64(t.id);
        w.U64(t.address);
        w.U64(t.size);
        w.U32(t.permission);
    }
    w.U64(s.event_states.size());
    for (const auto& [id, signaled] : s.event_states) {
        w.U64(id);
        w.Bool(signaled);
    }
    w.Blob(EncodeHandleTable(s.handles));
}

KernelObjectsState DecodeKernelObjects(Reader& r) {
    r.Section(KernelObjectsTag, KernelObjectsVersion);
    KernelObjectsState s;
    s.process_id = r.U64();
    s.process_object = r.U64();
    s.resource_limit_object = r.U64();
    s.next_thread_id = r.U64();
    const auto count = [&](size_t min_bytes) {
        const u64 n = r.U64();
        if (n > r.Remaining() / min_bytes) {
            throw Error("Truncated save state");
        }
        return static_cast<size_t>(n);
    };
    s.threads.resize(count(600));
    for (auto& t : s.threads) {
        t.id = r.U64();
        t.thread_id = r.U64();
        t.is_main = r.Bool();
        t.argument = r.U64();
        t.stack_top = r.U64();
        t.base_priority = r.S32();
        t.ideal_core = r.S32();
        t.affinity_mask = r.U64();
        t.tls_address = r.U64();
        const u8 hold = r.U8();
        if (hold > static_cast<u8>(ThreadHold::Terminated)) {
            throw Error("Invalid thread state in save state");
        }
        t.hold = static_cast<ThreadHold>(hold);
        t.paused_by_game = r.Bool();
        t.context = DecodeContext(r);
        t.wait_key = r.U64();
        t.wait_address = r.U64();
        t.wait_value = r.U32();
        t.lock_owner = r.U64();
        t.timeout_ns = r.S64();
    }
    s.events.resize(count(16));
    for (auto& e : s.events) {
        e.event_id = r.U64();
        e.readable_id = r.U64();
    }
    s.transfer_memories.resize(count(28));
    for (auto& t : s.transfer_memories) {
        t.id = r.U64();
        t.address = r.U64();
        t.size = r.U64();
        t.permission = r.U32();
    }
    s.event_states.resize(count(9));
    for (auto& [id, signaled] : s.event_states) {
        id = r.U64();
        signaled = r.Bool();
    }
    const auto handles = r.Blob();
    s.handles = DecodeHandleTable(handles);
    return s;
}

KernelObjectsState CaptureKernelObjects(Core::System& system, Kernel::KProcess& process,
                                        ObjectIds& ids) {
    auto& kernel = system.Kernel();
    KernelObjectsState s;
    s.process_id = process.GetProcessId();
    s.process_object = ids.Id(&process);
    s.resource_limit_object = ids.Id(process.GetResourceLimit());
    s.next_thread_id = kernel.PeekNextThreadID();
    const auto identify = [&](Kernel::KAutoObject* object) { return ids.Id(object); };

    {
        Kernel::KScopedLightLock ll{process.GetListLock()};
        for (auto& thread : process.GetThreadList()) {
            ThreadRecord t;
            t.id = ids.Id(&thread);
            t.hold = Checkpoint::Classify(thread);
            KernelAccess::ReadThread(system, thread, t, identify);
            s.threads.push_back(t);
        }
    }

    s.handles = HandleTableAccess::Capture(process.GetHandleTable(), identify);

    // Classify everything the handle table and threads reference. Objects provided by services
    // are restored by their service; everything else must be the game's own.
    std::unordered_map<Kernel::KAutoObject*, bool> seen_event;
    for (size_t i = 0; i < ids.Objects().size(); ++i) {
        Kernel::KAutoObject* object = ids.Objects()[i];
        if (auto* readable = object->DynamicCast<Kernel::KReadableEvent*>(); readable) {
            s.event_states.emplace_back(ids.Id(readable), readable->IsSignaled(kernel));
            if (ids.IsProvided(object)) {
                continue;
            }
            Kernel::KEvent* parent = readable->GetParent();
            if (parent == nullptr || parent->GetOwner() != &process) {
                throw Error("The game holds an event created by a service that save states do "
                            "not support yet");
            }
            if (!seen_event.try_emplace(parent, true).second) {
                continue;
            }
            s.events.push_back({ids.Has(parent) ? ids.Id(parent) : 0, ids.Id(readable)});
            continue;
        }
        if (ids.IsProvided(object)) {
            continue;
        }
        if (object == &process || object == process.GetResourceLimit()) {
            continue;
        }
        if (auto* event = object->DynamicCast<Kernel::KEvent*>(); event) {
            if (event->GetOwner() != &process) {
                throw Error("The game holds an event created by a service that save states do "
                            "not support yet");
            }
            if (seen_event.try_emplace(event, true).second) {
                s.events.push_back({ids.Id(event), ids.Id(&event->GetReadableEvent())});
            } else {
                for (auto& e : s.events) {
                    if (e.readable_id == ids.Id(&event->GetReadableEvent())) {
                        e.event_id = ids.Id(event);
                    }
                }
            }
            continue;
        }
        if (object->DynamicCast<Kernel::KThread*>() != nullptr) {
            const auto* thread = object->DynamicCast<Kernel::KThread*>();
            if (thread->GetOwnerProcess() != &process) {
                throw Error("The game holds a thread of another process");
            }
            continue;
        }
        if (auto* memory = object->DynamicCast<Kernel::KTransferMemory*>(); memory) {
            auto record = KernelAccess::ReadTransferMemory(*memory);
            record.id = ids.Id(memory);
            s.transfer_memories.push_back(record);
            continue;
        }
        throw Error(fmt::format("The game holds a kernel object save states do not support "
                                "yet: {}",
                                Describe(object)));
    }
    return s;
}

KernelRestore::KernelRestore(Core::System& system_, Kernel::KProcess& process_,
                             const KernelObjectsState& state_, ObjectTable& table_)
    : system{system_}, process{process_}, state{state_}, table{table_} {}

KernelRestore::~KernelRestore() {
    for (auto* object : creator_references) {
        object->Close(system.Kernel());
    }
}

void KernelRestore::CreateObjects() {
    auto& kernel = system.Kernel();
    if (process.GetProcessId() != state.process_id) {
        throw Error(fmt::format("The restored game has process ID {:#x}, the state {:#x}",
                                process.GetProcessId(), state.process_id));
    }
    table.Bind(state.process_object, &process);
    table.Bind(state.resource_limit_object, process.GetResourceLimit());
    KernelAccess::PrepareProcessForThreads(system, process);

    for (const auto& e : state.events) {
        Kernel::KScopedResourceReservation reservation(
            kernel, &process, Kernel::Svc::LimitableResource::EventCountMax);
        if (!reservation.Succeeded()) {
            throw Error("The event limit was reached while restoring events");
        }
        Kernel::KEvent* event = Kernel::KEvent::Create(kernel);
        if (event == nullptr) {
            throw Error("Out of kernel memory for events");
        }
        event->Initialize(kernel, &process);
        reservation.Commit();
        Kernel::KEvent::Register(kernel, event);
        creator_references.push_back(&event->GetReadableEvent());
        creator_references.push_back(event);
        if (e.event_id != 0) {
            table.Bind(e.event_id, event);
        }
        table.Bind(e.readable_id, &event->GetReadableEvent());
    }

    for (const auto& t : state.transfer_memories) {
        Kernel::KScopedResourceReservation reservation(
            kernel, &process, Kernel::Svc::LimitableResource::TransferMemoryCountMax);
        if (!reservation.Succeeded()) {
            throw Error("The transfer memory limit was reached while restoring");
        }
        Kernel::KTransferMemory* memory = Kernel::KTransferMemory::Create(kernel);
        if (memory == nullptr) {
            throw Error("Out of kernel memory for transfer memory");
        }
        creator_references.push_back(memory);
        if (memory->Initialize(kernel, t.address, t.size,
                               static_cast<Kernel::Svc::MemoryPermission>(t.permission))
                .IsError()) {
            throw Error(fmt::format("Could not restore transfer memory at {:#x}", t.address));
        }
        reservation.Commit();
        Kernel::KTransferMemory::Register(kernel, memory);
        table.Bind(t.id, memory);
    }

    for (const auto& t : state.threads) {
        Kernel::KThread* thread = KernelAccess::CreateThread(system, process, t);
        creator_references.push_back(thread);
        table.Bind(t.id, thread);
    }
}

void KernelRestore::Finish() {
    auto& kernel = system.Kernel();
    HandleTableAccess::RestoreFresh(kernel, process.GetHandleTable(), state.handles,
                                    [&](u64 id) { return table.Find(id); });

    for (const auto& [id, signaled] : state.event_states) {
        auto* event = table.Get<Kernel::KReadableEvent>(id);
        if (signaled) {
            event->Signal(kernel);
        } else {
            event->Clear(kernel);
        }
    }
}

void KernelRestore::StartThreads() {
    auto& kernel = system.Kernel();
    for (const auto& t : state.threads) {
        auto* thread = table.Get<Kernel::KThread>(t.id);
        auto* owner = t.lock_owner != 0 ? table.Get<Kernel::KThread>(t.lock_owner) : nullptr;
        KernelAccess::StartThread(system, process, *thread, t, owner);
    }
    kernel.EnsureNextThreadIDAbove(state.next_thread_id - 1);
}

} // namespace Core::SaveState
