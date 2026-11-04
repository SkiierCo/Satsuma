// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

// Instructions from Yoshi and the Mysterious Book's compute shaders (layouts from nxnative's
// notes and decoder, MIT).
namespace {
using Shader::Ampere::Instruction;

constexpr Instruction Filler{{0x09047220, 4, 0x0041c000, 0}};
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

Shader::Ampere::LoweredProgram Lower(const Instruction& instruction) {
    const std::array<Instruction, 3> code{{Filler, instruction, Exit}};
    // As the emulator lowers Switch 2 programs: Vulkan, tensor emulation, per-draw handles.
    return Shader::Ampere::LowerToMaxwell(code, false, {}, true, true, true);
}

bool Emits(const Shader::Ampere::LoweredProgram& lowered, u64 mask, u64 value) {
    return std::ranges::any_of(lowered.instructions,
                               [&](u64 word) { return (word & mask) == value; });
}
} // namespace

TEST_CASE("Ampere SUATOM lowers to Maxwell SUATOM", "[video_core][ampere][yoshi]") {
    // SUATOM.ADD.U32 on a 1D buffer through c[3][0]: R6 = atomic(R9, R6).
    const auto buffer = Lower({{0x09060593, 0x20c00006, 0x001ee000, 0x004ea200}});
    REQUIRE(static_cast<bool>(buffer));
    constexpr u64 opcode_mask = 0x1ffULL << 55;
    const auto suatom = std::ranges::find_if(buffer.instructions, [](u64 word) {
        return (word & opcode_mask) == (0x1d4ULL << 55);
    });
    REQUIRE(suatom != buffer.instructions.end());
    CHECK((*suatom & 0xff) == 6);           // destination
    CHECK(((*suatom >> 8) & 0xff) == 9);    // coordinate
    CHECK(((*suatom >> 20) & 0xff) == 6);   // operand
    CHECK(((*suatom >> 29) & 0xf) == 0);    // ADD
    CHECK(((*suatom >> 33) & 7) == 1);      // 1D buffer
    CHECK(((*suatom >> 51) & 7) == 0);      // U32
    // A 2D image whose handle is at c[3][0x20].
    CHECK(static_cast<bool>(Lower({{0x020c7593, 0x60c0080c, 0x001ee000, 0x00052200}})));
    // A predicate result and sizes beyond S32 are refused.
    CHECK_FALSE(static_cast<bool>(Lower({{0x09060593, 0x20c00006, 0x001ce000, 0x004ea200}})));
    CHECK_FALSE(static_cast<bool>(Lower({{0x09060593, 0x20c00006, 0x001ee400, 0x004ea200}})));
}

TEST_CASE("Ampere SULD reads 1D buffers", "[video_core][ampere][yoshi]") {
    // SULD.P.R from a 1D buffer through c[3][0] into R1.
    const auto lowered = Lower({{0x00017b97, 0x20c00000, 0x001e0100, 0x000f6200}});
    REQUIRE(static_cast<bool>(lowered));
    constexpr u64 mask = (0x1ffULL << 55) | (7ULL << 33);
    CHECK(Emits(lowered, mask, (0x1d6ULL << 55) | (1ULL << 33)));
}

TEST_CASE("Ampere funnel shifts lower in their clamped and high forms",
          "[video_core][ampere][yoshi]") {
    // SHF.L.U32.HI R11, RZ, R11, 0xffffffff without .W: SHL without wrap.
    const auto clamped = Lower({{0xff0b7419, 0xffffffff, 0x0001060b, 0x000fe400}});
    REQUIRE(static_cast<bool>(clamped));
    constexpr u64 shl = 0xfff8ULL << 48;
    const auto shift = std::ranges::find_if(clamped.instructions, [](u64 word) {
        return (word & shl) == (0x5c48ULL << 48);
    });
    REQUIRE(shift != clamped.instructions.end());
    CHECK(((*shift >> 39) & 1) == 0);
    // SHF.L.W.U32.HI R7, R7, c[..], R4: Maxwell SHF.L with low in Ra and high in Rc.
    const auto high = Lower({{0xff077a19, 0x01000300, 0x00010e04, 0x000fe200}});
    REQUIRE(static_cast<bool>(high));
    const auto funnel = std::ranges::find_if(high.instructions, [](u64 word) {
        return (word & shl) == (0x5bf8ULL << 48);
    });
    REQUIRE(funnel != high.instructions.end());
    CHECK(((*funnel >> 39) & 0xff) == 4);
    CHECK(((*funnel >> 50) & 1) == 1);
}

TEST_CASE("Ampere TLD4 with a register handle lowers to TLD4.B", "[video_core][ampere][yoshi]") {
    // TLD4 R16.RGBA (second half R18), x, y in R2, R3, the handle in R16.
    const auto lowered = Lower({{0x02101364, 0x38000010, 0x001e0f12, 0x00014200}});
    REQUIRE(static_cast<bool>(lowered));
    CHECK(Emits(lowered, 0xffcULL << 52, 0xdecULL << 52));
    // Without per-draw handle evaluation it is refused, as the uniform-handle form.
    const std::array<Instruction, 3> code{{Filler, {{0x02101364, 0x38000010, 0x001e0f12, 0x00014200}}, Exit}};
    CHECK_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(code, false, {}, true, true, false)));
}
