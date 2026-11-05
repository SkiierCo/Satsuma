// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/savestate/kernel_state.h"
#include "core/savestate/memory.h"
#include "core/savestate/stream.h"

namespace {
using namespace Core::SaveState;

AddressSpaceState TestAddressSpace() {
    AddressSpaceState s;
    s.layout.program_id = 0x0400486024DEC000ULL;
    s.layout.code_address = 0x80004000;
    s.layout.code_size = 0x1234000;
    for (size_t i = 0; i < s.layout.regions.size(); ++i) {
        s.layout.regions[i] = 0x1000000ULL * (i + 1);
    }
    s.layout.current_heap_end = 0x3000000;
    s.layout.max_heap_size = 0x80000000;
    s.layout.main_thread_stack_size = 0x100000;
    s.layout.entropy = {1, 2, 3, 4};
    s.layout.plr_address = 0x7000200;
    s.layout.is_64bit = true;
    s.blocks = {
        {0x80004000, 0x1000000, 0x00DC7E03, 5, 0, 0, 0, 0},
        {0x81004000, 0x234000, 0x04FF7E04, 3, 0, 0, 0, 0},
        {0x1000000, 0x2000000, 0x077EBD05, 3, 0x40, 0, 0, 2},
    };
    s.runs = {{0x80004000, 0x80000000, 0x1000}, {0x1000000, 0x90000000, 0x10}};
    s.tls_pages = {{0x7000000, 0x0B}};
    s.stored_pages = {0x80000000, 0x80001000, 0x90000000};
    s.present = {true, false, true};
    return s;
}

KernelObjectsState TestKernelObjects() {
    KernelObjectsState s;
    s.process_id = 0x51;
    s.process_object = 1;
    s.resource_limit_object = 2;
    s.next_thread_id = 0x99;
    ThreadRecord parked{};
    parked.id = 3;
    parked.thread_id = 0x80;
    parked.is_main = true;
    parked.argument = 0x1234;
    parked.stack_top = 0x5000000;
    parked.base_priority = 44;
    parked.ideal_core = 1;
    parked.affinity_mask = 0x7;
    parked.tls_address = 0x7000200;
    parked.hold = ThreadHold::Parked;
    parked.context.pc = 0x80008000;
    parked.context.r[0] = 0xdead;
    parked.context.v[31] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    parked.context.tpidr = 0x777;
    ThreadRecord waiter = parked;
    waiter.id = 4;
    waiter.thread_id = 0x81;
    waiter.hold = ThreadHold::ConditionWait;
    waiter.wait_key = 0x2000040;
    waiter.wait_address = 0x2000044;
    waiter.wait_value = 0x8badf00d;
    waiter.lock_owner = 3;
    waiter.timeout_ns = 16'666'667;
    s.threads = {parked, waiter};
    s.events = {{5, 6}, {0, 7}};
    s.transfer_memories = {{8, 0x1100000, 0x200000, 0}};
    s.event_states = {{6, true}, {7, false}};
    s.handles.allocated_count = 2;
    s.handles.maximum_count = 2;
    s.handles.next_serial = 3;
    s.handles.free_head = 2;
    s.handles.slots = {{3, 1}, {6, 2}, {0, 0xffff}};
    return s;
}

bool SameThread(const ThreadRecord& a, const ThreadRecord& b) {
    return a.id == b.id && a.thread_id == b.thread_id && a.is_main == b.is_main &&
           a.argument == b.argument && a.stack_top == b.stack_top &&
           a.base_priority == b.base_priority && a.ideal_core == b.ideal_core &&
           a.affinity_mask == b.affinity_mask && a.tls_address == b.tls_address &&
           a.hold == b.hold && a.paused_by_game == b.paused_by_game &&
           a.context.pc == b.context.pc && a.context.r == b.context.r &&
           a.context.v == b.context.v && a.context.tpidr == b.context.tpidr &&
           a.wait_key == b.wait_key && a.wait_address == b.wait_address &&
           a.wait_value == b.wait_value && a.lock_owner == b.lock_owner &&
           a.timeout_ns == b.timeout_ns;
}

} // namespace

TEST_CASE("State stream round trips fields and rejects bad input", "[core][savestate]") {
    Writer w;
    w.Section(Tag("TEST"), 3);
    w.U8(7);
    w.Bool(true);
    w.U16(0xBEEF);
    w.U32(0xDEADBEEF);
    w.U64(0x0123456789ABCDEFULL);
    w.S32(-5);
    w.S64(-6);
    w.String("ocarina");
    w.Blob(std::vector<u8>{1, 2, 3});
    const std::array<u32, 3> pods{10, 20, 30};
    w.PodVector<u32>(pods);

    Reader r{w.data};
    r.Section(Tag("TEST"), 3);
    CHECK(r.U8() == 7);
    CHECK(r.Bool());
    CHECK(r.U16() == 0xBEEF);
    CHECK(r.U32() == 0xDEADBEEF);
    CHECK(r.U64() == 0x0123456789ABCDEFULL);
    CHECK(r.S32() == -5);
    CHECK(r.S64() == -6);
    CHECK(r.String() == "ocarina");
    CHECK(r.Blob() == std::vector<u8>{1, 2, 3});
    CHECK(r.PodVector<u32>() == std::vector<u32>{10, 20, 30});
    CHECK_NOTHROW(r.Finish());

    Reader wrong_tag{w.data};
    CHECK_THROWS_AS(wrong_tag.Section(Tag("NOPE"), 3), Error);
    Reader wrong_version{w.data};
    CHECK_THROWS_AS(wrong_version.Section(Tag("TEST"), 4), Error);
    const std::vector<u8> bad_bool{2};
    Reader bool_reader{bad_bool};
    CHECK_THROWS_AS(bool_reader.Bool(), Error);
    Reader trailing{w.data};
    trailing.Section(Tag("TEST"), 3);
    CHECK_THROWS_AS(trailing.Finish(), Error);
}

TEST_CASE("Address space state round trips and rejects truncation", "[core][savestate]") {
    const auto state = TestAddressSpace();
    Writer w;
    EncodeAddressSpace(w, state);
    Reader r{w.data};
    const auto decoded = DecodeAddressSpace(r);
    r.Finish();
    CHECK(decoded.layout.program_id == state.layout.program_id);
    CHECK(decoded.layout.code_address == state.layout.code_address);
    CHECK(decoded.layout.regions == state.layout.regions);
    CHECK(decoded.layout.entropy == state.layout.entropy);
    CHECK(decoded.layout.plr_address == state.layout.plr_address);
    CHECK(decoded.blocks == state.blocks);
    REQUIRE(decoded.runs.size() == state.runs.size());
    CHECK(decoded.runs[1].physical == state.runs[1].physical);
    CHECK(decoded.tls_pages[0].used_mask == 0x0B);
    CHECK(decoded.stored_pages == state.stored_pages);
    CHECK(decoded.present == state.present);

    for (size_t size = 0; size < w.data.size(); ++size) {
        Reader truncated{std::span<const u8>{w.data}.first(size)};
        CHECK_THROWS_AS(DecodeAddressSpace(truncated), Error);
    }

    auto bad = state;
    bad.runs[0].pages = 0;
    Writer bad_writer;
    EncodeAddressSpace(bad_writer, bad);
    Reader bad_reader{bad_writer.data};
    CHECK_THROWS_AS(DecodeAddressSpace(bad_reader), Error);
}

TEST_CASE("Kernel object state round trips threads, waits and handles", "[core][savestate]") {
    const auto state = TestKernelObjects();
    Writer w;
    EncodeKernelObjects(w, state);
    Reader r{w.data};
    const auto decoded = DecodeKernelObjects(r);
    r.Finish();
    CHECK(decoded.process_id == state.process_id);
    CHECK(decoded.next_thread_id == state.next_thread_id);
    REQUIRE(decoded.threads.size() == 2);
    CHECK(SameThread(decoded.threads[0], state.threads[0]));
    CHECK(SameThread(decoded.threads[1], state.threads[1]));
    REQUIRE(decoded.events.size() == 2);
    CHECK(decoded.events[1].event_id == 0);
    CHECK(decoded.events[1].readable_id == 7);
    CHECK(decoded.transfer_memories[0].size == 0x200000);
    CHECK(decoded.event_states == state.event_states);
    CHECK(decoded.handles == state.handles);

    for (size_t size = 0; size < w.data.size(); size += 7) {
        Reader truncated{std::span<const u8>{w.data}.first(size)};
        CHECK_THROWS_AS(DecodeKernelObjects(truncated), Error);
    }

    // An unknown thread hold must be rejected, not cast into the enum.
    auto bytes = w.data;
    Writer prefix;
    prefix.Section(Tag("KOBJ"), 1);
    const size_t hold_offset = prefix.data.size() + 8 * 5 + 8 + 8 + 1 + 8 + 8 + 4 + 4 + 8 + 8;
    REQUIRE(bytes[hold_offset] == static_cast<u8>(ThreadHold::Parked));
    bytes[hold_offset] = 0xFF;
    Reader bad{bytes};
    CHECK_THROWS_AS(DecodeKernelObjects(bad), Error);
}
