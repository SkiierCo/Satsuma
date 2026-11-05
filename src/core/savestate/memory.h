// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <vector>

#include "common/common_types.h"
#include "core/savestate/stream.h"

namespace Core {
class System;
}

namespace Kernel {
class KProcess;
}

namespace Core::SaveState {

// A user process's address space: layout, memory blocks, virtual-to-physical runs and page
// contents. Physical addresses are only identities here: a restore maps fresh pages and keeps
// aliases (one physical page at two addresses) shared, it never needs the same host pages.
struct ProcessLayout {
    u64 program_id{};
    u64 code_address{};
    u64 code_size{};
    u64 aslr_offset{};
    std::array<u64, 12> regions{}; // heap, alias, stack, kernel map, code, alias code (start, end)
    u64 current_heap_end{};
    u64 max_heap_size{};
    u64 main_thread_stack_size{};
    std::array<u64, 4> entropy{};
    u64 plr_address{};
    bool is_64bit{};
};

struct MemoryBlockRecord {
    u64 address{};
    u64 size{};
    u32 state{};
    u8 permission{};
    u8 attribute{};
    u8 original_permission{};
    u16 ipc_lock_count{};
    u16 device_use_count{};
    auto operator<=>(const MemoryBlockRecord&) const = default;
};

// size pages of virtual memory at address, backed by physical pages starting at physical.
struct PhysicalRun {
    u64 address{};
    u64 physical{};
    u64 pages{};
};

// Thread-local pages: address and which 0x200-byte regions are in use.
struct TlsPageRecord {
    u64 address{};
    u8 used_mask{};
};

struct AddressSpaceState {
    ProcessLayout layout;
    std::vector<MemoryBlockRecord> blocks;
    std::vector<PhysicalRun> runs;
    std::vector<TlsPageRecord> tls_pages;
    // Contents are stored once per physical page, in the order of the first run that maps it;
    // all-zero pages are omitted (bit clear in present).
    std::vector<u64> stored_pages; // physical page addresses, in content order
    std::vector<bool> present;
};

void EncodeAddressSpace(Writer& w, const AddressSpaceState& state);
AddressSpaceState DecodeAddressSpace(Reader& r);

// Capture: reads layout/blocks/runs at a checkpoint and streams page contents in chunks of at
// most chunk_bytes through emit.
AddressSpaceState CaptureAddressSpace(Core::System& system, Kernel::KProcess& process);
void StreamPageContents(Core::System& system, const AddressSpaceState& state, size_t chunk_bytes,
                        const std::function<void(std::span<const u8>)>& emit);

// Restore, in order, into the freshly loaded (not started) process:
//  1. ApplyLayout: region bounds, entropy, main-stack size, before anything is mapped.
//  2. RestoreMappings: heap, physical memory, stacks, aliases, TLS pages, permissions,
//     attributes. Kernel objects that own memory (transfer/code/shared memory) are restored by
//     the kernel component and checked afterwards by VerifyBlocks.
//  3. VerifyBlocks: the block list must equal the saved one.
//  4. RestorePageContents: copies contents through the new virtual-to-physical mapping.
void ApplyLayout(Core::System& system, Kernel::KProcess& process, const ProcessLayout& layout);
void RestoreMappings(Core::System& system, Kernel::KProcess& process,
                     const AddressSpaceState& state);
void VerifyBlocks(Kernel::KProcess& process, const AddressSpaceState& state);
void RestorePageContents(Core::System& system, Kernel::KProcess& process,
                         const AddressSpaceState& state,
                         const std::function<std::vector<u8>()>& next_chunk);

// Reads the current block list (for VerifyBlocks and tests).
std::vector<MemoryBlockRecord> ReadBlocks(Kernel::KProcess& process);

} // namespace Core::SaveState
