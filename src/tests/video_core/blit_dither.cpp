// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
using IR::Opcode;
using IR::Value;

class BlitEnvironment final : public Shader::Environment {
public:
    bool native{true};

    explicit BlitEnvironment(Shader::Stage shader_stage) {
        stage = shader_stage;
    }

    u64 ReadInstruction(u32) override { return 0; }
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
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool HasNativeInstructions() const override { return native; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

struct BlitProgram {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    BlitEnvironment env;

    explicit BlitProgram(Shader::Stage stage = Shader::Stage::Fragment) : env{stage} {
        program.stage = stage;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
    }

    Value Emit(Opcode opcode, std::initializer_list<Value> args) {
        block.AppendNewInst(opcode, args);
        return Value{&block.back()};
    }

    Value Cbuf(u32 bank, u32 offset) {
        return Emit(Opcode::GetCbufU32, {Value{bank}, Value{offset}});
    }

    Value ColorBits() {
        const Value color = Emit(Opcode::GetAttribute, {Value{IR::Attribute::Generic0X}, Value{0U}});
        return Emit(Opcode::BitCastU32F32, {color});
    }

    void Run() {
        Shader::Optimization::BlitConstantsPass(env, program);
    }
};

void RequireZero(Value value) {
    value = value.Resolve();
    REQUIRE(value.IsImmediate());
    REQUIRE(value.U32() == 0U);
}

void RequireBlitWord(Value value, u32 offset) {
    value = value.Resolve();
    if (offset == 0x18) {
        REQUIRE(value.IsImmediate());
        REQUIRE(value.U32() == 0x3f800000U);
        return;
    }
    REQUIRE_FALSE(value.IsImmediate());
    const auto* bits = value.InstRecursive();
    REQUIRE(bits->GetOpcode() == Opcode::BitCastU32F32);
    const auto* reciprocal = bits->Arg(0).InstRecursive();
    REQUIRE(reciprocal->GetOpcode() == Opcode::FPRecip32);
    const auto* dimension = reciprocal->Arg(0).InstRecursive();
    REQUIRE(dimension->GetOpcode() == Opcode::CompositeExtractF32x4);
    REQUIRE(dimension->Arg(0).InstRecursive()->GetOpcode() == Opcode::RenderArea);
    REQUIRE(dimension->Arg(1).U32() == (offset == 0x0 || offset == 0x10 ? 0U : 1U));
}

}

TEST_CASE("Native fragment shaders read the dither bias words as zero", "[video_core][blit_dither]") {
    for (const u32 offset : {0x10U, 0x14U, 0x18U}) {
        for (const auto opcode : {Opcode::IAdd32, Opcode::ISub32}) {
            BlitProgram shader;
            const Value bias = shader.Cbuf(17, offset);
            const Value bits = shader.ColorBits();
            const Value sum = shader.Emit(opcode, {bits, bias});
            const Value color = shader.Emit(Opcode::BitCastF32U32, {sum});
            const Value typed_load = shader.Emit(Opcode::GetCbufF32, {Value{17U}, Value{offset}});
            const Value typed_fp = shader.Emit(Opcode::FPAdd32, {typed_load, Value{1.0f}});
            shader.Run();
            // The add stays, as on the console; it adds zero.
            REQUIRE(color.Inst()->Arg(0) == sum);
            REQUIRE(sum.Inst()->Arg(0) == bits);
            RequireZero(sum.Inst()->Arg(1));
            REQUIRE(typed_fp.Inst()->Arg(0) == typed_load);
            REQUIRE(typed_load.Resolve().InstRecursive()->GetOpcode() == Opcode::BitCastF32U32);
            RequireZero(typed_load.Resolve().InstRecursive()->Arg(0));
        }
    }
}

TEST_CASE("Native fragment shaders read a zero pair at the dither bias", "[video_core][blit_dither]") {
    BlitProgram shader;
    const Value pair = shader.Emit(Opcode::GetCbufU32x2, {Value{17U}, Value{0x10U}});
    const Value texel = shader.Cbuf(17, 0x0);
    shader.Run();
    const auto* composite = pair.Resolve().InstRecursive();
    REQUIRE(composite->GetOpcode() == Opcode::CompositeConstructU32x2);
    RequireZero(composite->Arg(0));
    RequireZero(composite->Arg(1));
    RequireBlitWord(texel, 0x0);
}

TEST_CASE("Fragment shaders keep the other blit words", "[video_core][blit_dither]") {
    BlitProgram shader;
    const Value width = shader.Cbuf(17, 0x0);
    const Value height = shader.Cbuf(17, 0x4);
    const Value scale = shader.Cbuf(17, 0x1c);
    shader.Run();
    RequireBlitWord(width, 0x0);
    RequireBlitWord(height, 0x4);
    REQUIRE(scale.Resolve().IsImmediate());
    REQUIRE(scale.Resolve().U32() == 0x3f800000U);
    REQUIRE(shader.program.info.uses_render_area);
}

TEST_CASE("Vertex and non-native shaders keep the texel size and UV scale at 0x10-0x18",
          "[video_core][blit_dither]") {
    for (const bool vertex : {true, false}) {
        for (const u32 offset : {0x10U, 0x14U, 0x18U}) {
            BlitProgram shader{vertex ? Shader::Stage::VertexB : Shader::Stage::Fragment};
            shader.env.native = vertex;
            const Value word = shader.Cbuf(17, offset);
            shader.Run();
            RequireBlitWord(word, offset);
        }
    }
}

TEST_CASE("Compute shaders keep their bank 17 reads", "[video_core][blit_dither]") {
    BlitProgram shader{Shader::Stage::Compute};
    const Value word = shader.Cbuf(17, 0x10);
    shader.Run();
    REQUIRE(word.Inst()->GetOpcode() == Opcode::GetCbufU32);
}

TEST_CASE("Blit words keep root table exclusions", "[video_core][blit_dither]") {
    for (const bool pair : {false, true}) {
        BlitProgram shader;
        const Value bias = shader.Cbuf(17, 0x10);
        const Value root = shader.Emit(pair ? Opcode::GetCbufU32x2 : Opcode::GetCbufU32,
                                       {Value{17U}, Value{pair ? 0x5cU : 0x80U}});
        shader.Run();
        REQUIRE(bias.Inst()->GetOpcode() == Opcode::GetCbufU32);
        REQUIRE(root.Inst()->GetOpcode() ==
                (pair ? Opcode::GetCbufU32x2 : Opcode::GetCbufU32));
        REQUIRE_FALSE(shader.program.info.uses_render_area);
    }
    BlitProgram shader;
    const Value zero_word = shader.Cbuf(17, 0x40);
    shader.Run();
    REQUIRE(zero_word.Inst()->GetOpcode() == Opcode::GetCbufU32);
    REQUIRE_FALSE(shader.program.info.uses_render_area);
}
