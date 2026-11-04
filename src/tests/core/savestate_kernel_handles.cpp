// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/savestate/kernel_handles.h"

namespace {
using namespace Core::SaveState;

HandleTableState ExampleTable() {
    // Two handles alias object 7, slot 1 is reserved for an unfinished IPC transfer,
    // and free slots allocate in order 4, 2. Preserve serial wrap and high-water count.
    return {3, 5, 0x7fff, 4, {{7, 0x3456}, {0, 0xffff}, {0, 0xffff}, {7, 42}, {0, 2}}};
}

TEST_CASE("Kernel handle state preserves aliases, reservations, serials and free-list order",
          "[core][savestate][kernel_handles]") {
    const auto original = ExampleTable();
    CHECK(DecodeHandleTable(EncodeHandleTable(original)) == original);
}

TEST_CASE("Kernel handle state rejects malformed allocation graphs",
          "[core][savestate][kernel_handles]") {
    auto state = ExampleTable();
    SECTION("Free-list cycle") {
        state.slots[2].entry_info = 4;
    }
    SECTION("Free-list pointer outside table") {
        state.slots[4].entry_info = 1024;
    }
    SECTION("Invalid negative free-list head") {
        state.free_head = -2;
    }
    SECTION("Object on free list") {
        state.slots[2].object_id = 9;
    }
    SECTION("Wrong allocation count") {
        ++state.allocated_count;
    }
    SECTION("Invalid serial") {
        state.slots[0].entry_info = 0;
    }
    SECTION("Wide serial") {
        state.slots[0].entry_info = 0x8000;
    }
    SECTION("Invalid next serial") {
        state.next_serial = 0;
    }
    SECTION("Invalid high-water count") {
        state.maximum_count = 2;
    }
    SECTION("Oversized table") {
        state.slots.resize(1025);
    }
    CHECK_THROWS_AS(EncodeHandleTable(state), Error);
}

TEST_CASE("Kernel handle decode rejects every truncation before constructing a table",
          "[core][savestate][kernel_handles]") {
    const auto saved = EncodeHandleTable(ExampleTable());
    for (size_t size = 0; size < saved.size(); ++size) {
        CHECK_THROWS_AS(DecodeHandleTable(std::span{saved}.first(size)), Error);
    }
}

TEST_CASE("Kernel handle reconstruction resolves aliases once and restores reference ownership",
          "[core][savestate][kernel_handles]") {
    Core::System system;
    auto& kernel = system.Kernel();
    Kernel::KAutoObject object{kernel};
    Kernel::KAutoObject::Create(&object);
    Kernel::KHandleTable table{kernel};
    u32 resolutions{};
    const auto state = DecodeHandleTable(EncodeHandleTable(ExampleTable()));
    HandleTableAccess::RestoreFresh(kernel, table, state, [&](u64 id) {
        CHECK(id == 7);
        ++resolutions;
        return &object;
    });
    CHECK(resolutions == 1);
    CHECK(table.GetCount() == 3);
    CHECK(table.GetMaxCount() == 5);
    CHECK(object.GetReferenceCount() == 3);
    CHECK(HandleTableAccess::Capture(table, [&](auto pointer) {
              CHECK(pointer == &object);
              return 7;
          }) == state);
    CHECK_THROWS_AS(
        HandleTableAccess::RestoreFresh(kernel, table, state, [&](u64) { return &object; }), Error);
    CHECK(object.GetReferenceCount() == 3);
    // No scheduler is initialized in this codec fixture. Release the two table references
    // explicitly, then unregister the fixture-owned stack object without invoking Destroy.
    object.Close(kernel);
    object.Close(kernel);
    kernel.UnregisterKernelObject(&object);
}

TEST_CASE("Missing kernel objects leave a candidate handle table and reference counts unchanged",
          "[core][savestate][kernel_handles]") {
    Core::System system;
    auto& kernel = system.Kernel();
    Kernel::KAutoObject object{kernel};
    Kernel::KAutoObject::Create(&object);
    Kernel::KHandleTable table{kernel};
    auto state = ExampleTable();
    state.slots[3].object_id = 8;
    CHECK_THROWS_AS(HandleTableAccess::RestoreFresh(
                        kernel, table, state, [&](u64 id) { return id == 7 ? &object : nullptr; }),
                    Error);
    CHECK(table.GetCount() == 0);
    CHECK(table.GetTableSize() == 0);
    CHECK(object.GetReferenceCount() == 1);
    kernel.UnregisterKernelObject(&object);
}

TEST_CASE("Inactive kernel objects roll back references acquired during candidate reconstruction",
          "[core][savestate][kernel_handles]") {
    Core::System system;
    auto& kernel = system.Kernel();
    Kernel::KAutoObject live{kernel};
    Kernel::KAutoObject inactive{kernel};
    Kernel::KAutoObject::Create(&live);
    Kernel::KHandleTable table{kernel};
    auto state = ExampleTable();
    state.slots[3].object_id = 8;
    CHECK_THROWS_AS(HandleTableAccess::RestoreFresh(
                        kernel, table, state, [&](u64 id) { return id == 7 ? &live : &inactive; }),
                    Error);
    CHECK(table.GetCount() == 0);
    CHECK(table.GetTableSize() == 0);
    CHECK(live.GetReferenceCount() == 1);
    CHECK(inactive.GetReferenceCount() == 0);
    kernel.UnregisterKernelObject(&live);
    kernel.UnregisterKernelObject(&inactive);
}
} // namespace
