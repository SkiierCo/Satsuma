// SPDX-License-Identifier: GPL-3.0-or-later

// Cyberpunk 2077 (0400DDB020648000): instruction words captured from Fil's Build 79 run.

#include <algorithm>
#include <array>
#include <span>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction exit_instruction{{0x0000794d, 0, 0x03800000, 0}};
constexpr u64 membar_gl = 0xef98000000000000ULL | (7ULL << 16) | (1ULL << 8);

class MaxwellEnvironment final : public Shader::Environment {
public:
    explicit MaxwellEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Shader::Stage::Compute;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i)
            if (0x50 + i * 8 + ((i + 1) / 3) * 8 == address) return code[i];
        FAIL("Read outside the program at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};
} // Anonymous namespace

TEST_CASE("Cyberpunk ULDC.64 reads root tables 1 and 7 from bank 17", "[video_core][ampere][cyberpunk]") {
    struct Case {
        u32 word1;
        u32 offset;
    };
    // c[0x19][0x0], c[0x19][0x8] and c[0x1f][0x0] into UR4 (R220).
    for (const Case& c : {Case{0x06400000, 0x100}, Case{0x06400200, 0x108}, Case{0x07c00000, 0x700}}) {
        const std::array code{Instruction{{0x00047ab9, c.word1, 0x00000a00, 0x000fe200}},
                              exit_instruction};
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.front() ==
                (0xef90000000000000ULL | (7ULL << 16) | 220ULL | (255ULL << 8) |
                 (u64{c.offset} << 20) | (17ULL << 36) | (5ULL << 48)));
    }
    // Past a table's 256 bytes is not a root table read.
    const std::array outside{Instruction{{0x00047ab9, 0x06400000 | (0x100U << 6), 0x00000a00, 0}},
                             exit_instruction};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(outside)));
}

TEST_CASE("Cyberpunk SURED with a register handle lowers to a bindless SURED",
          "[video_core][ampere][cyberpunk]") {
    // @P1 SURED.1D_BUFFER.ADD.U32 [R10], R9, handle R4.
    const std::array code{Instruction{{0x0a0013a0, 0x20000009, 0x0010e004, 0x0041e200}},
                          exit_instruction};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.front() ==
            (0xeb40000000000000ULL | (1ULL << 16) | 9ULL | (10ULL << 8) | (1ULL << 33) |
             (4ULL << 39)));
    auto bad = code;
    bad[0].words[2] |= 0xf8; // handle R252 is a scratch register
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(bad)));
    bad = code;
    bad[0].words[2] |= 8U << 23; // no EXCH reduction
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(bad)));
}

TEST_CASE("Cyberpunk strong GPU LDG through a uniform base is fenced",
          "[video_core][ampere][cyberpunk]") {
    // LDG.E.STRONG.GPU R7, [R7 + UR4].
    const std::array code{Instruction{{0x07077981, 0x00000004, 0x081ee900, 0x002f2800}},
                          exit_instruction};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() >= 5);
    REQUIRE(lowered.instructions[2] == membar_gl); // after the base + offset adds
    REQUIRE((lowered.instructions[3] >> 52) == 0xeedULL);
    REQUIRE(lowered.instructions[4] == membar_gl);
    // Ordering 6 (bit 78 alone) is not a defined form.
    auto unordered = code;
    unordered[0].words[2] &= ~0x2000U;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(unordered)));
    // The constant form stays unfenced.
    auto constant = code;
    constant[0].words[2] &= ~0x6000U;
    const auto plain = LowerToMaxwell(constant);
    REQUIRE(static_cast<bool>(plain));
    REQUIRE(std::ranges::count(plain.instructions, membar_gl) == 0);
}

TEST_CASE("16-bit and 8-bit global loads left global reach SPIR-V", "[video_core][cyberpunk]") {
    // Maxwell LDG.E.<size> R0, [R2] then STG.E [R4], R0: an untracked pointer stays global.
    for (const u64 size : {0ULL, 1ULL, 2ULL, 3ULL}) {
        const std::array<u64, 3> code{
            0xeed0000000000000ULL | (7ULL << 16) | 0ULL | (2ULL << 8) | (1ULL << 45) | (size << 48),
            0xeed8000000000000ULL | (7ULL << 16) | 0ULL | (4ULL << 8) | (1ULL << 45) | (4ULL << 48),
            0xe30000000007000fULL};
        MaxwellEnvironment env{code};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
        Shader::ObjectPool<Shader::IR::Inst> inst_pool;
        Shader::ObjectPool<Shader::IR::Block> block_pool;
        Shader::HostTranslateInfo host_info{};
        host_info.support_int64 = true;
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info);
        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.support_int64 = true;
        profile.min_ssbo_alignment = 16;
        REQUIRE(Shader::Backend::SPIRV::EmitSPIRV(profile, program).size() > 5);
    }
}
