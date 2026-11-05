// SPDX-License-Identifier: GPL-3.0-or-later

#include <thread>

#include <fmt/format.h>

#include "core/core.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_lock.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_types.h"
#include "core/savestate/checkpoint.h"

namespace Core::SaveState {

namespace {

using Kernel::KThread;
using Kernel::SuspendType;
using Kernel::ThreadState;
using Kernel::Svc::SvcId;

bool IsRestartableWait(u32 svc) {
    switch (static_cast<SvcId>(svc)) {
    case SvcId::SleepThread:
    case SvcId::WaitSynchronization:
    case SvcId::ArbitrateLock:
        return true;
    default:
        return false;
    }
}

bool IsRunningOnCore(Kernel::KernelCore& kernel, const KThread& thread) {
    for (size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core) {
        if (kernel.Scheduler(core).GetSchedulerCurrentThread() == &thread) {
            return true;
        }
    }
    return false;
}

template <typename F>
void ForEachUserThread(Kernel::KernelCore& kernel, F&& f) {
    for (auto& process : kernel.GetProcessList()) {
        Kernel::KScopedLightLock ll{process->GetListLock()};
        for (auto& thread : process->GetThreadList()) {
            f(*process, thread);
        }
    }
}

} // namespace

const char* SvcName(u32 svc) {
    switch (static_cast<SvcId>(svc)) {
    case SvcId::SleepThread: return "SleepThread";
    case SvcId::WaitSynchronization: return "WaitSynchronization";
    case SvcId::ArbitrateLock: return "ArbitrateLock";
    case SvcId::WaitProcessWideKeyAtomic: return "WaitProcessWideKeyAtomic";
    case SvcId::SendSyncRequestLight: return "SendSyncRequestLight";
    case SvcId::SendSyncRequest: return "SendSyncRequest";
    case SvcId::SendSyncRequestWithUserBuffer: return "SendSyncRequestWithUserBuffer";
    case SvcId::SendAsyncRequestWithUserBuffer: return "SendAsyncRequestWithUserBuffer";
    case SvcId::ReplyAndReceive: return "ReplyAndReceive";
    case SvcId::WaitForAddress: return "WaitForAddress";
    default: return "SVC";
    }
}

Checkpoint::Checkpoint(Core::System& system_) : system{system_} {}

Checkpoint::~Checkpoint() {
    Release();
}

ThreadHold Checkpoint::Classify(const KThread& thread) {
    const ThreadState state = thread.GetState();
    if (state == ThreadState::Terminated) {
        return ThreadHold::Terminated;
    }
    if (state == ThreadState::Initialized) {
        return ThreadHold::NotStarted;
    }
    if (state == ThreadState::Waiting) {
        if (thread.GetCurrentSvc() == static_cast<u32>(SvcId::WaitForAddress)) {
            return ThreadHold::AddressWait;
        }
        return ThreadHold::ConditionWait;
    }
    return ThreadHold::Parked;
}

bool Checkpoint::Poll(std::vector<std::string>* blockers) {
    auto& kernel = system.Kernel();
    bool ready = true;
    ForEachUserThread(kernel, [&](Kernel::KProcess& process, KThread& thread) {
        Kernel::KScopedSchedulerLock sl{kernel};
        const ThreadState state = thread.GetState();
        const u32 svc = thread.GetCurrentSvc();
        const auto block = [&](std::string_view why) {
            ready = false;
            if (blockers != nullptr) {
                blockers->push_back(fmt::format("process {:#x} thread {} {} ({}, svc {:#x})",
                                                process.GetProgramId(), thread.GetThreadId(),
                                                why, SvcName(svc), svc));
            }
        };
        if (state == ThreadState::Terminated || state == ThreadState::Initialized) {
            return;
        }
        if (state == ThreadState::Waiting) {
            if (svc == static_cast<u32>(SvcId::WaitProcessWideKeyAtomic) ||
                svc == static_cast<u32>(SvcId::WaitForAddress)) {
                return; // Recorded and rebuilt; not cancelled.
            }
            if (IsRestartableWait(svc)) {
                if (!thread.IsCheckpointRestart()) {
                    thread.SetCheckpointRestart(true);
                    thread.CancelWait(kernel, Kernel::ResultCancelled, true);
                }
                block("is leaving a restartable wait");
                return;
            }
            block("is waiting");
            return;
        }
        if (IsRunningOnCore(kernel, thread)) {
            block("is running");
            return;
        }
        if (thread.IsSuspendRequested(SuspendType::Checkpoint)) {
            return; // Parked at a gate.
        }
        if (thread.IsSuspended() && svc == KThread::NoSvc) {
            return; // Paused by the game (svcSetThreadActivity) in guest code.
        }
        block("has not reached a stop point");
    });
    return ready;
}

bool Checkpoint::Stop(std::chrono::milliseconds timeout, std::vector<std::string>& blockers) {
    auto& kernel = system.Kernel();
    if (!kernel.IsMulticore()) {
        blockers.push_back("Save states need multicore CPU emulation");
        return false;
    }
    {
        Kernel::KScopedSchedulerLock sl{kernel};
        kernel.SetCheckpointRequested(true);
    }
    requested = true;
    for (size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core) {
        kernel.PhysicalCore(core).Interrupt();
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!Poll(nullptr)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            Poll(&blockers);
            Release();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        // Threads that became runnable since may be in guest code on a core again.
        for (size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core) {
            kernel.PhysicalCore(core).Interrupt();
        }
    }
    held = true;
    return true;
}

void Checkpoint::Release() {
    if (!requested) {
        return;
    }
    auto& kernel = system.Kernel();
    {
        Kernel::KScopedSchedulerLock sl{kernel};
        kernel.SetCheckpointRequested(false);
    }
    ForEachUserThread(kernel, [&](Kernel::KProcess&, KThread& thread) {
        if (thread.IsSuspendRequested(SuspendType::Checkpoint)) {
            thread.SetCheckpointGate(KThread::CheckpointGate::None);
            thread.Resume(kernel, SuspendType::Checkpoint);
        }
    });
    requested = false;
    held = false;
}

} // namespace Core::SaveState
