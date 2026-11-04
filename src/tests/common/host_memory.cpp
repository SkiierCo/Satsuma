// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/host_memory.h"
#include "common/literals.h"
#include "common/settings.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

#ifdef _WIN32
TEST_CASE("HostMemory: Enforce the 12 GiB resident RAM ceiling", "[common][backing]") {
    SIZE_T old_minimum{}, old_maximum{};
    DWORD old_flags{};
    REQUIRE(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &old_minimum, &old_maximum, &old_flags));
    REQUIRE(Common::ApplyHostRamLimit());
    SIZE_T minimum{}, maximum{};
    DWORD flags{};
    REQUIRE(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minimum, &maximum, &flags));
    REQUIRE(maximum <= Common::HostRamLimitBytes);
    REQUIRE((flags & QUOTA_LIMITS_HARDWS_MAX_ENABLE) != 0);
    REQUIRE(SetProcessWorkingSetSizeEx(GetCurrentProcess(), old_minimum, old_maximum, old_flags));
}

TEST_CASE("HostMemory: Only the 12 GiB layout can be loaded", "[common][backing]") {
    Settings::Linkage linkage;
    Settings::FixedMemoryLayoutSetting setting{linkage};
    REQUIRE(setting.GetDefault() == Settings::MemoryLayout::Memory_12Gb);
    for (const char* value : {"0", "1", "2", "3", "4", "99"}) {
        setting.LoadString(value);
        REQUIRE(setting.GetValue() == Settings::MemoryLayout::Memory_12Gb);
    }
    setting.SetValue(Settings::MemoryLayout::Memory_4Gb);
    REQUIRE(setting.GetValue() == Settings::MemoryLayout::Memory_12Gb);
}

static MEMORY_BASIC_INFORMATION QueryPage(const void* address) {
    MEMORY_BASIC_INFORMATION region{};
    REQUIRE(VirtualQuery(address, &region, sizeof(region)) == sizeof(region));
    return region;
}

static bool TryProtectedWrite(volatile u8* address) {
    __try {
        *address = 42;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

TEST_CASE("HostMemory: Reserve 20 GiB without committing untouched pages", "[common][backing]") {
    HostMemory mem(20_GiB, VIRTUAL_SIZE, HostMemory::BackingMode::Deferred);
    auto* const base = mem.BackingBasePointer();
    REQUIRE(base != nullptr);
    REQUIRE(QueryPage(base).Type == MEM_MAPPED);
    REQUIRE(QueryPage(base).State == MEM_RESERVE);
    REQUIRE(QueryPage(base + 20_GiB - 0x1000).State == MEM_RESERVE);
    mem.ClearBackingRegion(0, 0x1000, 0);
    REQUIRE(QueryPage(base).State == MEM_COMMIT);
    REQUIRE(base[0] == 0);
    REQUIRE(QueryPage(base + 20_GiB - 0x1000).State == MEM_RESERVE);
}

TEST_CASE("HostMemory: Deferred commits share mirrors and preserve unused backing", "[common][backing]") {
    HostMemory mem(32_MiB, VIRTUAL_SIZE, HostMemory::BackingMode::Deferred);
    auto* const base = mem.BackingBasePointer();
    mem.Map(4_MiB, 8_MiB, 0x10000, PERMS, HEAP);
    volatile u8* const mirror = mem.VirtualBasePointer() + 4_MiB;
    mem.MarkUsed(8_MiB, 0x10000);
    REQUIRE(mirror[0x1234] == 0);
    mirror[0x1234] = 73;
    REQUIRE(base[8_MiB + 0x1234] == 73);
    base[8_MiB + 0x4321] = 91;
    REQUIRE(mirror[0x4321] == 91);
    REQUIRE(QueryPage(base + 24_MiB).State == MEM_RESERVE);
    mem.MarkDirty(8_MiB, 0x10000);
    mem.ClearBackingRegion(8_MiB, 0x10000, 0);
    REQUIRE(mirror[0x1234] == 0);
    REQUIRE(mirror[0x4321] == 0);
    REQUIRE(QueryPage(base + 24_MiB).State == MEM_RESERVE);
    mem.ClearBackingRegion(24_MiB, 0x1000, 0xAB);
    REQUIRE(base[24_MiB] == 0xAB);
    REQUIRE(base[24_MiB + 0xfff] == 0xAB);
    REQUIRE(QueryPage(base + 24_MiB).State == MEM_COMMIT);
}

TEST_CASE("HostMemory: Lazy backing preserves tracked read-only pages", "[common][backing]") {
    HostMemory mem(8_MiB, VIRTUAL_SIZE, HostMemory::BackingMode::Deferred);
    mem.MarkUsed(0, 0x10000);
    mem.Map(0x10000, 0, 0x10000, PERMS, HEAP);
    volatile u8* const mirror = mem.VirtualBasePointer() + 0x10000;
    mirror[0] = 17;
    mem.Protect(0x10000, 0x1000, Common::MemoryPermission::Read);
    REQUIRE_FALSE(TryProtectedWrite(mirror));
    REQUIRE(mirror[0] == 17);
    mem.Protect(0x10000, 0x1000, PERMS);
    REQUIRE(TryProtectedWrite(mirror));
    REQUIRE(mirror[0] == 42);
}

TEST_CASE("HostMemory: Protect explicitly committed deferred pages", "[common][backing]") {
    HostMemory mem(32_MiB, VIRTUAL_SIZE, HostMemory::BackingMode::Deferred);
    mem.Map(4_MiB, 8_MiB, 4_MiB, PERMS, HEAP);
    auto* const mirror = mem.VirtualBasePointer() + 4_MiB;
    REQUIRE(QueryPage(mirror).State == MEM_RESERVE);
    mem.MarkUsed(8_MiB, 0x1000);
    mem.Protect(4_MiB, 0x1000, Common::MemoryPermission::Read);
    REQUIRE(QueryPage(mirror).State == MEM_COMMIT);
    REQUIRE(QueryPage(mirror).Protect == PAGE_READONLY);
    REQUIRE_FALSE(TryProtectedWrite(mirror));
    REQUIRE(mirror[0] == 0);
    REQUIRE(QueryPage(mem.BackingBasePointer() + 24_MiB).State == MEM_RESERVE);

    // Commit the remaining mapped range before applying its page protections.
    mem.MarkUsed(8_MiB, 4_MiB);
    volatile u8* const middle = mirror + 2_MiB;
    middle[0] = 19;
    mem.Protect(4_MiB, 4_MiB, Common::MemoryPermission::Read);
    REQUIRE(QueryPage(mirror + 0x1000).Protect == PAGE_READONLY);
    REQUIRE_FALSE(TryProtectedWrite(mirror + 0x1000));
    REQUIRE_FALSE(TryProtectedWrite(middle));
    REQUIRE(middle[0] == 19);
    REQUIRE(QueryPage(mem.BackingBasePointer() + 24_MiB).State == MEM_RESERVE);
    mem.Protect(4_MiB, 4_MiB, PERMS);
    REQUIRE(TryProtectedWrite(mirror));
    REQUIRE(TryProtectedWrite(middle));
}

TEST_CASE("HostMemory: Multiple deferred arenas retain independent lifetimes", "[common][backing]") {
    HostMemory first(8_MiB, VIRTUAL_SIZE, HostMemory::BackingMode::Deferred);
    first.MarkUsed(0, 0x10000);
    first.BackingBasePointer()[0] = 37;
    {
        HostMemory second(8_MiB, VIRTUAL_SIZE, HostMemory::BackingMode::Deferred);
        second.MarkUsed(0, 0x10000);
        second.Map(0x10000, 0, 0x10000, PERMS, HEAP);
        second.VirtualBasePointer()[0x10000] = 91;
        REQUIRE(first.BackingBasePointer()[0] == 37);
        REQUIRE(second.BackingBasePointer()[0] == 91);
        REQUIRE(QueryPage(first.BackingBasePointer() + 4_MiB).State == MEM_RESERVE);
    }
    first.Map(0x10000, 0, 0x10000, PERMS, HEAP);
    REQUIRE(first.VirtualBasePointer()[0x10000] == 37);
    first.MarkUsed(4_MiB, 0x10000);
    first.BackingBasePointer()[4_MiB] = 73;
    REQUIRE(first.BackingBasePointer()[4_MiB] == 73);
}
#endif

TEST_CASE("HostMemory: Initialize and deinitialize", "[common]") {
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
}

TEST_CASE("HostMemory: Simple map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[75] = 50;
    REQUIRE(data[75] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}
