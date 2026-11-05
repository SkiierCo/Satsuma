// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// Cyberpunk 2077's compute: TLD.LZ through a uniform-register handle (2D, 3D) and through a
// register handle (3D), and a 1D-buffer SUST through a register handle.
constexpr Instruction Uniform2D{{0x04047f66, 0x30000403, 0x0c9e01ff, 0x000fc200}};
constexpr Instruction Uniform3D{{0x20107f66, 0x50000a02, 0x089e0f00, 0x000f4200}};
constexpr Instruction Register3D{{0x08027367, 0x5800000a, 0x049e0f00, 0x004f4400}};
constexpr Instruction BufferStore{{0x0100799c, 0x20000010, 0x00100f00, 0x0041e200}};

struct Move {
    u32 target;
    u32 source;
};

u32 Field(u64 insn, u32 offset, u32 count) {
    return static_cast<u32>((insn >> offset) & ((u64{1} << count) - 1));
}

struct Lowered {
    std::vector<Move> before;
    u64 fetch{};
    std::vector<Move> after;
};

Lowered Split(const Instruction& instruction) {
    const auto lowered = LowerToMaxwell(std::array{instruction, Exit}, false, {}, false, true, true);
    INFO(lowered.error);
    REQUIRE(lowered);
    Lowered out;
    bool seen_fetch = false;
    for (const u64 insn : lowered.instructions) {
        if ((insn >> 52) == 0x5c9) {
            (seen_fetch ? out.after : out.before).push_back({Field(insn, 0, 8), Field(insn, 20, 8)});
        } else if ((insn >> 56) == 0xdd) {
            REQUIRE_FALSE(seen_fetch);
            out.fetch = insn;
            seen_fetch = true;
        }
    }
    REQUIRE(seen_fetch);
    return out;
}

void RequireMoves(const std::vector<Move>& moves, std::initializer_list<Move> expected) {
    REQUIRE(moves.size() == expected.size());
    size_t i = 0;
    for (const Move& move : expected) {
        REQUIRE(moves[i].target == move.target);
        REQUIRE(moves[i].source == move.source);
        ++i;
    }
}

void RequireFetch(u64 fetch, u32 type, u32 mask, u32 bit49) {
    REQUIRE(Field(fetch, 0, 8) == 250);
    REQUIRE(Field(fetch, 8, 8) == 250);
    REQUIRE(Field(fetch, 20, 8) == 246);
    REQUIRE(Field(fetch, 28, 3) == type);
    REQUIRE(Field(fetch, 31, 4) == mask);
    REQUIRE(Field(fetch, 35, 1) == 0); // no offsets
    REQUIRE(Field(fetch, 49, 1) == bit49);
    REQUIRE(Field(fetch, 50, 1) == 0); // not multisampled
    REQUIRE(Field(fetch, 51, 3) == 7);
    REQUIRE(Field(fetch, 55, 1) == 0); // level zero
}
} // namespace

TEST_CASE("Ampere TLD through a uniform handle, 2D", "[video_core][ampere]") {
    const auto lowered = Split(Uniform2D);
    // x in Ra (R4), y in Rb (R3), handle UR4 (R220).
    RequireMoves(lowered.before, {{250, 4}, {251, 3}, {246, 220}});
    RequireFetch(lowered.fetch, 2, 1, 1);
    RequireMoves(lowered.after, {{4, 250}});
}

TEST_CASE("Ampere TLD through a uniform handle, 3D, two destinations", "[video_core][ampere]") {
    const auto lowered = Split(Uniform3D);
    // x, y in Ra (R32, R33), z in Rb (R2), handle UR10 (R226).
    RequireMoves(lowered.before, {{250, 32}, {251, 33}, {252, 2}, {246, 226}});
    RequireFetch(lowered.fetch, 4, 15, 0);
    RequireMoves(lowered.after, {{16, 250}, {17, 251}, {0, 252}, {1, 253}});
}

TEST_CASE("Ampere TLD through a register handle, 3D", "[video_core][ampere]") {
    const auto lowered = Split(Register3D);
    // x, y in Ra (R8, R9); z, then the handle, in Rb (R10, R11).
    RequireMoves(lowered.before, {{250, 8}, {251, 9}, {252, 10}, {246, 11}});
    RequireFetch(lowered.fetch, 4, 15, 1);
    RequireMoves(lowered.after, {{2, 250}, {3, 251}, {0, 252}, {1, 253}});
}

TEST_CASE("Ampere TLD through a handle needs per-draw handles", "[video_core][ampere]") {
    for (const Instruction& instruction : {Uniform2D, Uniform3D, Register3D}) {
        REQUIRE_FALSE(LowerToMaxwell(std::array{instruction, Exit}));
    }
}

TEST_CASE("Ampere TLD through a handle rejects unsupported fields", "[video_core][ampere]") {
    const auto rejected = [](Instruction instruction) {
        return !LowerToMaxwell(std::array{instruction, Exit}, false, {}, false, true, true);
    };
    for (const Instruction& original : {Uniform2D, Uniform3D, Register3D}) {
        auto residency = original;
        residency.words[2] &= ~(1U << 17); // 81-83: residency predicate other than PT
        REQUIRE(rejected(residency));
        auto half = original;
        half.words[2] |= 1U << 15; // .F16
        REQUIRE(rejected(half));
        auto bias = original;
        bias.words[2] = (bias.words[2] & ~(7U << 23)) | (2U << 23); // LOD mode 2
        REQUIRE(rejected(bias));
        for (const u32 bit : {12U, 14U}) { // 76: texel offsets, 78: multisample
            auto unseen = original;
            unseen.words[2] |= 1U << bit;
            REQUIRE(rejected(unseen));
        }
        auto cube = original;
        cube.words[1] = (cube.words[1] & 0x1fffffffU) | (3U << 29);
        REQUIRE(rejected(cube));
    }
    auto register_with_uniform = Register3D;
    register_with_uniform.words[2] |= 1U << 27; // bit 91
    REQUIRE(rejected(register_with_uniform));
    auto uniform_without_flag = Uniform2D;
    uniform_without_flag.words[2] &= ~(1U << 27);
    REQUIRE(rejected(uniform_without_flag));
}

TEST_CASE("Ampere register-handle surface store on a 1D buffer", "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(std::array{BufferStore, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    const u64 store = lowered.instructions.front();
    REQUIRE((store >> 56) == 0xeb);
    REQUIRE(Field(store, 0, 8) == 16);  // data
    REQUIRE(Field(store, 8, 8) == 1);   // coordinate
    REQUIRE(Field(store, 20, 4) == 15); // channels
    REQUIRE(Field(store, 33, 3) == 1);  // 1D buffer
    REQUIRE(Field(store, 39, 8) == 0);  // handle
}

TEST_CASE("Ampere register-handle surface atomic on a 1D buffer", "[video_core][ampere]") {
    // Cyberpunk 2077: @P0 SUATOM.ADD R7, [R6], R7 through the handle in R8.
    constexpr Instruction Atomic{{0x06070394, 0x20000007, 0x001ee008, 0x012ee200}};
    const auto lowered = LowerToMaxwell(std::array{Atomic, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 2);
    const u64 atomic = lowered.instructions.front();
    REQUIRE((atomic >> 56) == 0xea);
    REQUIRE(Field(atomic, 0, 8) == 7);   // destination
    REQUIRE(Field(atomic, 8, 8) == 6);   // coordinate
    REQUIRE(Field(atomic, 16, 4) == 0);  // @P0
    REQUIRE(Field(atomic, 20, 8) == 7);  // data
    REQUIRE(Field(atomic, 29, 4) == 0);  // ADD
    REQUIRE(Field(atomic, 33, 3) == 1);  // 1D buffer
    REQUIRE(Field(atomic, 39, 8) == 8);  // handle register
    REQUIRE(Field(atomic, 51, 3) == 0);  // U32
    REQUIRE(Field(atomic, 54, 1) == 1);  // bindless
    auto unknown = Atomic;
    unknown.words[1] |= 1U << 8; // bits 40-60 are unused in this form
    REQUIRE_FALSE(LowerToMaxwell(std::array{unknown, Exit}));
}
