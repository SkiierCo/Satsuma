// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using Shader::Ampere::Instruction;
constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

// ISBEWR.O [Ra + offset], Rdata; region 3 is .ATTR, 0 the map region.
Instruction Isbewr(u32 ra, u32 offset, u32 data, bool is_32, u32 region) {
    return {{0x7927U | (ra << 24), data | (offset << 8),
             (u32{is_32} << 11) | (region << 12) | (1U << 15), 0}};
}

// ISBERD.ATTR Rd, [Ra + offset] from the input region.
Instruction Isberd(u32 dest, u32 ra, u32 offset) {
    return {{0x7923U | (dest << 16) | (ra << 24), offset << 8, (1U << 11) | (3U << 12), 0}};
}

class MeshEnvironment final : public Shader::Environment {
public:
    MeshEnvironment(Shader::Stage stage_, std::span<const u64> code_, std::array<u32, 3> registers)
        : code{code_} {
        stage = stage_;
        mesh_registers = registers;
        // Output map: position x, y, z, w (system values b bits 4-7) for the mesh program.
        sph.vtg.omap_systemb.raw = 0xf0;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i)
            if (Address(i) == address)
                return code[i];
        FAIL("Frontend read outside lowered program at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        FAIL("Unexpected cbuf read");
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {32, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    bool HasNativeInstructions() const override {
        return true;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

std::vector<u64> Lower(std::span<const Instruction> program) {
    const auto lowered = Shader::Ampere::LowerToMaxwell(program);
    REQUIRE(lowered);
    return lowered.instructions;
}

std::vector<u32> Emit(Shader::Stage stage, std::span<const u64> code,
                      std::array<u32, 3> registers) {
    MeshEnvironment env{stage, code, registers};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                                     Shader::HostTranslateInfo{});
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010400;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}

// SPIR-V opcodes and execution models (SPV_EXT_mesh_shader), from the specification.
constexpr u32 OpEntryPoint = 15;
constexpr u32 OpReturn = 253;
constexpr u32 OpEmitMeshTasksEXT = 5294;
constexpr u32 OpSetMeshOutputsEXT = 5295;
constexpr u32 ExecutionModelTaskEXT = 5364;
constexpr u32 ExecutionModelMeshEXT = 5365;

// Counts the instructions with this opcode in a SPIR-V module.
size_t CountOp(std::span<const u32> code, u32 op) {
    size_t count{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0) {
            break;
        }
        count += (code[offset] & 0xffff) == op;
        offset += word_count;
    }
    return count;
}

bool HasExecutionModel(std::span<const u32> code, u32 model) {
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0) {
            break;
        }
        if ((code[offset] & 0xffff) == OpEntryPoint && code[offset + 1] == model) {
            return true;
        }
        offset += word_count;
    }
    return false;
}

// SET_MESH_SHADER_A: triangles, 3 vertices, 1 primitive. SET_MESH_SHADER_B: 32 mesh threads.
// SET_MESH_INIT_SHADER: 32 task threads, one 128-byte line handed to the mesh stage.
constexpr std::array<u32, 3> MeshRegisters{4U | (3U << 4) | (1U << 13), 32U << 20,
                                           32U | (1U << 22)};
} // namespace

TEST_CASE("Task and mesh stage buffer accesses lower to the nx2 pseudo instruction",
          "[ampere][mesh]") {
    const auto write = Lower(std::array{Isbewr(2, 0x10, 5, true, 3), Exit});
    REQUIRE(write.size() >= 2);
    const u64 isbe{write.front()};
    CHECK((isbe >> 52) == 0xefc);
    CHECK((isbe & 0xff) == 5);           // data register
    CHECK(((isbe >> 8) & 0xff) == 2);    // address register
    CHECK(((isbe >> 20) & 0xffffff) == 0x10);
    CHECK(((isbe >> 44) & 1) == 1);      // .32
    CHECK(((isbe >> 45) & 1) == 1);      // .ATTR
    CHECK(((isbe >> 47) & 1) == 1);      // .O
    CHECK(((isbe >> 48) & 1) == 1);      // write

    const auto read = Lower(std::array{Isberd(7, 255, 0x10), Exit});
    CHECK((read.front() >> 52) == 0xefc);
    CHECK((read.front() & 0xff) == 7);
    CHECK(((read.front() >> 48) & 1) == 0);

    // A store to the input region, or a read from the map region, is not a valid access.
    Instruction bad_write{Isbewr(2, 0, 5, true, 3)};
    bad_write.words[2] &= ~(1U << 15);
    CHECK_FALSE(Shader::Ampere::LowerToMaxwell(std::array{bad_write, Exit}));
}

TEST_CASE("Geometry programs keep the Maxwell stage buffer read", "[ampere][mesh]") {
    // Geometry programs read their inputs with the plain form: ISBERD R7, [R3].
    const Instruction read{{0x7923U | (7U << 16) | (3U << 24), 0, 0, 0}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(std::array{read, Exit}, true);
    REQUIRE(lowered);
    CHECK((lowered.instructions.front() >> 52) == 0x5c9);
}

TEST_CASE("Task programs emit their mesh workgroups through the task payload", "[spirv][mesh]") {
    // Mesh workgroup count at byte 4 of the output region, one payload word after it.
    const auto code = Lower(std::array{Isbewr(255, 0x4, 255, true, 3),
                                       Isbewr(255, 0x10, 255, true, 3), Exit});
    const auto spirv = Emit(Shader::Stage::Task, code, MeshRegisters);
    CHECK(HasExecutionModel(spirv, ExecutionModelTaskEXT));
    CHECK(CountOp(spirv, OpEmitMeshTasksEXT) == 1);
    CHECK(CountOp(spirv, OpReturn) == 0);
}

TEST_CASE("Mesh programs set their outputs from the stage buffer", "[spirv][mesh]") {
    // Index of this workgroup and the payload word, one position line, the primitive count and
    // three vertex indices.
    const auto code = Lower(std::array{Isberd(4, 255, 0x0), Isberd(5, 255, 0x10),
                                       Isbewr(255, 0x0, 5, true, 3),
                                       Isbewr(255, 0x3, 4, true, 0),
                                       Isbewr(255, 0x4, 255, false, 0),
                                       Isbewr(255, 0x5, 4, false, 0), Exit});
    const auto spirv = Emit(Shader::Stage::Mesh, code, MeshRegisters);
    CHECK(HasExecutionModel(spirv, ExecutionModelMeshEXT));
    CHECK(CountOp(spirv, OpSetMeshOutputsEXT) == 1);
    CHECK(CountOp(spirv, OpReturn) == 1);
}

TEST_CASE("Stage buffer accesses are rejected outside task and mesh programs", "[spirv][mesh]") {
    const auto code = Lower(std::array{Isbewr(255, 0x4, 255, true, 3), Exit});
    CHECK_THROWS_AS(Emit(Shader::Stage::VertexB, code, MeshRegisters),
                    Shader::NotImplementedException);
}
