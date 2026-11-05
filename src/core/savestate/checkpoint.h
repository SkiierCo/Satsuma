// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class KThread;
}

namespace Core::SaveState {

// How a user thread is held at a checkpoint.
enum class ThreadHold : u8 {
    Parked,          // At an SVC or guest-instruction boundary; its registers are its whole state.
    ConditionWait,   // Inside WaitProcessWideKeyAtomic (cv tree or waiting for the mutex).
    AddressWait,     // Inside WaitForAddress (address arbiter tree).
    NotStarted,      // Created, not started.
    Terminated,
};

// Stops every user thread at a point a state can describe:
//  - new SVCs are not started (the svc instruction runs again afterwards);
//  - finished SVCs return their results first;
//  - running guest code stops at an instruction boundary;
//  - WaitSynchronization, SleepThread and ArbitrateLock waits are cancelled and run again;
//  - condition-variable and address-arbiter waits stay blocked and are recorded.
// IPC requests to HLE services complete first. Requires multicore CPU emulation.
class Checkpoint {
public:
    explicit Checkpoint(Core::System& system);
    ~Checkpoint();

    Checkpoint(const Checkpoint&) = delete;
    Checkpoint& operator=(const Checkpoint&) = delete;

    // Returns false, after releasing every thread, if the threads could not all be held within
    // the timeout; blockers then name each thread and the SVC it is in.
    bool Stop(std::chrono::milliseconds timeout, std::vector<std::string>& blockers);

    // Resumes every thread. Safe to call more than once.
    void Release();

    bool IsHeld() const {
        return held;
    }

    // Valid while held.
    static ThreadHold Classify(const Kernel::KThread& thread);

private:
    bool Poll(std::vector<std::string>* blockers);

    Core::System& system;
    bool requested{};
    bool held{};
};

const char* SvcName(u32 svc);

} // namespace Core::SaveState
