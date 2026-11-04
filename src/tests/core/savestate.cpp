// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "core/savestate/archive.h"
#include "core/savestate/cpu.h"

namespace {
using namespace Core::SaveState;

Identity TestIdentity() {
    Identity identity{.program_id = 0x0400c3f00006e000ULL};
    identity.program_hash.fill(0x12);
    identity.configuration_hash.fill(0x34);
    return identity;
}

std::string TestArchive() {
    std::ostringstream stream{std::ios::binary};
    ArchiveWriter writer{stream, TestIdentity()};
    for (u32 component = 1; component <= 7; ++component) {
        const std::array<u8, 3> data{static_cast<u8>(component), 0x12, 0x34};
        writer.Add(static_cast<Component>(component), data);
    }
    writer.Finish();
    return stream.str();
}

TEST_CASE("State archive round trip checks every machine component", "[core][savestate]") {
    std::istringstream stream{TestArchive(), std::ios::binary};
    ArchiveReader reader{stream, TestIdentity()};
    REQUIRE(reader.Chunks().size() == 7);
    for (const auto& chunk : reader.Chunks()) {
        const std::vector<u8> expected{static_cast<u8>(chunk.component), 0x12, 0x34};
        CHECK(reader.Read(chunk) == expected);
    }
}

TEST_CASE("State archives reject every single-byte corruption before restore",
          "[core][savestate]") {
    const auto valid = TestArchive();
    for (size_t offset = 0; offset < valid.size(); ++offset) {
        auto corrupt = valid;
        corrupt[offset] ^= 0x40;
        std::istringstream stream{corrupt, std::ios::binary};
        CHECK_THROWS_AS(ArchiveReader(stream, TestIdentity()), Error);
    }
}

TEST_CASE("State archives reject truncation and appended garbage", "[core][savestate]") {
    const auto valid = TestArchive();
    for (size_t size = 0; size < valid.size(); ++size) {
        std::istringstream stream{valid.substr(0, size), std::ios::binary};
        CHECK_THROWS_AS(ArchiveReader(stream, TestIdentity()), Error);
    }
    std::istringstream stream{valid + "garbage", std::ios::binary};
    CHECK_THROWS_AS(ArchiveReader(stream, TestIdentity()), Error);
}

TEST_CASE("State identity includes game update and emulation configuration", "[core][savestate]") {
    auto wrong = TestIdentity();
    SECTION("Other title") {
        ++wrong.program_id;
    }
    SECTION("Other update") {
        ++wrong.program_hash[0];
    }
    SECTION("Other configuration") {
        ++wrong.configuration_hash[31];
    }
    std::istringstream stream{TestArchive(), std::ios::binary};
    CHECK_THROWS_AS(ArchiveReader(stream, wrong), Error);
}

TEST_CASE("State writers reject incomplete archives and oversized chunks", "[core][savestate]") {
    std::ostringstream stream{std::ios::binary};
    ArchiveWriter writer{stream, TestIdentity()};
    writer.Add(Component::Cpu, {});
    CHECK_THROWS_AS(writer.Finish(), Error);
    CHECK_THROWS_AS(writer.Add(static_cast<Component>(8), {}), Error);
    std::vector<u8> too_big(MaxChunkBytes + 1);
    CHECK_THROWS_AS(writer.Add(Component::Memory, too_big), Error);
    for (u32 component = 2; component <= 7; ++component) {
        writer.Add(static_cast<Component>(component), {});
    }
    writer.Finish();
    CHECK_THROWS_AS(writer.Finish(), Error);
    CHECK_THROWS_AS(writer.Add(Component::Cpu, {}), Error);
    std::istringstream input{stream.str(), std::ios::binary};
    ArchiveReader reader{input, TestIdentity()};
    for (const auto& chunk : reader.Chunks()) {
        CHECK(reader.Read(chunk).empty());
    }
}

TEST_CASE("Chunked state archives handle incompressible data without whole-RAM copies",
          "[core][savestate]") {
    std::mt19937 random{87134};
    std::vector<u8> memory(1024 * 1024);
    for (auto& value : memory) {
        value = static_cast<u8>(random());
    }
    std::ostringstream stream{std::ios::binary};
    ArchiveWriter writer{stream, TestIdentity()};
    for (u32 component = 1; component <= 7; ++component) {
        writer.Add(static_cast<Component>(component), {});
    }
    writer.Add(Component::Memory, memory);
    writer.Add(Component::Memory, memory);
    writer.Finish();
    std::istringstream input{stream.str(), std::ios::binary};
    ArchiveReader reader{input, TestIdentity()};
    REQUIRE(reader.Chunks().size() == 9);
    for (size_t index = 7; index < 9; ++index) {
        CHECK(reader.Chunks()[index].index == index - 6);
        CHECK(reader.Read(reader.Chunks()[index]) == memory);
    }
}

TEST_CASE("State rereads detect file changes after validation", "[core][savestate]") {
    std::stringstream stream{TestArchive(), std::ios::in | std::ios::out | std::ios::binary};
    ArchiveReader reader{stream, TestIdentity()};
    const auto chunk = reader.Chunks().front();
    stream.seekp(chunk.offset);
    stream.put('X');
    CHECK_THROWS_AS(reader.Read(chunk), Error);
}

TEST_CASE("CPU state preserves architectural register bits without serializing ABI padding",
          "[core][savestate]") {
    Core::SaveState::ThreadRegisters original;
    std::mt19937_64 random{946713};
    for (const auto architecture : {Core::Architecture::AArch64, Core::Architecture::AArch32}) {
        original.architecture = architecture;
        original.process_id = random();
        original.thread_id = random();
        original.tpidrro = random();
        auto& context = original.context;
        for (auto& value : context.r) {
            value = random();
        }
        context.fp = random();
        context.lr = random();
        context.sp = random();
        context.pc = random();
        context.pstate = static_cast<u32>(random());
        context.padding = 0xdeadbeef;
        for (auto& value : context.v) {
            value = {random(), random()};
        }
        context.fpcr = static_cast<u32>(random());
        context.fpsr = static_cast<u32>(random());
        context.tpidr = random();
        const auto encoded = EncodeThreadRegisters(original);
        const auto decoded = DecodeThreadRegisters(encoded);
        CHECK(decoded.architecture == architecture);
        CHECK(decoded.process_id == original.process_id);
        CHECK(decoded.thread_id == original.thread_id);
        CHECK(decoded.tpidrro == original.tpidrro);
        CHECK(decoded.context.padding == 0);
        CHECK(EncodeThreadRegisters(decoded) == encoded);
        const auto& result = decoded.context;
        CHECK(result.r == context.r);
        CHECK(result.fp == context.fp);
        CHECK(result.lr == context.lr);
        CHECK(result.sp == context.sp);
        CHECK(result.pc == context.pc);
        CHECK(result.pstate == context.pstate);
        CHECK(result.v == context.v);
        CHECK(result.fpcr == context.fpcr);
        CHECK(result.fpsr == context.fpsr);
        CHECK(result.tpidr == context.tpidr);
        for (size_t size = 0; size < encoded.size(); ++size) {
            CHECK_THROWS_AS(DecodeThreadRegisters(std::span{encoded}.first(size)), Error);
        }
        auto invalid = encoded;
        invalid[0] = 99;
        CHECK_THROWS_AS(DecodeThreadRegisters(invalid), Error);
        invalid = encoded;
        invalid[4] = 99;
        CHECK_THROWS_AS(DecodeThreadRegisters(invalid), Error);
        invalid = encoded;
        invalid.push_back(0);
        CHECK_THROWS_AS(DecodeThreadRegisters(invalid), Error);
    }
}
} // namespace
