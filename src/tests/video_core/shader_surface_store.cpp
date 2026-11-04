// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace {
class StoreEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return {}; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};

TEST_CASE("Surface stores pack selected channels and zero unselected channels", "[shader][surface]") {
    for (u32 mask = 1; mask <= 15; ++mask) {
        Shader::ObjectPool<Shader::IR::Inst> pool;
        Shader::IR::Block block{pool};
        StoreEnvironment environment;
        Shader::Maxwell::TranslatorVisitor visitor{environment, block};
        visitor.SUST((1ULL << 51) | (3ULL << 33) | (u64{mask} << 20) | (16ULL << 8) | 8);
        const auto& write = block.Instructions().back();
        REQUIRE(write.GetOpcode() == Shader::IR::Opcode::BoundImageWrite);
        const auto* color = write.Arg(2).InstRecursive();
        REQUIRE(color->GetOpcode() == Shader::IR::Opcode::CompositeConstructU32x4);
        u32 source = 8;
        for (u32 component = 0; component < 4; ++component) {
            const auto value = color->Arg(component);
            if ((mask & (1U << component)) == 0) {
                REQUIRE(value.IsImmediate());
                REQUIRE(value.U32() == 0);
            } else {
                const auto* read = value.InstRecursive();
                REQUIRE(read->GetOpcode() == Shader::IR::Opcode::GetRegister);
                REQUIRE(read->Arg(0).Reg() == Shader::IR::Reg{source++});
            }
        }
    }
}

TEST_CASE("The last generic attribute retains every component", "[shader][attribute]") {
    using namespace Shader::IR;
    for (const auto attribute : {Attribute::Generic31X, Attribute::Generic31Y,
                                  Attribute::Generic31Z, Attribute::Generic31W}) {
        REQUIRE(IsGeneric(attribute));
        REQUIRE(GenericAttributeIndex(attribute) == 31);
    }
    REQUIRE(GenericAttributeElement(Attribute::Generic31W) == 3);
    REQUIRE_FALSE(IsGeneric(Attribute::PositionX));
}
} // namespace
