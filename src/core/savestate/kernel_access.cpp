// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>

#include <fmt/format.h>

#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_condition_variable.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_page_buffer.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread_local_page.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/savestate/kernel_access.h"

namespace Core::SaveState {

using namespace Kernel;

ProcessLayout KernelAccess::ReadLayout(const KProcess& process) {
    const KPageTableBase& t = process.m_page_table.GetBasePageTable();
    ProcessLayout l;
    l.program_id = process.m_program_id;
    l.code_address = GetInteger(process.m_code_address);
    l.code_size = process.m_code_size;
    l.regions = {
        GetInteger(t.m_heap_region_start),       GetInteger(t.m_heap_region_end),
        GetInteger(t.m_alias_region_start),      GetInteger(t.m_alias_region_end),
        GetInteger(t.m_stack_region_start),      GetInteger(t.m_stack_region_end),
        GetInteger(t.m_kernel_map_region_start), GetInteger(t.m_kernel_map_region_end),
        GetInteger(t.m_code_region_start),       GetInteger(t.m_code_region_end),
        GetInteger(t.m_alias_code_region_start), GetInteger(t.m_alias_code_region_end),
    };
    l.current_heap_end = GetInteger(t.m_current_heap_end);
    l.max_heap_size = t.m_max_heap_size;
    l.main_thread_stack_size = process.m_main_thread_stack_size;
    l.entropy = process.m_entropy;
    l.plr_address = GetInteger(process.m_plr_address);
    l.is_64bit = process.Is64Bit();
    return l;
}

void KernelAccess::WriteLayout(Core::System&, KProcess& process, const ProcessLayout& l) {
    KPageTableBase& t = process.m_page_table.GetBasePageTable();
    if (t.m_current_heap_end != t.m_heap_region_start) {
        throw Error("The process already has a heap");
    }
    t.m_heap_region_start = l.regions[0];
    t.m_heap_region_end = l.regions[1];
    t.m_alias_region_start = l.regions[2];
    t.m_alias_region_end = l.regions[3];
    t.m_stack_region_start = l.regions[4];
    t.m_stack_region_end = l.regions[5];
    t.m_kernel_map_region_start = l.regions[6];
    t.m_kernel_map_region_end = l.regions[7];
    t.m_code_region_start = l.regions[8];
    t.m_code_region_end = l.regions[9];
    t.m_alias_code_region_start = l.regions[10];
    t.m_alias_code_region_end = l.regions[11];
    t.m_current_heap_end = t.m_heap_region_start;
    process.m_entropy = l.entropy;
}

std::vector<TlsPageRecord> KernelAccess::ReadTlsPages(const KProcess& process) {
    std::vector<TlsPageRecord> out;
    const auto add = [&](const KThreadLocalPage& page) {
        u8 mask = 0;
        for (size_t i = 0; i < KThreadLocalPage::RegionsPerPage; ++i) {
            if (!page.m_is_region_free[i]) {
                mask |= static_cast<u8>(1U << i);
            }
        }
        out.push_back({GetInteger(page.GetAddress()), mask});
    };
    for (const auto& page : process.m_fully_used_tlp_tree) {
        add(page);
    }
    for (const auto& page : process.m_partially_used_tlp_tree) {
        add(page);
    }
    std::ranges::sort(out, {}, &TlsPageRecord::address);
    return out;
}

void KernelAccess::ReleaseFreshThreadLocalPages(Core::System& system, KProcess& process) {
    auto& kernel = system.Kernel();
    if (process.m_plr_address != 0) {
        if (process.DeleteThreadLocalRegion(kernel, process.m_plr_address).IsError()) {
            throw Error("Could not release the process-local region");
        }
        process.m_plr_address = 0;
    }
    if (!process.m_fully_used_tlp_tree.empty() || !process.m_partially_used_tlp_tree.empty()) {
        throw Error("The loaded process already has threads");
    }
}

void KernelAccess::RestoreThreadLocalPages(Core::System& system, KProcess& process,
                                           const std::vector<TlsPageRecord>& pages,
                                           u64 plr_address) {
    auto& kernel = system.Kernel();
    for (const auto& record : pages) {
        KPageBuffer* buffer = KPageBuffer::Allocate(kernel);
        if (buffer == nullptr) {
            throw Error("Out of kernel memory for thread-local pages");
        }
        const auto phys = system.DeviceMemory().GetPhysicalAddr(buffer);
        const Result result = process.m_page_table.GetBasePageTable().MapPagesAtForRestore(
            record.address, 1, phys, true, KMemoryState::ThreadLocal,
            KMemoryPermission::UserReadWrite);
        if (result.IsError()) {
            KPageBuffer::Free(kernel, buffer);
            throw Error(fmt::format("Could not map the thread-local page at {:#x}",
                                    record.address));
        }
        KThreadLocalPage* page = KThreadLocalPage::Allocate(kernel);
        if (page == nullptr) {
            throw Error("Out of kernel memory for thread-local pages");
        }
        page->m_owner = &process;
        page->m_kernel = &kernel;
        page->m_virt_addr = record.address;
        for (size_t i = 0; i < KThreadLocalPage::RegionsPerPage; ++i) {
            page->m_is_region_free[i] = (record.used_mask & (1U << i)) == 0;
        }
        KScopedSchedulerLock sl{kernel};
        if (page->IsAllUsed()) {
            process.m_fully_used_tlp_tree.insert(*page);
        } else {
            process.m_partially_used_tlp_tree.insert(*page);
        }
    }
    process.m_plr_address = plr_address;
}

void KernelAccess::MapMainStack(Core::System& system, KProcess& process, u64 address, u64 size) {
    auto& kernel = system.Kernel();
    if (process.m_main_thread_stack_size != 0) {
        throw Error("The process has two main thread stacks");
    }
    KScopedResourceReservation reservation(kernel, &process,
                                           Svc::LimitableResource::PhysicalMemoryMax, size);
    if (!reservation.Succeeded()) {
        throw Error("Out of memory for the main thread stack");
    }
    const Result result = process.m_page_table.GetBasePageTable().MapPagesAtForRestore(
        address, size / PageSize, 0, false, KMemoryState::Stack,
        KMemoryPermission::UserReadWrite);
    if (result.IsError()) {
        throw Error(fmt::format("Could not map the main thread stack at {:#x}", address));
    }
    reservation.Commit();
    process.m_main_thread_stack_size = size;
}

namespace {

// Wait queue for threads restored inside WaitProcessWideKeyAtomic or WaitForAddress. Their SVC
// has no host frame any more, so waking writes the SVC result to X0 directly; the saved PC is
// already past the svc instruction.
class RestoredWaitQueue final : public KThreadQueue {
public:
    RestoredWaitQueue(KernelCore& kernel, KConditionVariable::ThreadTree* tree)
        : KThreadQueue(kernel), m_tree{tree} {}

    void EndWait(KernelCore& kernel, KThread* thread, Result result) override {
        Complete(thread, result);
        KThreadQueue::EndWait(kernel, thread, result);
    }

    void CancelWait(KernelCore& kernel, KThread* thread, Result result,
                    bool cancel_timer_task) override {
        if (KThread* owner = thread->GetLockOwner(kernel); owner != nullptr) {
            owner->RemoveWaiter(kernel, thread);
        }
        if (thread->IsWaitingForConditionVariable()) {
            m_tree->erase(m_tree->iterator_to(*thread));
            thread->ClearConditionVariable();
        }
        Complete(thread, result);
        KThreadQueue::CancelWait(kernel, thread, result, cancel_timer_task);
    }

private:
    static void Complete(KThread* thread, Result result) {
        thread->GetContext().r[0] = result.raw;
        thread->SetCurrentSvc(KThread::NoSvc);
    }

    KConditionVariable::ThreadTree* m_tree;
};

// The queues must outlive their waits; a few objects per restore.
std::vector<std::unique_ptr<KThreadQueue>>& RestoredQueues() {
    static std::vector<std::unique_ptr<KThreadQueue>> queues;
    return queues;
}

} // namespace

void KernelAccess::PrepareProcessForThreads(Core::System& system, KProcess& process) {
    auto& kernel = system.Kernel();
    if (process.InitializeHandleTable(kernel, process.m_capabilities.GetHandleTableSize())
            .IsError()) {
        throw Error("Could not initialize the handle table");
    }
    {
        KScopedLightLock lk(process.m_state_lock);
        process.ChangeState(kernel, KProcess::State::Running);
    }
    process.Open(kernel);
}

void KernelAccess::ReadThread(Core::System& system, const KThread& thread, ThreadRecord& r,
                              const std::function<u64(KAutoObject*)>& identify) {
    auto& kernel = system.Kernel();
    r.thread_id = thread.m_thread_id;
    r.argument = thread.m_argument;
    r.stack_top = GetInteger(thread.m_stack_top);
    r.base_priority = thread.m_base_priority;
    r.ideal_core = thread.m_virtual_ideal_core_id;
    r.affinity_mask = thread.m_virtual_affinity_mask;
    r.tls_address = GetInteger(thread.m_tls_address);
    r.paused_by_game = thread.IsSuspendRequested(SuspendType::Thread);
    r.context = thread.m_thread_context;
    if (r.hold == ThreadHold::ConditionWait || r.hold == ThreadHold::AddressWait) {
        r.wait_key = thread.m_condvar_key;
        r.wait_address = GetInteger(thread.m_address_key);
        r.wait_value = thread.m_address_key_value;
        if (r.hold == ThreadHold::ConditionWait && thread.m_condvar_tree == nullptr) {
            KThread* owner = thread.m_waiting_lock_info != nullptr
                                 ? thread.m_waiting_lock_info->GetOwner()
                                 : nullptr;
            if (owner == nullptr) {
                throw Error(fmt::format("Thread {} waits for a mutex without an owner",
                                        thread.m_thread_id));
            }
            r.lock_owner = identify(owner);
        }
        const s64 when = thread.GetTime();
        r.timeout_ns =
            when > 0 ? std::max<s64>(0, when - kernel.HardwareTimer().GetTick()) : -1;
    }
}

KThread* KernelAccess::CreateThread(Core::System& system, KProcess& process,
                                    const ThreadRecord& r) {
    auto& kernel = system.Kernel();
    KScopedResourceReservation reservation(kernel, &process,
                                           Svc::LimitableResource::ThreadCountMax);
    if (!reservation.Succeeded()) {
        throw Error("The thread limit was reached while restoring threads");
    }
    KThread* thread = KThread::Create(kernel);
    if (thread == nullptr) {
        throw Error("Out of kernel memory for threads");
    }
    KThread::s_restore_tls_address = r.tls_address;
    const Result result =
        KThread::InitializeUserThread(system, thread, r.context.pc, r.argument, r.stack_top,
                                      r.base_priority, r.ideal_core, &process);
    KThread::s_restore_tls_address = 0;
    if (result.IsError()) {
        thread->Close(kernel);
        throw Error(fmt::format("Could not create thread {}", r.thread_id));
    }
    reservation.Commit();
    KThread::Register(kernel, thread);

    thread->m_thread_id = r.thread_id;
    kernel.EnsureNextThreadIDAbove(r.thread_id);
    thread->m_thread_context = r.context;
    if (thread->SetCoreMask(kernel, r.ideal_core, r.affinity_mask).IsError()) {
        throw Error(fmt::format("Could not restore the affinity of thread {}", r.thread_id));
    }
    return thread;
}

void KernelAccess::StartThread(Core::System& system, KProcess& process, KThread& thread,
                               const ThreadRecord& r, KThread* lock_owner) {
    auto& kernel = system.Kernel();
    switch (r.hold) {
    case ThreadHold::NotStarted:
        return;
    case ThreadHold::Terminated: {
        KScopedSchedulerLock sl{kernel};
        process.GetResourceLimit()->Release(kernel, LimitableResource::ThreadCountMax, 0, 1);
        thread.m_resource_limit_release_hint = true;
        thread.m_suspend_allowed_flags = 0;
        thread.SetState(kernel, ThreadState::Terminated);
        thread.RegisterDpc(DpcFlag::Terminated);
        thread.m_signaled = true;
        return;
    }
    default:
        break;
    }

    KScopedSchedulerLock sl{kernel};
    if (thread.Run(kernel).IsError()) {
        throw Error(fmt::format("Could not start thread {}", r.thread_id));
    }
    if (r.paused_by_game) {
        thread.RequestSuspend(kernel, SuspendType::Thread);
    }
    if (r.hold == ThreadHold::Parked) {
        return;
    }

    KConditionVariable::ThreadTree* tree = r.hold == ThreadHold::ConditionWait
                                               ? &process.m_cond_var.m_tree
                                               : &process.m_address_arbiter.m_tree;
    auto owned = std::make_unique<RestoredWaitQueue>(kernel, tree);
    KThreadQueue* queue = owned.get();
    RestoredQueues().push_back(std::move(owned));
    if (r.hold == ThreadHold::ConditionWait) {
        thread.SetCurrentSvc(static_cast<u32>(Svc::SvcId::WaitProcessWideKeyAtomic));
        if (lock_owner == nullptr) {
            thread.SetConditionVariable(tree, r.wait_address, r.wait_key, r.wait_value);
            tree->insert(thread);
        } else {
            thread.SetUserAddressKey(r.wait_address, r.wait_value);
            lock_owner->AddWaiter(kernel, &thread);
        }
    } else {
        thread.SetCurrentSvc(static_cast<u32>(Svc::SvcId::WaitForAddress));
        thread.SetAddressArbiter(tree, r.wait_key);
        tree->insert(thread);
    }
    if (r.timeout_ns >= 0) {
        auto& timer = kernel.HardwareTimer();
        timer.RegisterAbsoluteTask(&thread, timer.GetTick() + std::max<s64>(r.timeout_ns, 1));
        queue->SetHardwareTimer(&timer);
    }
    thread.BeginWait(kernel, queue);
}

TransferMemoryRecord KernelAccess::ReadTransferMemory(const KTransferMemory& memory) {
    return {
        .address = GetInteger(memory.m_address),
        .size = memory.GetSize(),
        .permission = static_cast<u32>(memory.m_owner_perm),
    };
}

} // namespace Core::SaveState
