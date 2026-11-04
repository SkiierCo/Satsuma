// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

auto LowerInstruction(const Instruction& instruction) {
    const std::array<Instruction, 2> code{{instruction, {{0x0000794d, 0, 0x03800000, 0}}}};
    return LowerToMaxwell(code);
}

TEST_CASE("Raiders gradient samples preserve the NODEP scheduling flag", "[ampere][raiders]") {
    for (const Instruction captured : {
             Instruction{{0x2c0a7b6c, 0x20800800, 0x041e0f0c, 0x001fc200}},
             Instruction{{0x10087b6c, 0x20800824, 0x041e0f0a, 0x000fc200}}}) {
        auto without_hint = captured;
        without_hint.words[2] &= ~(1U << 26);
        const auto reference = LowerInstruction(without_hint);
        REQUIRE(static_cast<bool>(reference));
        const auto lowered = LowerInstruction(captured);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.required_texture_buffer == reference.required_texture_buffer);
        REQUIRE(lowered.instructions.size() == reference.instructions.size());
        REQUIRE(Shader::Maxwell::Decode(lowered.instructions.front()) == Shader::Maxwell::Opcode::TXD);
        REQUIRE(lowered.instructions.front() == (reference.instructions.front() | (1ULL << 49)));
        REQUIRE((lowered.instructions.front() >> 31 & 15) == 15);
    }
}

TEST_CASE("Raiders unsupported gradient modifiers remain rejected", "[ampere][raiders]") {
    Instruction instruction{{0x2c0a7b6c, 0x20800800, 0x041e1f0c, 0x001fc200}};
    REQUIRE_FALSE(static_cast<bool>(LowerInstruction(instruction)));
}

TEST_CASE("Raiders FRND reads its constant buffer and floors F32", "[ampere][raiders]") {
    const Instruction instruction{{0x00097b07, 0x0242fc00, 0x00205000, 0x000e6400}};
    const auto lowered = LowerInstruction(instruction);
    REQUIRE(static_cast<bool>(lowered));
    const auto operation = lowered.instructions.front();
    REQUIRE(Shader::Maxwell::Decode(operation) == Shader::Maxwell::Opcode::F2F_cbuf);
    REQUIRE((operation & 255) == 9);
    REQUIRE((operation >> 34 & 31) == 9);
    REQUIRE((operation >> 20 & 0x3fff) * 4 == 0xbf0);
    REQUIRE((operation >> 39 & 15) == 9);
    REQUIRE((operation >> 8 & 3) == 2);
    REQUIRE((operation >> 10 & 3) == 2);
}

TEST_CASE("Raiders FRND rejects an unavailable constant buffer", "[ampere][raiders]") {
    Instruction instruction{{0x00097b07, 0x0242fc00, 0x00205000, 0x000e6400}};
    instruction.words[1] = (instruction.words[1] & ~(31U << 22)) | (18U << 22);
    REQUIRE_FALSE(static_cast<bool>(LowerInstruction(instruction)));
}

TEST_CASE("Native 3D TEX keeps contiguous XYZ without DC", "[ampere][texture_layout]") {
    const Instruction instruction{{0x0a087b60, 0x40800014, 0x000e01ff, 0}};
    const auto lowered = LowerInstruction(instruction);
    REQUIRE(static_cast<bool>(lowered));
    for (u32 component = 0; component < 3; ++component) {
        REQUIRE((lowered.instructions[component] & 255) == 250 + component);
        REQUIRE((lowered.instructions[component] >> 20 & 255) == 10 + component);
    }
}

TEST_CASE("Native 3D TEX with DC retains its split coordinate registers",
          "[ampere][texture_layout]") {
    const Instruction instruction{{0x0a087b60, 0x50800014, 0x000e01ff, 0}};
    const auto lowered = LowerInstruction(instruction);
    REQUIRE(static_cast<bool>(lowered));
    constexpr std::array<u32, 3> coordinates{10, 11, 20};
    for (u32 component = 0; component < coordinates.size(); ++component) {
        REQUIRE((lowered.instructions[component] & 255) == 250 + component);
        REQUIRE((lowered.instructions[component] >> 20 & 255) == coordinates[component]);
    }
}

TEST_CASE("Native 3D TEX checks the full contiguous coordinate range",
          "[ampere][texture_layout]") {
    Instruction instruction{{0xf4087b60, 0x40800014, 0x000e01ff, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerInstruction(instruction)));
    instruction.words[1] |= 1U << 28;
    REQUIRE(static_cast<bool>(LowerInstruction(instruction)));
}
} // namespace
