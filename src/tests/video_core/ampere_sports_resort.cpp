// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};
} // namespace

TEST_CASE("SR_VARIABLE_RATE reads as a 1x1 shading rate", "[video_core][ampere]") {
    // S2UR UR4, SR_VARIABLE_RATE, as captured from Nintendo Switch Sports Resort.
    constexpr Instruction s2ur{{0x000479c3, 0, 0x00008400, 0x000e2200}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(std::array{s2ur, Exit});
    REQUIRE(lowered);
    // MOV32I to UR4's Maxwell register (220) with width 1 and height 1 in bytes 1 and 2.
    CHECK(lowered.instructions.front() == (0x0100000000000000ULL | (7ULL << 16) | 220 |
                                           (15ULL << 12) | (u64{0x00010100} << 20)));

    // The same register to a general register (S2R R5).
    constexpr Instruction s2r{{0x00057919, 0, 0x00008400, 0}};
    const auto general = Shader::Ampere::LowerToMaxwell(std::array{s2r, Exit});
    REQUIRE(general);
    CHECK((general.instructions.front() & 0xff) == 5);
}

TEST_CASE("Other unsupported special registers stay rejected", "[video_core][ampere]") {
    constexpr Instruction s2ur{{0x000479c3, 0, 0x00008500, 0}};
    CHECK_FALSE(Shader::Ampere::LowerToMaxwell(std::array{s2ur, Exit}));
}

TEST_CASE("Nintendo Switch Sports Resort instruction forms lower", "[video_core][ampere]") {
    // Captured from its first scenes; each was rejected before.
    for (const Instruction instruction : {
             // UPRMT UR5, UR4, 0x7772, URZ: the uniform byte permutation, as PRMT.
             Instruction{{0x04057896, 0x00007772, 0x0800003f, 0x002fe200}},
             // TLD.SCR.LL with half results, and the level and offset forms.
             Instruction{{0x08087b66, 0xb0000802, 0x059e0f0a, 0x000f4400}},
             Instruction{{0x2e1a7b66, 0xb0000826, 0x019e0f0e, 0x000fc200}},
             Instruction{{0x02207b66, 0x30001c0b, 0x019e01ff, 0x00114200}},
             // SULD.P.3D.IGN.
             Instruction{{0x04087b97, 0xa0000800, 0x001e0100, 0x004f6400}},
             // ATOMS.POPC.INC.32 at an address offset.
             Instruction{{0x0aff7f8c, 0x00000400, 0x0d00003f, 0x0005e200}},
         }) {
        const auto lowered = Shader::Ampere::LowerToMaxwell(std::array{instruction, Exit});
        INFO("op " << std::hex << (instruction.words[0] & 0xfff) << " " << lowered.error);
        CHECK(lowered);
    }
}

TEST_CASE("Shared increments keep their address offset", "[video_core][ampere]") {
    constexpr Instruction increment{{0x0aff7f8c, 0x00000400, 0x0d00003f, 0x0005e200}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(std::array{increment, Exit});
    REQUIRE(lowered);
    // Maxwell ATOMS.ADD: the word offset (0x4 bytes -> 1) at bit 30.
    const auto atoms = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                    [](u64 insn) { return (insn >> 56) == 0xec; });
    REQUIRE(atoms != lowered.instructions.end());
    CHECK(((*atoms >> 30) & 0x3fffff) == 1);
}
