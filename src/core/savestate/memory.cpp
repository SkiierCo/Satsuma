// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include <fmt/format.h>

#include "common/alignment.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_page_buffer.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread_local_page.h"
#include "core/hle/kernel/kernel.h"
#include "core/savestate/kernel_access.h"
#include "core/savestate/memory.h"

namespace Core::SaveState {

namespace {

using Kernel::KMemoryAttribute;
using Kernel::KMemoryInfo;
using Kernel::KMemoryPermission;
using Kernel::KMemoryState;
using Kernel::KProcessAddress;
using Kernel::PageSize;

constexpr u32 AddressSpaceTag = Tag("ASPC");
constexpr u32 AddressSpaceVersion = 1;

std::string Hex(u64 v) {
    return fmt::format("{:#x}", v);
}

void Check(Result result, std::string_view what, u64 address, u64 size) {
    if (result.IsError()) {
        throw Error(fmt::format("{} at {} (size {}) failed with result {:#x}", what, Hex(address),
                                Hex(size), result.raw));
    }
}

bool IsState(const MemoryBlockRecord& b, KMemoryState state) {
    return static_cast<KMemoryState>(b.state) == state;
}

// Merges neighbouring records with identical properties; block managers may split ranges
// differently depending on the order of operations.
std::vector<MemoryBlockRecord> Coalesce(std::vector<MemoryBlockRecord> in) {
    std::vector<MemoryBlockRecord> out;
    for (const auto& b : in) {
        if (!out.empty()) {
            auto& last = out.back();
            if (last.address + last.size == b.address && last.state == b.state &&
                last.permission == b.permission && last.attribute == b.attribute &&
                last.original_permission == b.original_permission &&
                last.ipc_lock_count == b.ipc_lock_count &&
                last.device_use_count == b.device_use_count) {
                last.size += b.size;
                continue;
            }
        }
        out.push_back(b);
    }
    return out;
}

Kernel::KPageTableBase& BaseTable(Kernel::KProcess& process) {
    return process.GetPageTable().GetBasePageTable();
}

} // namespace

std::vector<MemoryBlockRecord> ReadBlocks(Kernel::KProcess& process) {
    auto& table = BaseTable(process);
    std::vector<MemoryBlockRecord> blocks;
    u64 address = GetInteger(table.GetAddressSpaceStart());
    const u64 end = address + table.GetAddressSpaceSize();
    while (address < end) {
        KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        if (table.QueryInfo(&info, &page, address).IsError() || info.m_size == 0) {
            throw Error(fmt::format("Could not read the memory block at {}", Hex(address)));
        }
        blocks.push_back({
            .address = info.m_address,
            .size = info.m_size,
            .state = static_cast<u32>(info.m_state),
            .permission = static_cast<u8>(info.m_permission),
            .attribute = static_cast<u8>(info.m_attribute),
            .original_permission = static_cast<u8>(info.m_original_permission),
            .ipc_lock_count = info.m_ipc_lock_count,
            .device_use_count = info.m_device_use_count,
        });
        address = info.m_address + info.m_size;
    }
    return blocks;
}

void EncodeAddressSpace(Writer& w, const AddressSpaceState& s) {
    w.Section(AddressSpaceTag, AddressSpaceVersion);
    const auto& l = s.layout;
    w.U64(l.program_id);
    w.U64(l.code_address);
    w.U64(l.code_size);
    w.U64(l.aslr_offset);
    for (u64 v : l.regions) {
        w.U64(v);
    }
    w.U64(l.current_heap_end);
    w.U64(l.max_heap_size);
    w.U64(l.main_thread_stack_size);
    for (u64 v : l.entropy) {
        w.U64(v);
    }
    w.U64(l.plr_address);
    w.Bool(l.is_64bit);

    w.U64(s.blocks.size());
    for (const auto& b : s.blocks) {
        w.U64(b.address);
        w.U64(b.size);
        w.U32(b.state);
        w.U8(b.permission);
        w.U8(b.attribute);
        w.U8(b.original_permission);
        w.U16(b.ipc_lock_count);
        w.U16(b.device_use_count);
    }
    w.U64(s.runs.size());
    for (const auto& r : s.runs) {
        w.U64(r.address);
        w.U64(r.physical);
        w.U64(r.pages);
    }
    w.U64(s.tls_pages.size());
    for (const auto& t : s.tls_pages) {
        w.U64(t.address);
        w.U8(t.used_mask);
    }
    w.U64(s.stored_pages.size());
    for (size_t i = 0; i < s.stored_pages.size(); ++i) {
        w.U64(s.stored_pages[i]);
        w.Bool(s.present[i]);
    }
}

AddressSpaceState DecodeAddressSpace(Reader& r) {
    r.Section(AddressSpaceTag, AddressSpaceVersion);
    AddressSpaceState s;
    auto& l = s.layout;
    l.program_id = r.U64();
    l.code_address = r.U64();
    l.code_size = r.U64();
    l.aslr_offset = r.U64();
    for (u64& v : l.regions) {
        v = r.U64();
    }
    l.current_heap_end = r.U64();
    l.max_heap_size = r.U64();
    l.main_thread_stack_size = r.U64();
    for (u64& v : l.entropy) {
        v = r.U64();
    }
    l.plr_address = r.U64();
    l.is_64bit = r.Bool();

    const auto count = [&](size_t min_bytes) {
        const u64 n = r.U64();
        if (n > r.Remaining() / min_bytes) {
            throw Error("Truncated save state");
        }
        return static_cast<size_t>(n);
    };
    s.blocks.resize(count(27));
    for (auto& b : s.blocks) {
        b.address = r.U64();
        b.size = r.U64();
        b.state = r.U32();
        b.permission = r.U8();
        b.attribute = r.U8();
        b.original_permission = r.U8();
        b.ipc_lock_count = r.U16();
        b.device_use_count = r.U16();
    }
    s.runs.resize(count(24));
    for (auto& run : s.runs) {
        run.address = r.U64();
        run.physical = r.U64();
        run.pages = r.U64();
        if (run.pages == 0 || run.address % PageSize != 0 || run.physical % PageSize != 0) {
            throw Error("Invalid memory run in save state");
        }
    }
    s.tls_pages.resize(count(9));
    for (auto& t : s.tls_pages) {
        t.address = r.U64();
        t.used_mask = r.U8();
    }
    const size_t pages = count(9);
    s.stored_pages.resize(pages);
    s.present.resize(pages);
    for (size_t i = 0; i < pages; ++i) {
        s.stored_pages[i] = r.U64();
        s.present[i] = r.Bool();
    }
    return s;
}

AddressSpaceState CaptureAddressSpace(Core::System& system, Kernel::KProcess& process) {
    AddressSpaceState s;
    s.layout = KernelAccess::ReadLayout(process);
    s.blocks = ReadBlocks(process);
    s.tls_pages = KernelAccess::ReadTlsPages(process);

    auto& table = BaseTable(process);
    auto& memory = system.DeviceMemory();
    static const std::array<u8, PageSize> zero{};
    std::unordered_map<u64, size_t> seen;
    for (const auto& b : s.blocks) {
        const auto state = static_cast<KMemoryState>(b.state);
        if (state == KMemoryState::Free || state == KMemoryState::Inaccessible) {
            continue;
        }
        if (state == KMemoryState::IoMemory || state == KMemoryState::IoRegister ||
            state == KMemoryState::Static) {
            throw Error(fmt::format("Memory at {} maps device or static memory (state {:#x}), "
                                    "which save states do not support",
                                    Hex(b.address), b.state));
        }
        for (u64 va = b.address; va < b.address + b.size; va += PageSize) {
            Kernel::KPhysicalAddress pa{};
            if (!table.GetPhysicalAddress(&pa, va)) {
                continue; // Reserved but unbacked (for example unmapped physical memory).
            }
            const u64 phys = GetInteger(pa);
            if (!s.runs.empty()) {
                auto& last = s.runs.back();
                if (last.address + last.pages * PageSize == va &&
                    last.physical + last.pages * PageSize == phys) {
                    ++last.pages;
                } else {
                    s.runs.push_back({va, phys, 1});
                }
            } else {
                s.runs.push_back({va, phys, 1});
            }
            if (seen.emplace(phys, s.stored_pages.size()).second) {
                const u8* host = memory.GetPointer<u8>(pa);
                s.stored_pages.push_back(phys);
                s.present.push_back(std::memcmp(host, zero.data(), PageSize) != 0);
            }
        }
    }
    return s;
}

void StreamPageContents(Core::System& system, const AddressSpaceState& s, size_t chunk_bytes,
                        const std::function<void(std::span<const u8>)>& emit) {
    auto& memory = system.DeviceMemory();
    std::vector<u8> chunk;
    chunk.reserve(chunk_bytes);
    for (size_t i = 0; i < s.stored_pages.size(); ++i) {
        if (!s.present[i]) {
            continue;
        }
        const u8* host = memory.GetPointer<u8>(Kernel::KPhysicalAddress(s.stored_pages[i]));
        chunk.insert(chunk.end(), host, host + PageSize);
        if (chunk.size() + PageSize > chunk_bytes) {
            emit(chunk);
            chunk.clear();
        }
    }
    if (!chunk.empty()) {
        emit(chunk);
    }
}

void ApplyLayout(Core::System& system, Kernel::KProcess& process, const ProcessLayout& layout) {
    const auto current = KernelAccess::ReadLayout(process);
    if (current.program_id != layout.program_id || current.is_64bit != layout.is_64bit) {
        throw Error("The save state belongs to a different program");
    }
    if (current.code_address != layout.code_address || current.code_size != layout.code_size) {
        throw Error(fmt::format("The game's code was loaded at {} (size {}), the state needs {} "
                                "(size {}); its update or mods differ",
                                Hex(current.code_address), Hex(current.code_size),
                                Hex(layout.code_address), Hex(layout.code_size)));
    }
    KernelAccess::WriteLayout(system, process, layout);
}

void RestoreMappings(Core::System& system, Kernel::KProcess& process,
                     const AddressSpaceState& s) {
    auto& pt = process.GetPageTable();
    auto& table = BaseTable(process);
    const auto& l = s.layout;
    const u64 heap_start = l.regions[0];
    const u64 alias_start = l.regions[2];
    const u64 alias_end = l.regions[3];

    // Physical page -> virtual pages that map it.
    std::unordered_map<u64, std::vector<u64>> mappers;
    std::unordered_map<u64, u64> phys_of;
    for (const auto& run : s.runs) {
        for (u64 i = 0; i < run.pages; ++i) {
            mappers[run.physical + i * PageSize].push_back(run.address + i * PageSize);
            phys_of[run.address + i * PageSize] = run.physical + i * PageSize;
        }
    }
    const auto aliased = [&](u64 va) {
        const auto it = phys_of.find(va);
        return it != phys_of.end() && mappers[it->second].size() > 1;
    };

    // The code must already match (ApplyLayout); the fresh process-local region moves to the
    // saved TLS pages below.
    KernelAccess::ReleaseFreshThreadLocalPages(system, process);

    // Heap.
    Check(pt.SetMaxHeapSize(l.max_heap_size), "Restoring the maximum heap size", heap_start,
          l.max_heap_size);
    if (l.current_heap_end > heap_start) {
        KProcessAddress out{};
        Check(pt.SetHeapSize(&out, l.current_heap_end - heap_start), "Restoring the heap",
              heap_start, l.current_heap_end - heap_start);
    }

    // Physical memory mapped into the alias region (svcMapPhysicalMemory), as contiguous ranges.
    for (size_t i = 0; i < s.blocks.size();) {
        const auto& b = s.blocks[i];
        if (!IsState(b, KMemoryState::Normal) || b.address < alias_start ||
            b.address >= alias_end) {
            ++i;
            continue;
        }
        u64 end = b.address + b.size;
        size_t j = i + 1;
        while (j < s.blocks.size() && IsState(s.blocks[j], KMemoryState::Normal) &&
               s.blocks[j].address == end && end < alias_end) {
            end += s.blocks[j].size;
            ++j;
        }
        Check(pt.MapPhysicalMemory(b.address, end - b.address), "Restoring physical memory",
              b.address, end - b.address);
        i = j;
    }

    // Stacks: the main thread stack has its own pages; other stacks alias heap pages
    // (svcMapMemory).
    for (const auto& b : s.blocks) {
        if (!IsState(b, KMemoryState::Stack)) {
            continue;
        }
        for (u64 va = b.address; va < b.address + b.size;) {
            const bool is_alias = aliased(va);
            u64 end = va + PageSize;
            u64 src = 0;
            if (is_alias) {
                for (u64 m : mappers[phys_of[va]]) {
                    if (m != va) {
                        src = m;
                    }
                }
            }
            while (end < b.address + b.size && aliased(end) == is_alias &&
                   (!is_alias || std::ranges::find(mappers[phys_of[end]], src + (end - va)) !=
                                     mappers[phys_of[end]].end())) {
                end += PageSize;
            }
            if (is_alias) {
                Check(pt.MapMemory(va, src, end - va), "Restoring a thread stack", va, end - va);
            } else {
                KernelAccess::MapMainStack(system, process, va, end - va);
            }
            va = end;
        }
    }

    // Code mapped from heap pages (svcMapProcessCodeMemory, used by ldr:ro).
    for (size_t i = 0; i < s.blocks.size();) {
        const auto& b = s.blocks[i];
        if (!IsState(b, KMemoryState::AliasCode) && !IsState(b, KMemoryState::AliasCodeData)) {
            ++i;
            continue;
        }
        u64 end = b.address + b.size;
        size_t j = i + 1;
        while (j < s.blocks.size() && s.blocks[j].address == end &&
               (IsState(s.blocks[j], KMemoryState::AliasCode) ||
                IsState(s.blocks[j], KMemoryState::AliasCodeData))) {
            end += s.blocks[j].size;
            ++j;
        }
        u64 src = 0;
        for (u64 m : mappers[phys_of[b.address]]) {
            if (m != b.address) {
                src = m;
            }
        }
        if (src == 0) {
            throw Error(fmt::format("Code at {} has no source pages", Hex(b.address)));
        }
        Check(pt.MapCodeMemory(b.address, src, end - b.address), "Restoring module code",
              b.address, end - b.address);
        i = j;
    }

    KernelAccess::RestoreThreadLocalPages(system, process, s.tls_pages, l.plr_address);

    // Permissions, then attributes.
    for (const auto& b : s.blocks) {
        const auto state = static_cast<KMemoryState>(b.state);
        const auto perm = static_cast<KMemoryPermission>(b.permission) &
                          KMemoryPermission::UserMask;
        const auto attr = static_cast<KMemoryAttribute>(b.attribute);
        if ((attr & KMemoryAttribute::Locked) != KMemoryAttribute::None) {
            continue; // Set by the operation that locked it.
        }
        KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        Check(table.QueryInfo(&info, &page, b.address), "Reading memory", b.address, b.size);
        const bool covers = info.m_address <= b.address &&
                            b.address + b.size <= info.m_address + info.m_size;
        if (covers && (info.m_permission & KMemoryPermission::UserMask) == perm) {
            continue;
        }
        const auto svc_perm = static_cast<Kernel::Svc::MemoryPermission>(perm);
        if (state == KMemoryState::Normal) {
            Check(pt.SetMemoryPermission(b.address, b.size, svc_perm), "Restoring permissions",
                  b.address, b.size);
        } else if (state == KMemoryState::Code || state == KMemoryState::CodeData ||
                   state == KMemoryState::AliasCode || state == KMemoryState::AliasCodeData) {
            Check(pt.SetProcessMemoryPermission(b.address, b.size, svc_perm),
                  "Restoring code permissions", b.address, b.size);
        }
    }
    for (const auto& b : s.blocks) {
        const auto attr = static_cast<KMemoryAttribute>(b.attribute) & KMemoryAttribute::SetMask;
        if (attr != KMemoryAttribute::None) {
            Check(pt.SetMemoryAttribute(b.address, b.size, attr, attr), "Restoring attributes",
                  b.address, b.size);
        }
    }
}

void VerifyBlocks(Kernel::KProcess& process, const AddressSpaceState& s) {
    const auto want = Coalesce(s.blocks);
    const auto got = Coalesce(ReadBlocks(process));
    for (size_t i = 0; i < std::min(want.size(), got.size()); ++i) {
        if (want[i] != got[i]) {
            const auto& a = want[i];
            const auto& b = got[i];
            throw Error(fmt::format(
                "Restored memory differs at {}: saved size {} state {:#x} perm {:#x} attr {:#x}, "
                "restored size {} state {:#x} perm {:#x} attr {:#x} (address {})",
                Hex(a.address), Hex(a.size), a.state, a.permission, a.attribute, Hex(b.size),
                b.state, b.permission, b.attribute, Hex(b.address)));
        }
    }
    if (want.size() != got.size()) {
        throw Error("Restored memory has a different number of blocks");
    }
}

void RestorePageContents(Core::System& system, Kernel::KProcess& process,
                         const AddressSpaceState& s,
                         const std::function<std::vector<u8>()>& next_chunk) {
    auto& table = BaseTable(process);
    auto& memory = system.DeviceMemory();
    std::unordered_map<u64, u64> first_va;
    for (const auto& run : s.runs) {
        for (u64 i = 0; i < run.pages; ++i) {
            first_va.emplace(run.physical + i * PageSize, run.address + i * PageSize);
        }
    }
    std::vector<u8> chunk;
    size_t offset = 0;
    for (size_t i = 0; i < s.stored_pages.size(); ++i) {
        const auto it = first_va.find(s.stored_pages[i]);
        if (it == first_va.end()) {
            throw Error("Saved memory page is not mapped by any run");
        }
        Kernel::KPhysicalAddress pa{};
        if (!table.GetPhysicalAddress(&pa, it->second)) {
            throw Error(fmt::format("Restored memory at {} is not mapped", Hex(it->second)));
        }
        u8* host = memory.GetPointer<u8>(pa);
        if (!s.present[i]) {
            std::memset(host, 0, PageSize);
            continue;
        }
        if (offset == chunk.size()) {
            chunk = next_chunk();
            offset = 0;
            if (chunk.size() % PageSize != 0 || chunk.empty()) {
                throw Error("Saved memory contents are truncated");
            }
        }
        std::memcpy(host, chunk.data() + offset, PageSize);
        offset += PageSize;
    }
    if (offset != chunk.size()) {
        throw Error("Saved memory contents have trailing data");
    }
}

} // namespace Core::SaveState
