// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Selected semantic regression fixtures adapted from eden-fefw.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/trap_guards.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/instruction.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
namespace {
namespace IR = Shader::IR;
using Native = Shader::Ampere::Instruction;
double DecodeTestHalf(u16 bits);
u16 EncodeTestHalf(double value);
using Warp = std::array<u32, 32>;
void ExecuteNativeWarp(IR::Block& block, u32 active,
                       std::array<std::array<u32, IR::NUM_REGS>, 32>& registers,
                       std::array<std::array<bool, 7>, 32>& predicates,
                       const std::unordered_map<u32, u32>* shared = nullptr) {
    std::unordered_map<const IR::Inst*, Warp> values;
    const auto get = [&](const IR::Value& value) -> Warp {
        if (!value.IsImmediate()) return values.at(value.Inst());
        Warp result;
        result.fill(value.Type() == IR::Type::U1 ? static_cast<u32>(value.U1()) :
                    value.Type() == IR::Type::F16 || value.Type() == IR::Type::U16 ? value.U16() : value.U32());
        return result;
    };
    for (const auto& inst : block.Instructions()) {
        std::array<Warp, 4> args{};
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            if (inst.Arg(i).Type() != IR::Type::Reg && inst.Arg(i).Type() != IR::Type::Pred)
                args[i] = get(inst.Arg(i));
        }
        Warp result{};
        const auto op = inst.GetOpcode();
        if (op == IR::Opcode::SubgroupBallot || op == IR::Opcode::VoteAny || op == IR::Opcode::VoteAll) {
            u32 mask = 0;
            for (u32 lane = 0; lane < 32; ++lane)
                if ((active & (1U << lane)) && args[0][lane]) mask |= 1U << lane;
            result.fill(op == IR::Opcode::SubgroupBallot ? mask : op == IR::Opcode::VoteAny ? mask != 0 : mask == active);
        } else {
            for (u32 lane = 0; lane < 32; ++lane) {
                const auto a = args[0][lane], b = args[1][lane], c = args[2][lane];
                switch (op) {
                case IR::Opcode::GetRegister: result[lane] = static_cast<u32>(inst.Arg(0).Reg()) == 255 ? 0 : registers[lane][static_cast<u32>(inst.Arg(0).Reg())]; break;
                case IR::Opcode::SetRegister:
                    if ((active & (1U << lane)) && static_cast<u32>(inst.Arg(0).Reg()) != 255) registers[lane][static_cast<u32>(inst.Arg(0).Reg())] = b;
                    break;
                case IR::Opcode::GetPred: result[lane] = predicates[lane][static_cast<u32>(inst.Arg(0).Pred())]; break;
                case IR::Opcode::SetPred:
                    if (active & (1U << lane)) predicates[lane][static_cast<u32>(inst.Arg(0).Pred())] = b != 0;
                    break;
                case IR::Opcode::LaneId: result[lane] = lane; break;
                case IR::Opcode::LoadSharedU32:
                    REQUIRE(shared != nullptr);
                    result[lane] = shared->at(a);
                    break;
                case IR::Opcode::UnpackFloat2x16: case IR::Opcode::PackFloat2x16: result[lane] = a; break;
                case IR::Opcode::CompositeExtractF16x2: result[lane] = (a >> (b * 16)) & 0xffff; break;
                case IR::Opcode::CompositeConstructF16x2: result[lane] = a | (b << 16); break;
                case IR::Opcode::FPNeg16: result[lane] = a ^ 0x8000; break;
                case IR::Opcode::FPAbs16: result[lane] = a & 0x7fff; break;
                case IR::Opcode::ConvertF32F16: result[lane] = std::bit_cast<u32>(static_cast<float>(DecodeTestHalf(static_cast<u16>(a)))); break;
                case IR::Opcode::ConvertF16F32: result[lane] = EncodeTestHalf(std::bit_cast<float>(a)); break;
                case IR::Opcode::FPFma32: result[lane] = std::bit_cast<u32>(std::fma(std::bit_cast<float>(a), std::bit_cast<float>(b), std::bit_cast<float>(c))); break;
                case IR::Opcode::FPFma16: result[lane] = EncodeTestHalf(std::fma(DecodeTestHalf(static_cast<u16>(a)),
                    DecodeTestHalf(static_cast<u16>(b)), DecodeTestHalf(static_cast<u16>(c)))); break;
                case IR::Opcode::BitCastF32U32: case IR::Opcode::BitCastU32F32: result[lane] = a; break;
                case IR::Opcode::SelectU32: case IR::Opcode::SelectU1: result[lane] = a ? b : c; break;
                case IR::Opcode::BitwiseAnd32: result[lane] = a & b; break;
                case IR::Opcode::BitwiseOr32: result[lane] = a | b; break;
                case IR::Opcode::BitwiseNot32: result[lane] = ~a; break;
                case IR::Opcode::UMin32: result[lane] = std::min(a, b); break;
                case IR::Opcode::UMax32: result[lane] = std::max(a, b); break;
                case IR::Opcode::ShiftRightLogical32: result[lane] = b < 32 ? a >> b : 0; break;
                case IR::Opcode::ShiftLeftLogical32: result[lane] = b < 32 ? a << b : 0; break;
                case IR::Opcode::INeg32: result[lane] = 0U - a; break;
                case IR::Opcode::IAdd32: result[lane] = a + b; break;
                case IR::Opcode::FindUMsb32: result[lane] = a ? 31U - std::countl_zero(a) : ~0U; break;
                case IR::Opcode::BitFieldUExtract: result[lane] = (a >> b) & ((1U << c) - 1); break;
                case IR::Opcode::ULessThan: result[lane] = a < b; break;
                case IR::Opcode::SLessThan: result[lane] = static_cast<s32>(a) < static_cast<s32>(b); break;
                case IR::Opcode::IEqual: result[lane] = a == b; break;
                case IR::Opcode::GetZeroFromOp: result[lane] = a == 0; break;
                case IR::Opcode::LogicalAnd: result[lane] = a && b; break;
                case IR::Opcode::LogicalNot: result[lane] = !a; break;
                case IR::Opcode::GetInBoundsFromOp: {
                    const auto* shuffle = inst.Arg(0).Inst();
                    const u32 index = get(shuffle->Arg(1))[lane] & 31;
                    const u32 clamp = get(shuffle->Arg(2))[lane], segment = get(shuffle->Arg(3))[lane];
                    const bool upward = shuffle->GetOpcode() == IR::Opcode::ShuffleUp;
                    const u32 target = upward ? lane - index : shuffle->GetOpcode() == IR::Opcode::ShuffleButterfly
                        ? lane ^ index : (index & ~segment) | (lane & segment);
                    const u32 bound = (lane & segment) | (clamp & ~segment);
                    result[lane] = upward ? static_cast<s32>(target) >= static_cast<s32>(bound) : target <= bound;
                    break;
                }
                case IR::Opcode::FSwizzleAdd: {
                    const u32 mode = (c >> ((lane & 3) * 2)) & 3;
                    const float x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
                    const float ma = mode == 0 || mode == 2 ? -1.0f : mode == 1 ? 1.0f : 0.0f;
                    const float mb = mode == 0 || mode == 1 ? -1.0f : mode == 2 ? 1.0f : 0.0f;
                    result[lane] = std::bit_cast<u32>(x * ma + y * mb);
                    break;
                }
                case IR::Opcode::ShuffleDown: result[lane] = args[0][lane + b <= c ? lane + b : lane]; break;
                case IR::Opcode::ShuffleButterfly: case IR::Opcode::ShuffleIndex: case IR::Opcode::ShuffleUp: {
                    const u32 segment = args[3][lane];
                    const bool upward = op == IR::Opcode::ShuffleUp;
                    const u32 target = upward ? lane - (b & 31) : op == IR::Opcode::ShuffleButterfly
                        ? lane ^ (b & 31) : ((b & 31) & ~segment) | (lane & segment);
                    const u32 bound = (lane & segment) | (c & ~segment);
                    const bool valid = upward ? static_cast<s32>(target) >= static_cast<s32>(bound) : target <= bound;
                    result[lane] = !valid ? a
                        : (active & (1U << target)) ? args[0][target] : 0xdeadbeefU;
                    break;
                }
                default: FAIL("Unexpected warp-uniform IR " << IR::NameOf(op));
                }
            }
        }
        values.emplace(&inst, result);
    }
}
class EmptyEnvironment : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::logic_error("unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { throw std::logic_error("unexpected constant folding"); }
    Shader::TextureType ReadTextureType(u32) override { return {}; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return local_size; }
    u32 local_size{};
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}
};

constexpr std::array<Native, 7> VertexTransform{{
    {{0xff007321, 0x000080ff, 0x00000400, 0x000e2200}}, // load two input components
    {{0x00037a02, 0x00c00000, 0x00000f00, 0x000fe400}}, // scale X
    {{0x00027a02, 0x00c00100, 0x00000f00, 0x000fe200}}, // scale Y
    {{0x00007623, 0x00c00200, 0x00010003, 0x001fe200}}, // X * scale X + offset X
    {{0x01017623, 0x00c00300, 0x00010002, 0x000fc400}}, // Y * scale Y + offset Y
    {{0xff007322, 0x00007000, 0x000004ff, 0x000fe200}}, // store position X/Y
    {{0x0000794d, 0x00000000, 0x03800000, 0x000fea00}},
}};

double DecodeTestHalf(u16 bits) {
    const u32 exponent = (bits >> 10) & 31, mantissa = bits & 1023;
    const double value = exponent == 31 ? (mantissa ? std::numeric_limits<double>::quiet_NaN()
                                                   : std::numeric_limits<double>::infinity())
                        : exponent == 0 ? std::ldexp(static_cast<double>(mantissa), -24)
                                        : std::ldexp(static_cast<double>(1024 + mantissa), static_cast<int>(exponent) - 25);
    return (bits & 0x8000) ? -value : value;
}
u16 EncodeTestHalf(double value) {
    const u16 sign = std::signbit(value) ? 0x8000 : 0;
    value = std::abs(value);
    if (std::isnan(value)) return 0xfe01; // Deliberately noncanonical: ReLU must canonicalize it.
    if (value >= 65520) return sign | 0x7c00;
    if (value < std::ldexp(1.0, -14))
        return sign | static_cast<u16>(std::nearbyint(std::ldexp(value, 24)));
    int exponent{};
    std::frexp(value, &exponent);
    --exponent;
    u32 mantissa = static_cast<u32>(std::nearbyint(std::ldexp(value, 10 - exponent)));
    if (mantissa == 2048) { ++exponent; mantissa = 1024; }
    return sign | static_cast<u16>(((exponent + 15) << 10) | (mantissa - 1024));
}
struct ScalarNativeMachine {
    std::array<u32, IR::NUM_REGS> regs{};
    std::array<bool, 7> predicates{};
    std::unordered_map<u64, u32> constants, global, shared, local;
    std::unordered_map<u32, u32> attributes;
    u32 workgroup_barriers{}, device_barriers{}, shared_memory_barriers{};
    bool carry{}, zero{}, sign{}, overflow{};
    void Execute(IR::Block& block) {
        std::unordered_map<const IR::Inst*, u64> values;
        std::unordered_map<const IR::Inst*, std::array<u32, 4>> vectors;
        const auto get = [&](const IR::Value& value) -> u64 {
            if (!value.IsImmediate()) return values.at(value.Inst());
            if (value.Type() == IR::Type::U1) return value.U1();
            if (value.Type() == IR::Type::F16 || value.Type() == IR::Type::U16) return value.U16();
            if (value.Type() == IR::Type::U64) return value.U64();
            if (value.Type() == IR::Type::F32) return std::bit_cast<u32>(value.F32());
            return value.U32();
        };
        for (const auto& inst : block.Instructions()) {
            const auto arg = [&](size_t i) { return get(inst.Arg(i)); };
            u64 result{};
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetAttribute: case IR::Opcode::GetAttributeU32:
                result = attributes.at(static_cast<u32>(inst.Arg(0).Attribute())); break;
            case IR::Opcode::SetAttribute:
                attributes[static_cast<u32>(inst.Arg(0).Attribute())] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::BitCastF32U32: case IR::Opcode::BitCastU32F32: result = arg(0); break;
            case IR::Opcode::UnpackFloat2x16: result = (arg(0) & 0xffff) | ((arg(0) >> 16) << 32); break;
            case IR::Opcode::PackFloat2x16: result = (arg(0) & 0xffff) | ((arg(0) >> 32) << 16); break;
            case IR::Opcode::CompositeConstructF16x2: result = arg(0) | (arg(1) << 32); break;
            case IR::Opcode::CompositeExtractF16x2: result = (arg(0) >> (arg(1) * 32)) & 0xffff; break;
            case IR::Opcode::FPNeg16: result = arg(0) ^ 0x8000; break;
            case IR::Opcode::FPAbs16: result = arg(0) & 0x7fff; break;
            case IR::Opcode::FPMul32: result = std::bit_cast<u32>(std::bit_cast<float>(static_cast<u32>(arg(0))) * std::bit_cast<float>(static_cast<u32>(arg(1)))); break;
            case IR::Opcode::FPAdd32: result = std::bit_cast<u32>(std::bit_cast<float>(static_cast<u32>(arg(0))) + std::bit_cast<float>(static_cast<u32>(arg(1)))); break;
            case IR::Opcode::FPFma32: result = std::bit_cast<u32>(std::fma(std::bit_cast<float>(static_cast<u32>(arg(0))),
                std::bit_cast<float>(static_cast<u32>(arg(1))), std::bit_cast<float>(static_cast<u32>(arg(2))))); break;
            case IR::Opcode::FPSaturate32: {
                const float value = std::bit_cast<float>(static_cast<u32>(arg(0)));
                result = std::bit_cast<u32>(std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f));
                break;
            }
            case IR::Opcode::FPFma16: result = EncodeTestHalf(std::fma(DecodeTestHalf(static_cast<u16>(arg(0))),
                DecodeTestHalf(static_cast<u16>(arg(1))), DecodeTestHalf(static_cast<u16>(arg(2))))); break;
            case IR::Opcode::FPRecip32: result = std::bit_cast<u32>(1.0f / std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPFloor32: result = std::bit_cast<u32>(std::floor(std::bit_cast<float>(static_cast<u32>(arg(0))))); break;
            case IR::Opcode::ConvertF32F16: result = std::bit_cast<u32>(static_cast<float>(DecodeTestHalf(static_cast<u16>(arg(0))))); break;
            case IR::Opcode::FPUnordGreaterThanEqual16: {
                const float a = DecodeTestHalf(static_cast<u16>(arg(0)));
                const float b = DecodeTestHalf(static_cast<u16>(arg(1)));
                result = std::isnan(a) || std::isnan(b) || a >= b;
                break;
            }
            case IR::Opcode::FPUnordEqual16: {
                const double a = DecodeTestHalf(static_cast<u16>(arg(0))), b = DecodeTestHalf(static_cast<u16>(arg(1)));
                result = std::isnan(a) || std::isnan(b) || a == b;
                break;
            }
            case IR::Opcode::FPOrdEqual32: result = std::bit_cast<float>(static_cast<u32>(arg(0))) == std::bit_cast<float>(static_cast<u32>(arg(1))); break;
            case IR::Opcode::FPOrdLessThanEqual32: result = std::bit_cast<float>(static_cast<u32>(arg(0))) <= std::bit_cast<float>(static_cast<u32>(arg(1))); break;
            case IR::Opcode::FPOrdGreaterThanEqual32: result = std::bit_cast<float>(static_cast<u32>(arg(0))) >= std::bit_cast<float>(static_cast<u32>(arg(1))); break;
            case IR::Opcode::FPMax32: result = std::bit_cast<u32>(std::fmax(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))))); break;
            case IR::Opcode::FPMin32: result = std::bit_cast<u32>(std::fmin(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))))); break;
            case IR::Opcode::ConvertU64F32: {
                const float value = std::bit_cast<float>(static_cast<u32>(arg(0)));
                REQUIRE(std::isfinite(value));
                REQUIRE(value >= 0.0f);
                REQUIRE(static_cast<double>(value) < 0x1p64);
                result = static_cast<u64>(value);
                break;
            }
            case IR::Opcode::ConvertF32U64: result = std::bit_cast<u32>(static_cast<float>(arg(0))); break;
            case IR::Opcode::ConvertF32U32: result = std::bit_cast<u32>(static_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::ConvertU32F32: result = static_cast<u32>(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPTrunc32: result = std::bit_cast<u32>(std::trunc(std::bit_cast<float>(static_cast<u32>(arg(0))))); break;
            case IR::Opcode::FPNeg32: result = arg(0) ^ 0x80000000U; break;
            case IR::Opcode::IAbs32: result = static_cast<s32>(arg(0)) < 0 ? 0U - static_cast<u32>(arg(0)) : static_cast<u32>(arg(0)); break;
            case IR::Opcode::FPAbs32: result = arg(0) & 0x7fffffffU; break;
            case IR::Opcode::ConvertF16F32: result = EncodeTestHalf(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPRoundEven32: result = std::bit_cast<u32>(std::nearbyint(std::bit_cast<float>(static_cast<u32>(arg(0))))); break;
            case IR::Opcode::FPIsNan32: result = std::isnan(std::bit_cast<float>(static_cast<u32>(arg(0)))); break;
            case IR::Opcode::FPClamp32: result = std::bit_cast<u32>(std::clamp(std::bit_cast<float>(static_cast<u32>(arg(0))), std::bit_cast<float>(static_cast<u32>(arg(1))), std::bit_cast<float>(static_cast<u32>(arg(2))))); break;
            case IR::Opcode::ConvertS32F32: {
                const float value = std::bit_cast<float>(static_cast<u32>(arg(0)));
                result = std::isnan(value) ? 0x87654321U : static_cast<u32>(static_cast<s32>(value));
                break;
            }
            case IR::Opcode::GetRegister: result = inst.Arg(0).Reg() == IR::Reg::RZ ? 0 : regs[static_cast<u32>(inst.Arg(0).Reg())]; break;
            case IR::Opcode::SetRegister: if (inst.Arg(0).Reg() != IR::Reg::RZ) regs[static_cast<u32>(inst.Arg(0).Reg())] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::GetPred: result = predicates[static_cast<u32>(inst.Arg(0).Pred())]; break;
            case IR::Opcode::SetPred: predicates[static_cast<u32>(inst.Arg(0).Pred())] = arg(1) != 0; break;
            case IR::Opcode::IAdd32: result = static_cast<u32>(arg(0) + arg(1)); break;
            case IR::Opcode::ISub32: result = static_cast<u32>(arg(0) - arg(1)); break;
            case IR::Opcode::IMul32: result = static_cast<u32>(arg(0) * arg(1)); break;
            case IR::Opcode::IAdd64: result = arg(0) + arg(1); break;
            case IR::Opcode::UMax32: result = std::max(arg(0), arg(1)); break;
            case IR::Opcode::UMin32: result = std::min(arg(0), arg(1)); break;
            case IR::Opcode::FindUMsb32: result = arg(0) ? 31U - std::countl_zero(static_cast<u32>(arg(0))) : ~0U; break;
            case IR::Opcode::BitReverse32: {
                const u32 source = static_cast<u32>(arg(0));
                for (u32 bit = 0; bit < 32; ++bit) result |= u64{(source >> bit) & 1U} << (31 - bit);
                break;
            }
            case IR::Opcode::ShiftLeftLogical64: result = arg(1) < 64 ? arg(0) << arg(1) : 0; break;
            case IR::Opcode::ShiftRightLogical64: result = arg(1) < 64 ? arg(0) >> arg(1) : 0; break;
            case IR::Opcode::INeg32: result = 0U - static_cast<u32>(arg(0)); break;
            case IR::Opcode::ShiftLeftLogical32: result = arg(1) < 32 ? static_cast<u32>(arg(0) << arg(1)) : 0; break;
            case IR::Opcode::ShiftRightArithmetic32: result = static_cast<u32>(static_cast<s32>(arg(0)) >> std::min<u64>(arg(1), 31)); break;
            case IR::Opcode::ShiftRightLogical32: result = arg(1) < 32 ? static_cast<u32>(arg(0)) >> arg(1) : 0; break;
            case IR::Opcode::BitwiseAnd32: result = arg(0) & arg(1); break;
            case IR::Opcode::BitwiseOr32: result = arg(0) | arg(1); break;
            case IR::Opcode::BitwiseXor32: result = arg(0) ^ arg(1); break;
            case IR::Opcode::BitwiseNot32: result = arg(0) ^ 0xffffffffULL; break;
            case IR::Opcode::BitFieldSExtract: {
                const u32 value = static_cast<u32>(arg(0)) >> arg(1);
                const u32 width = static_cast<u32>(arg(2));
                result = width == 0 ? 0 : width == 32 ? value :
                    static_cast<u32>(static_cast<s32>(value << (32 - width)) >> (32 - width));
                break;
            }
            case IR::Opcode::BitFieldUExtract: result = (arg(0) >> arg(1)) & ((u64{1} << arg(2)) - 1); break;
            case IR::Opcode::BitFieldInsert: { const u64 mask = ((u64{1} << arg(3)) - 1) << arg(2); result = (arg(0) & ~mask) | ((arg(1) << arg(2)) & mask); break; }
            case IR::Opcode::SelectF32: case IR::Opcode::SelectU64: case IR::Opcode::SelectU32: case IR::Opcode::SelectU1: result = arg(0) ? arg(1) : arg(2); break;
            case IR::Opcode::LogicalAnd: result = arg(0) && arg(1); break;
            case IR::Opcode::LogicalOr: result = arg(0) || arg(1); break;
            case IR::Opcode::LogicalXor: result = (arg(0) != 0) != (arg(1) != 0); break;
            case IR::Opcode::LogicalNot: result = !arg(0); break;
            case IR::Opcode::SMin32: result = static_cast<u32>(std::min(static_cast<s32>(arg(0)), static_cast<s32>(arg(1)))); break;
            case IR::Opcode::SMax32: result = static_cast<u32>(std::max(static_cast<s32>(arg(0)), static_cast<s32>(arg(1)))); break;
            case IR::Opcode::SLessThan: result = static_cast<s32>(arg(0)) < static_cast<s32>(arg(1)); break;
            case IR::Opcode::ULessThan: result = arg(0) < arg(1); break;
            case IR::Opcode::ULessThanEqual: result = arg(0) <= arg(1); break;
            case IR::Opcode::UGreaterThan: result = arg(0) > arg(1); break;
            case IR::Opcode::UGreaterThanEqual: result = arg(0) >= arg(1); break;
            case IR::Opcode::IEqual: result = arg(0) == arg(1); break;
            case IR::Opcode::INotEqual: result = arg(0) != arg(1); break;
            case IR::Opcode::GetCFlag: result = carry; break;
            case IR::Opcode::SetCFlag: carry = arg(0) != 0; break;
            case IR::Opcode::GetZFlag: result = zero; break;
            case IR::Opcode::SetZFlag: zero = arg(0) != 0; break;
            case IR::Opcode::GetSFlag: result = sign; break;
            case IR::Opcode::SetSFlag: sign = arg(0) != 0; break;
            case IR::Opcode::GetOFlag: result = overflow; break;
            case IR::Opcode::SetOFlag: overflow = arg(0) != 0; break;
            case IR::Opcode::GetCarryFromOp: { const auto* sum = inst.Arg(0).Inst(); result = get(sum->Arg(0)) + get(sum->Arg(1)) > 0xffffffffULL; break; }
            case IR::Opcode::GetZeroFromOp: result = arg(0) == 0; break;
            case IR::Opcode::GetSignFromOp: result = (arg(0) & 0x80000000U) != 0; break;
            case IR::Opcode::GetOverflowFromOp: { const auto* sum = inst.Arg(0).Inst(); result = ((get(sum->Arg(0)) ^ arg(0)) & (get(sum->Arg(1)) ^ arg(0)) & 0x80000000U) != 0; break; }
            case IR::Opcode::GetCbufU8: result = constants.at((arg(0) << 32) | arg(1)) & 0xff; break;
            case IR::Opcode::GetCbufU32: result = constants.at((arg(0) << 32) | arg(1)); break;
            case IR::Opcode::GetCbufU32x2: result = constants.at((arg(0) << 32) | arg(1)) | (u64{constants.at((arg(0) << 32) | (arg(1) + 4))} << 32); break;
            case IR::Opcode::CompositeConstructU32x2: result = arg(0) | (arg(1) << 32); break;
            case IR::Opcode::PackUint2x32: case IR::Opcode::UnpackUint2x32: result = arg(0); break;
            case IR::Opcode::CompositeExtractU32x2: result = static_cast<u32>(arg(0) >> (arg(1) * 32)); break;
            case IR::Opcode::CompositeConstructU32x4:
                vectors[&inst] = {static_cast<u32>(arg(0)), static_cast<u32>(arg(1)), static_cast<u32>(arg(2)), static_cast<u32>(arg(3))};
                break;
            case IR::Opcode::WriteSharedU128:
                for (u32 component = 0; component < 4; ++component) shared[arg(0) + component * 4] = vectors.at(inst.Arg(1).Inst())[component];
                break;
            case IR::Opcode::WriteSharedU64: shared[arg(0)] = static_cast<u32>(arg(1)); shared[arg(0) + 4] = static_cast<u32>(arg(1) >> 32); break;
            case IR::Opcode::LoadSharedU64: result = shared.at(arg(0)) | (u64{shared.at(arg(0) + 4)} << 32); break;
            case IR::Opcode::LoadLocal: result = local.at(arg(0)); break;
            case IR::Opcode::WriteLocal: local[arg(0)] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::LoadGlobalU16: result = global.at(arg(0)) & 0xffff; break;
            case IR::Opcode::LoadGlobal32: result = global.at(arg(0)); break;
            case IR::Opcode::LoadGlobal64: result = global.at(arg(0)) | (u64{global.at(arg(0) + 4)} << 32); break;
            case IR::Opcode::WriteGlobal64: global[arg(0)] = static_cast<u32>(arg(1)); global[arg(0) + 4] = static_cast<u32>(arg(1) >> 32); break;
            case IR::Opcode::WriteGlobalU16: global[arg(0)] = static_cast<u32>(arg(1)) & 0xffff; break;
            case IR::Opcode::WriteGlobalU8: global[arg(0)] = static_cast<u32>(arg(1)) & 0xff; break;
            case IR::Opcode::WriteGlobal32: global[arg(0)] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::WriteSharedU32: shared[arg(0)] = static_cast<u32>(arg(1)); break;
            case IR::Opcode::LoadSharedU32: result = shared.at(arg(0)); break;
            case IR::Opcode::GlobalAtomicIAdd32: result = global.at(arg(0)); global[arg(0)] += static_cast<u32>(arg(1)); break;
            case IR::Opcode::GlobalAtomicUMax32: result = global.at(arg(0)); global[arg(0)] = std::max(global[arg(0)], static_cast<u32>(arg(1))); break;
            case IR::Opcode::SharedAtomicUMin32: result = shared.at(arg(0)); shared[arg(0)] = std::min(shared[arg(0)], static_cast<u32>(arg(1))); break;
            case IR::Opcode::SharedAtomicUMax32: result = shared.at(arg(0)); shared[arg(0)] = std::max(shared[arg(0)], static_cast<u32>(arg(1))); break;
            case IR::Opcode::SharedAtomicOr32: result = shared.at(arg(0)); shared[arg(0)] |= static_cast<u32>(arg(1)); break;
            case IR::Opcode::SharedAtomicIAdd32: result = shared.at(arg(0)); shared[arg(0)] += static_cast<u32>(arg(1)); break;
            case IR::Opcode::Barrier: ++workgroup_barriers; break;
            case IR::Opcode::DeviceMemoryBarrier: ++device_barriers; break;
            case IR::Opcode::WorkgroupMemoryBarrier: ++shared_memory_barriers; break;
            default: FAIL("Unexpected scalar compute IR " << IR::NameOf(inst.GetOpcode()));
            }
            values[&inst] = result;
        }
    }
};
void TranslateScalarNative(std::span<const Native> input, IR::Block& block, bool shadow = false, u32 local_size = 0) {
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    INFO("offset=" << lowered.error_offset << " error=" << lowered.error);
    REQUIRE(lowered);
    EmptyEnvironment env;
    env.local_size = local_size;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    for (u64 word : lowered.instructions) {
        const auto opcode = Shader::Maxwell::Decode(word);
        if (opcode == Shader::Maxwell::Opcode::EXIT) continue;
        switch (opcode) {
#define INST(name, cute, encode) case Shader::Maxwell::Opcode::name: visitor.name(word); break;
#include "shader_recompiler/frontend/maxwell/maxwell.inc"
#undef INST
        }
    }
}
void ExecutePredicatedScalarNative(std::span<const Native> input, ScalarNativeMachine& machine, bool shadow = false, u32 local_size = 0) {
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    INFO(lowered.error << " offset=" << lowered.error_offset);
    REQUIRE(lowered);
    for (size_t position = 0; position < lowered.instructions.size(); ++position) {
        const u64 word = lowered.instructions[position];
        const auto opcode = Shader::Maxwell::Decode(word);
        if (opcode == Shader::Maxwell::Opcode::EXIT) continue;
        const auto pred = Shader::Maxwell::Instruction{word}.Pred();
        const bool active = (pred.index == 7 ? true : machine.predicates[pred.index]) != pred.negated;
        if (!active) continue;
        if (opcode == Shader::Maxwell::Opcode::BRA) {
            const auto address = [](size_t index) { return 0x50U + index * 8 + ((index + 1) / 3) * 8; };
            const s32 relative = static_cast<s32>(static_cast<u32>((word >> 20) & 0xffffff) << 8) >> 8;
            const s64 target_address = static_cast<s64>(address(position)) + 8 + relative;
            size_t target = position + 1;
            while (target < lowered.instructions.size() && static_cast<s64>(address(target)) != target_address) ++target;
            REQUIRE(target < lowered.instructions.size());
            position = target - 1;
            continue;
        }
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        EmptyEnvironment env;
        env.local_size = local_size;
        Shader::Maxwell::TranslatorVisitor visitor{env, block};
        switch (opcode) {
#define INST(name, cute, encode) case Shader::Maxwell::Opcode::name: visitor.name(word); break;
#include "shader_recompiler/frontend/maxwell/maxwell.inc"
#undef INST
        }
        machine.Execute(block);
    }
}
}

TEST_CASE("Native high multiplication reads a full constant-buffer multiplier", "[ampere][fefw]") {
    const u32 destination = GENERATE(0U, 2U);
    const std::array<Native, 2> input{{
        {{(destination << 16) | 0x7a27U, 0x00c0b300, 0x078e00ff, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 a : {0U, 1U, 0x12345678U, 0x80000000U, 0xffffffffU})
        for (u32 b : {0U, 1U, 0x87654321U, 0x80000000U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[0] = a;
            machine.constants[(u64{3} << 32) | 0x2cc] = b;
            machine.Execute(block);
            REQUIRE(machine.regs[destination] == static_cast<u32>((u64{a} * b) >> 32));
        }
}


TEST_CASE("Native signed high multiplication preserves signs and input aliases", "[ampere][fefw]") {
    const u32 destination = GENERATE(0U, 3U);
    const std::array<u32, 9> values{{0, 1, 9, 10, 0x66666667U, 0x7fffffffU, 0x80000000U, 0xffffffc0U, 0xffffffffU}};
    for (u32 b : values) {
        const std::array<Native, 2> input{{
            {{(destination << 16) | 0x7827U, b, 0x078e02ff, 0}}, VertexTransform.back(),
        }};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 a : values) {
            ScalarNativeMachine machine;
            machine.regs[0] = a;
            const s64 product = s64{static_cast<s32>(a)} * static_cast<s32>(b);
            machine.Execute(block);
            REQUIRE(machine.regs[destination] == static_cast<u32>(static_cast<u64>(product) >> 32));
        }
    }
}


TEST_CASE("Native min/max immediate preserves full signed and unsigned values", "[ampere][fefw]") {
    const bool constant = GENERATE(false, true);
    const bool signed_compare = GENERATE(false, true);
    const bool maximum = GENERATE(false, true);
    const u32 destination = GENERATE(0U, 2U);
    const std::array<u32, 8> values{{0, 1, 0x7ffffU, 0x80000U, 0x7fffffffU, 0x80000000U, 0xffffffc0U, 0xffffffffU}};
    for (u32 b : values) {
        const std::array<Native, 2> input{{
            {{(destination << 16) | (constant ? 0x7a17U : 0x7817U), constant ? 0x00c04000U : b, 0x03800000U | (signed_compare ? 0x200U : 0U) |
                (maximum ? 0x04000000U : 0U), 0}},
            VertexTransform.back(),
        }};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(input, block);
        for (u32 a : values) {
            ScalarNativeMachine machine;
            machine.regs[0] = a;
            machine.constants[(u64{3} << 32) | 0x100] = b;
            machine.predicates = {true, false, true, false, true, false, true};
            const auto predicates = machine.predicates;
            const bool less = signed_compare ? static_cast<s32>(a) < static_cast<s32>(b) : a < b;
            const u32 expected = (less != maximum) ? a : b;
            machine.Execute(block);
            REQUIRE(machine.regs[destination] == expected);
            REQUIRE(machine.predicates == predicates);
        }
    }
}


TEST_CASE("Uniform native trap guards reject changed constants and unknown inputs", "[ampere][fefw]") {
    std::array<Native, 5> input{{
        {{0xff037624, 0, 0x078e00ff, 0}},
        {{0x0300720c, 255, 0x03f06270, 0}},
        {{0x0947, 16, 0x03800000, 0}},
        {{0x795c, 4, 0x00300000, 0}},
        VertexTransform.back(),
    }};
    u32 constant{};
    const Shader::Ampere::TrapGuardReader read = [&](u32 bank, u32 offset) -> std::optional<u32> {
        REQUIRE(bank == 0); REQUIRE(offset == 0);
        return constant;
    };
    const auto prefix = std::span{input}.first(4);
    for (u32 value : {0U, 1U, 0x7fffffffU, 0x80000000U, 0xffffffffU}) {
        constant = value;
        REQUIRE(Shader::Ampere::VerifyTrapGuards(prefix, read) == (value < 0x80000000U));
    }
    REQUIRE_FALSE(Shader::Ampere::VerifyTrapGuards(prefix, {}));
    REQUIRE_FALSE(Shader::Ampere::VerifyTrapGuards(prefix, [](u32, u32) { return std::optional<u32>{}; }));
    constant = 0;
    auto unknown = input;
    unknown[0] = Native{{0xff037224, 255, 0x078e0004, 0}};
    REQUIRE_FALSE(Shader::Ampere::VerifyTrapGuards(std::span{unknown}.first(4), read));
    unknown = input;
    unknown[0] = Native{{0x00037919, 0, 0x2100, 0}};
    REQUIRE_FALSE(Shader::Ampere::VerifyTrapGuards(std::span{unknown}.first(4), read));
    unknown = input;
    unknown[2].words[1] = 0xfffffff0;
    REQUIRE_FALSE(Shader::Ampere::VerifyTrapGuards(std::span{unknown}.first(4), read));
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
    const std::array<u32, 1> traps{48};
    REQUIRE(Shader::Ampere::LowerToMaxwell(input, false, traps));
}


TEST_CASE("Native ordered extended comparisons preserve low-word decisions and predicates", "[ampere][fefw]") {
    const u32 comparison = GENERATE(1U, 3U, 4U, 6U);
    const u32 output = GENERATE(0U, 6U);
    const bool alias = GENERATE(false, true);
    const bool negate_low = GENERATE(false, true);
    const u32 low_input = alias ? output : 3U;
    const std::array<Native, 2> input{{
        {{0x0100720c, 2, 0x03f00100U | (comparison << 12) | (output << 17) |
            ((low_input | (negate_low ? 8U : 0U)) << 4), 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    const auto compare = [&](u64 a, u64 b) {
        if (comparison == 1) return a < b;
        if (comparison == 3) return a <= b;
        if (comparison == 4) return a > b;
        return a >= b;
    };
    const std::array<u64, 8> values{{0, 1, 0xffffffffULL, 0x100000000ULL,
        0x100000001ULL, 0x7fffffff00000000ULL, 0x8000000000000000ULL, ~u64{0}}};
    for (u64 a : values) for (u64 b : values) {
        ScalarNativeMachine machine;
        machine.regs[1] = static_cast<u32>(a >> 32);
        machine.regs[2] = static_cast<u32>(b >> 32);
        machine.predicates = {true, false, true, false, true, false, true};
        machine.predicates[low_input] = compare(static_cast<u32>(a), static_cast<u32>(b)) != negate_low;
        auto expected = machine.predicates;
        expected[output] = compare(a, b);
        machine.Execute(block);
        REQUIRE(machine.predicates == expected);
        REQUIRE(machine.regs[1] == static_cast<u32>(a >> 32));
        REQUIRE(machine.regs[2] == static_cast<u32>(b >> 32));
    }
}


TEST_CASE("Native ordered extended comparisons retain execution guards", "[ampere][fefw]") {
    const u32 execution = GENERATE(0U, 7U, 8U);
    const std::array<Native, 2> input{{
        {{(1U << 24) | (execution << 12) | 0x20cU, 2, 0x03f01100U, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() > 2);
    for (size_t i = 0; i + 1 < lowered.instructions.size(); ++i) {
        const auto predicate = Shader::Maxwell::Instruction{lowered.instructions[i]}.Pred();
        REQUIRE(predicate.index == (execution & 7));
        REQUIRE(predicate.negated == ((execution & 8) != 0));
    }
    for (u32 field : {0x200U, 0x400U, 0x800000U}) {
        auto invalid = input;
        invalid[0].words[2] ^= field;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    }
}


TEST_CASE("Native warp sum OR and unsigned extrema reduce only active lanes", "[ampere][fefw]") {
    const u32 operation = GENERATE(0U, 1U, 2U, 3U);
    std::array<Native, 2> input{{{{0x030573c4, 0, 0x0000c000, 0}}, VertexTransform.back()}};
    if (operation == 1) input[0].words[2] = 0x4000;
    if (operation == 2) input[0].words[2] = 0x10000;
    if (operation == 3) input[0].words[2] = 0x14000;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 active : {1U, 0x80000000U, 0x80000001U, 0x55555555U, 0xffffffffU}) {
        std::array<std::array<u32, IR::NUM_REGS>, 32> registers{};
        std::array<std::array<bool, 7>, 32> predicates{};
        u32 sum = operation == 2 ? 0xffffffffU : 0U;
        for (u32 lane = 0; lane < 32; ++lane) {
            registers[lane][3] = 0xfffffff0U + lane * 17;
            registers[lane][221] = 0xcafebeef;
            if (active & (1U << lane)) sum = operation == 3 ? std::max(sum, registers[lane][3]) : operation == 2 ? std::min(sum, registers[lane][3]) : operation == 1 ? sum | registers[lane][3] : sum + registers[lane][3];
            for (u32 p = 0; p < 7; ++p) predicates[lane][p] = ((lane + p) & 1) != 0;
        }
        const auto original = predicates;
        ExecuteNativeWarp(block, active, registers, predicates);
        REQUIRE(predicates == original);
        for (u32 lane = 0; lane < 32; ++lane)
            REQUIRE(registers[lane][221] == ((active & (1U << lane)) ? sum : 0xcafebeef));
    }
}


TEST_CASE("Native constant shift counts wrap at the word width", "[ampere][fefw]") {
    const std::array<Native, 2> input{{
        {{0xff097a19, 0x01003500, 0x00011e0a, 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 shift : {0U, 1U, 10U, 31U, 32U, 33U, 63U, 0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[10] = 0xfedcba98;
        machine.constants[(u64{4} << 32) | 0xd4] = shift;
        machine.Execute(block);
        REQUIRE(machine.regs[9] == (0xfedcba98U >> (shift & 31)));
        REQUIRE(machine.regs[10] == 0xfedcba98);
    }
}


TEST_CASE("Native extended constant-buffer addition retains its register source and carry", "[ampere][fefw]") {
    const u32 destination = GENERATE(0U, 4U);
    const u32 carry = GENERATE(0U, 7U, 8U, 15U);
    const std::array<Native, 2> input{{
        {{(destination << 16) | 0x7a10U, 0x00c04000, 0x007fe4ffU | (carry << 23), 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 a : {0U, 1U, 0x80000000U, 0xffffffffU})
        for (u32 b : {0U, 1U, 0x76543210U, 0xffffffffU}) for (bool active : {false, true}) {
            ScalarNativeMachine machine;
            machine.regs[0] = a;
            machine.constants[(u64{3} << 32) | 0x100] = b;
            machine.predicates.fill(active);
            const auto predicates = machine.predicates;
            const bool enabled = ((carry & 7) == 7 ? true : active) != bool(carry & 8);
            machine.Execute(block);
            REQUIRE(machine.regs[destination] == a + b + static_cast<u32>(enabled));
            REQUIRE(machine.predicates == predicates);
        }
}


TEST_CASE("Native formatted surface stores retain coordinates channels and descriptor", "[ampere][fefw]") {
    const bool strong = GENERATE(false, true);
    const std::array<Native, 2> input{{{{0x00007b9b, 0x60000a04, strong ? 0x0010af00U : 0x00100f00U, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 writes{}, barriers{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::DeviceMemoryBarrier) {
            REQUIRE(barriers == writes);
            ++barriers;
        }
        if (inst.GetOpcode() == IR::Opcode::BoundImageWrite) {
        REQUIRE(barriers == static_cast<u32>(strong));
        REQUIRE(inst.Arg(0).U32() == 40);
        const auto info = inst.Flags<IR::TextureInstInfo>();
        REQUIRE(info.type.Value() == Shader::TextureType::Color2D);
        const auto* coordinates = inst.Arg(1).Inst();
        REQUIRE(coordinates->Arg(0).Inst()->Arg(0).Reg() == IR::Reg::R0);
        REQUIRE(coordinates->Arg(1).Inst()->Arg(0).Reg() == IR::Reg::R1);
        const auto* color = inst.Arg(2).Inst();
        for (u32 component = 0; component < 4; ++component)
            REQUIRE(static_cast<u32>(color->Arg(component).Inst()->Arg(0).Reg()) == 4 + component);
        ++writes;
    }
    }
    REQUIRE(writes == 1);
    REQUIRE(barriers == (strong ? 2U : 0U));
}


TEST_CASE("Native array image stores retain a descriptor bank distinct from texture gathers", "[ampere][fefw]") {
    const std::array<Native, 3> input{{
        {{0x04047b63, 0x31400003, 0x041e0f06, 0}},
        {{0x00007b9b, 0x8080480c, 0x00100f00, 0}},
        VertexTransform.back(),
    }};
    const auto lowered = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(lowered);
    REQUIRE(lowered.required_texture_buffer == 5);
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    // Skii's gather expansion also moves coordinates and results; translate the whole sequence.
    TranslateScalarNative(input, block);
    u32 descriptors{}, stores{};
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetCbufU32) {
            ++descriptors;
            REQUIRE(inst.Arg(0).U32() == 2);
            REQUIRE(inst.Arg(1).U32() == 0x120);
        }
        if (inst.GetOpcode() == IR::Opcode::BindlessImageWrite) {
            ++stores;
            REQUIRE(inst.Flags<IR::TextureInstInfo>().type.Value() == Shader::TextureType::ColorArray2D);
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{250});
            REQUIRE(inst.Arg(1).Inst()->GetOpcode() == IR::Opcode::CompositeConstructU32x3);
            const auto* color = inst.Arg(2).Inst();
            for (u32 i = 0; i < 4; ++i) REQUIRE(color->Arg(i).Inst()->Arg(0).Reg() == IR::Reg{12 + i});
        }
    }
    REQUIRE(descriptors == 1);
    REQUIRE(stores == 1);
}


TEST_CASE("Native high address shifts retain both words and the carry predicate", "[ampere][fefw]") {
    const u32 shift = GENERATE(1U, 4U, 16U, 31U);
    const u32 carry = GENERATE(1U, 7U, 9U, 15U);
    const u32 destination = GENERATE(0U, 2U, 3U, 4U);
    const bool constant = true;
    const std::array<Native, 2> input{{
        {{(destination << 16) | (constant ? 0x7a11U : 0x7211U), constant ? 0x00c04000U : 3U, 0x000f0402U | (shift << 11) | (carry << 23), 0}}, VertexTransform.back(),
    }};
    for (u64 value : {0ULL, 0x12345678abcdef01ULL, 0x80000000ffffffffULL, ~0ULL})
        for (u32 base : {0U, 1U, 0xffffffffU}) for (bool enabled : {false, true}) {
            ScalarNativeMachine machine;
            machine.regs[0] = static_cast<u32>(value);
            machine.regs[2] = static_cast<u32>(value >> 32);
            machine.regs[3] = base;
            machine.constants[(u64{3} << 32) | 0x100] = base;
            machine.predicates = {true, enabled, false, true, false, true, false};
            const auto predicates = machine.predicates;
            const bool increment = ((carry & 7) == 7 ? true : enabled) != bool(carry & 8);
            const u32 expected = static_cast<u32>((value << shift) >> 32) + base + static_cast<u32>(increment);
            ExecutePredicatedScalarNative(input, machine);
            REQUIRE(machine.regs[destination] == expected);
            REQUIRE(machine.predicates == predicates);
        }
}


TEST_CASE("Fire Emblem captured arithmetic and surface forms lower together", "[ampere][fefw]") {
    for (const Native instruction : {
        Native{{0x0f0d8a11, 0x00005900, 0x010f0eff, 0x000fc400}},
        Native{{0x03050a10, 0x00005900, 0x037fe4ff, 0x000fe400}},
        Native{{0x94052a10, 0x00005b00, 0x007fe4ff, 0x000fe400}},
        Native{{0x9e072a10, 0x00005b00, 0x007fe4ff, 0x000fe400}},
        Native{{0xb4059a10, 0x00005b00, 0x027fe4ff, 0x000fe200}},
        Native{{0x6500720c, 0x000000ff, 0x03f05100, 0x000fc400}},
        Native{{0x02007b9b, 0x60005804, 0x0010af00, 0x000fe200}},
        Native{{0x05067827, 0x66666667, 0x078e02ff, 0x000fe200}},
        Native{{0x00097817, 0xffffffc0, 0x07800200, 0x040fe200}},
        Native{{0x020573c4, 0x00000000, 0x00010000, 0x000e6200}},
        Native{{0xff097a19, 0x01003500, 0x00011e0a, 0x000fe200}},
        Native{{0x00022a27, 0x0000a800, 0x078e00ff, 0x000fe200}},
        Native{{0x02031a27, 0x0000a800, 0x078e00ff, 0x000fe200}},
        Native{{0x02040a27, 0x0000b300, 0x078e00ff, 0x000fe200}},
    }) {
        const std::array input{instruction, VertexTransform.back()};
        const auto lowered = Shader::Ampere::LowerToMaxwell(input);
        INFO(lowered.error << " op=" << instruction.Bits(0, 12));
        REQUIRE(lowered);
    }
}

TEST_CASE("Extended equality retains both high and low word decisions", "[ampere][fefw]") {
    for (u32 comparison : {2U, 5U}) for (bool low : {false, true}) {
        const std::array<Native, 2> input{{
            {{0x0100720c, 2, 0x03f00100U | (comparison << 12) | (3U << 4), 0}},
            VertexTransform.back(),
        }};
        for (u32 a : {0U, 1U, 0xffffffffU}) for (u32 b : {0U, 1U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[1] = a; machine.regs[2] = b; machine.predicates[3] = low;
            ExecutePredicatedScalarNative(input, machine);
            REQUIRE(machine.predicates[0] == (comparison == 2 ? a == b && low : a != b || low));
        }
    }
}

TEST_CASE("Mixed-bank image stores protect the full data register range", "[ampere][fefw]") {
    for (const u32 data : {247U, 248U, 249U}) {
        const std::array<Native, 3> input{{
            {{0x04047b63, 0x31400003, 0x041e0f06, 0}},
            {{0x00007b9b, 0x80804800U | data, 0x00100f00, 0}},
            VertexTransform.back(),
        }};
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(input));
    }
}

TEST_CASE("Native bindless dimension queries keep handle mip and overlapping destinations", "[ampere][fefw][fe-next]") {
    const u32 mask = GENERATE(1U, 3U, 8U);
    const std::array<Native, 2> input{{{{0x1c1d7370, 0x08000000, 0xffU | (mask << 8), 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    u32 queries{};
    std::vector<u32> writes, channels;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::BindlessImageQueryDimensions) {
            ++queries;
            REQUIRE(inst.Arg(0).Inst()->Arg(0).Reg() == IR::Reg{28});
            REQUIRE(inst.Arg(1).Inst()->Arg(0).Reg() == IR::Reg{29});
            REQUIRE(inst.Arg(2).U1() == (mask != 8));
        }
        if (inst.GetOpcode() == IR::Opcode::SetRegister) {
            writes.push_back(static_cast<u32>(inst.Arg(0).Reg()));
            channels.push_back(inst.Arg(1).Inst()->Arg(1).U32());
        }
    }
    REQUIRE(queries == 1);
    REQUIRE(writes == (mask == 3 ? std::vector<u32>{29, 30} : std::vector<u32>{29}));
    REQUIRE(channels == (mask == 3 ? std::vector<u32>{0, 1} : mask == 8 ? std::vector<u32>{3} : std::vector<u32>{0}));
}


TEST_CASE("Native shared atomic extrema retain unsigned values and optional old results", "[ampere][fefw][fe-next]") {
    const bool minimum = GENERATE(false, true);
    const bool discard = GENERATE(false, true);
    const std::array<Native, 2> input{{{{discard ? 0xffff738cU : 0xff05738cU, 0x801,
        minimum ? 0x00800000U : 0x01000000U, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    for (u32 old : {0U, 1U, 0x80000000U, 0xffffffffU})
        for (u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.shared[8] = old;
            machine.regs[1] = value;
            machine.regs[5] = 0x12345678;
            machine.Execute(block);
            REQUIRE(machine.shared[8] == (minimum ? std::min(old, value) : std::max(old, value)));
            REQUIRE(machine.regs[1] == value);
            REQUIRE(machine.regs[5] == (discard ? 0x12345678U : old));
        }
    auto guarded = input;
    guarded[0].words[0] = (guarded[0].words[0] & ~0xf000U) | 0x3000;
    const auto lowered = Shader::Ampere::LowerToMaxwell(guarded);
    REQUIRE(lowered);
    REQUIRE(Shader::Maxwell::Instruction{lowered.instructions.front()}.Pred().index == 3);
}


TEST_CASE("Native descriptor-addressed global loads retain the explicit 64-bit pointer", "[ampere][fefw][fe-next]") {
    const u32 width = GENERATE(4U, 5U);
    const std::array<Native, 2> input{{
        {{0x02047981, 0x2008, 0x0c1e1100U | (width << 9), 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[2] = 0xfffffff0; machine.regs[3] = 0x12345678;
    machine.regs[224] = 0xabcdef01; machine.regs[225] = 0x87654321;
    machine.global[0x1234567900000010ULL] = 0x13579bdf;
    machine.global[0x1234567900000014ULL] = 0x2468ace0;
    machine.Execute(block);
    REQUIRE(machine.regs[4] == 0x13579bdfU);
    if (width == 5) REQUIRE(machine.regs[5] == 0x2468ace0U);
    REQUIRE(machine.regs[224] == 0xabcdef01U);
    REQUIRE(machine.regs[225] == 0x87654321U);
}


TEST_CASE("Native global-to-shared copies transfer four words or zero-fill without reading", "[ampere][fefw][fe-next]") {
    const bool zero_fill = GENERATE(false, true);
    const s32 global_offset = GENERATE(0, 32, 512, -16);
    const s32 shared_offset = GENERATE(0, 16, 4096, -16);
    const std::array<Native, 4> input{{
        {{0x02047fae, ((static_cast<u32>(shared_offset / 16) & 0xffffU) << 16) | (static_cast<u32>(global_offset) & 0xfffU),
            0x0f901c08U | (zero_fill ? 0x40U : 0U), 0}},
        {{0x79af, 0, 0, 0}},
        {{0x791a, 0x8000, 0, 0}},
        VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[2] = 0xfffffff0; machine.regs[3] = 1; machine.regs[4] = 64;
    machine.regs[224] = 0xabcdef12; machine.regs[225] = 0x76543210;
    machine.predicates = {true, false, true, false, true, false, true};
    const auto predicates = machine.predicates;
    const std::array<u32, 4> data{0x12345678, 0x9abcdef0, 0x76543210, 0xfedcba98};
    if (!zero_fill) for (u32 component = 0; component < 4; ++component)
        machine.global[0x1fffffff0ULL + global_offset + component * 4] = data[component];
    machine.shared[64 + shared_offset - 4] = 0xabababab;
    machine.shared[64 + shared_offset + 16] = 0xcdcdcdcd;
    machine.Execute(block);
    for (u32 component = 0; component < 4; ++component)
        REQUIRE(machine.shared.at(64 + shared_offset + component * 4) == (zero_fill ? 0U : data[component]));
    REQUIRE(machine.shared.size() == 6);
    REQUIRE(machine.shared.at(64 + shared_offset - 4) == 0xabababab);
    REQUIRE(machine.shared.at(64 + shared_offset + 16) == 0xcdcdcdcd);
    REQUIRE(machine.predicates == predicates);
    REQUIRE(machine.shared_memory_barriers == 3);
    REQUIRE(machine.regs[2] == 0xfffffff0U);
    REQUIRE(machine.regs[3] == 1);
    REQUIRE(machine.regs[4] == 64);
    REQUIRE(machine.regs[224] == 0xabcdef12U);
    REQUIRE(machine.regs[225] == 0x76543210U);
}


TEST_CASE("Native global-to-shared copy retains its source predicate and rejects other modes", "[ampere][fefw][fe-next]") {
    const u32 condition = GENERATE(0U, 4U, 7U, 8U, 12U);
    std::array<Native, 2> input{{
        {{0x02047fae, 0, 0x0c101c08U | ((condition & 7) << 23) | ((condition >> 3) << 6), 0}},
        VertexTransform.back(),
    }};
    const auto result = Shader::Ampere::LowerToMaxwell(input);
    REQUIRE(result);
    u32 loads{};
    for (u64 word : result.instructions) {
        if (Shader::Maxwell::Decode(word) != Shader::Maxwell::Opcode::LDG) continue;
        const auto pred = Shader::Maxwell::Instruction{word}.Pred();
        REQUIRE(pred.index == (condition & 7));
        REQUIRE(pred.negated == bool(condition & 8));
        ++loads;
    }
    REQUIRE(loads == 4);
    auto invalid = input;
    invalid[0].words[0] ^= 0x1000;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = input;
    invalid[0].words[0] |= 1U << 24;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = input;
    invalid[0].words[2] ^= 0x200;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}


TEST_CASE("Native global stores preserve scalar widths paired data and signed displacement", "[ampere][fefw][fe-next]") {
    const u32 width = GENERATE(0U, 2U, 4U, 5U);
    const u32 displacement = GENERATE(0U, 8U, 0xfffff8U);
    const std::array<Native, 2> input{{
        {{0x02007986, (displacement << 8) | 6U, 0x0c101104U | (width << 9), 0}}, VertexTransform.back(),
    }};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.regs[2] = 0xfffffffc; machine.regs[3] = 0x10;
    machine.regs[220] = 0x1000; machine.regs[221] = 2;
    machine.regs[6] = 0x89abcdef; machine.regs[7] = 0x76543210;
    const u64 address = 0x10fffffffcULL +
        static_cast<u64>(static_cast<s64>(static_cast<s32>(displacement << 8) >> 8));
    machine.Execute(block);
    REQUIRE(machine.global.size() == (width == 5 ? 2U : 1U));
    REQUIRE(machine.global.at(address) == (width == 0 ? 0xefU : width == 2 ? 0xcdefU : 0x89abcdefU));
    if (width == 5) {
        REQUIRE(machine.global.at(address + 4) == 0x76543210U);
        auto invalid = input;
        invalid[0].words[1] += 1;
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    }
}


TEST_CASE("Native deferred-blocking barrier remains a full workgroup synchronization", "[ampere][fefw][fe-next]") {
    const u32 mode = GENERATE(0U, 0x10000U);
    const std::array<Native, 2> input{{{{0x7b1d, 0, mode, 0}}, VertexTransform.back()}};
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    TranslateScalarNative(input, block);
    ScalarNativeMachine machine;
    machine.Execute(block);
    REQUIRE(machine.workgroup_barriers == 1);
    auto invalid = input;
    invalid[0].words[1] = 1;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    invalid = input;
    invalid[0].words[2] |= 1;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}


TEST_CASE("Captured uniform carry preserves the register input and predicate result", "[ampere][fefw][fe-next]") {
    const std::array<Native,2> input{{{{0x0c0a7c10,4,0x0ff9e0ff,0}},VertexTransform.back()}};
    for (u32 a : {0U,1U,0x80000000U,0xffffffffU}) for (u32 b : {0U,1U,0xffffffffU}) {
        ScalarNativeMachine machine;
        machine.regs[12]=a;machine.regs[220]=b;
        ExecutePredicatedScalarNative(input,machine);
        REQUIRE(machine.regs[10]==a+b);
        REQUIRE(machine.predicates[4]==(u64{a}+b>0xffffffffULL));
        REQUIRE(machine.regs[220]==b);
    }
}

TEST_CASE("Captured packed dot product retains both packed lanes and accumulator wrap",
          "[ampere][fefw][fe-feedback]") {
    const std::array<Native,2> code{{{{0x02097226,4,0x1006,0}},VertexTransform.back()}};
    for (u32 a : {0U,1U,0x8000ffffU,0xffffffffU})
        for (u32 b : {0U,0xffffU,0x12345678U,0xffffffffU})
            for (u32 c : {0U,1U,0xffffffffU}) {
                ScalarNativeMachine machine;
                machine.regs[2]=a;machine.regs[4]=b;machine.regs[6]=c;
                ExecutePredicatedScalarNative(code,machine);
                const u32 expected=(a&0xffff)*(b&0xff)+(a>>16)*((b>>8)&0xff)+c;
                REQUIRE(machine.regs[9]==expected);
                REQUIRE(machine.regs[2]==a);
                REQUIRE(machine.regs[4]==b);
                REQUIRE(machine.regs[6]==c);
            }
}

TEST_CASE("Signed uniform shifts read the extended uniform bank without aliasing its storage",
          "[ampere][fefw][fe-feedback]") {
    const auto physical=[](u32 reg){return reg<22 ? reg+216 : reg+218;};
    for(u32 source : {16U,21U,22U,27U}) for(u32 shift : {0U,1U,31U,32U})
        for(u32 value : {0U,0x7fffffffU,0x80000000U,0xffffffffU}) {
            const std::array<Native,2> code{{{{0x3f087899,shift,0x08011400U|source,0}},VertexTransform.back()}};
            ScalarNativeMachine machine;
            machine.regs[physical(source)]=value;
            ExecutePredicatedScalarNative(code,machine);
            REQUIRE(machine.regs[224]==static_cast<u32>(static_cast<s32>(value)>>std::min(shift,31U)));
            REQUIRE(machine.regs[physical(source)]==value);
        }
}

TEST_CASE("Captured NaN-propagating floating minimum preserves unordered values",
          "[ampere][fefw][fe-feedback]") {
    const std::array<Native,2> code{{{{0x03037809,0x3f800000,0x03820000,0}},VertexTransform.back()}};
    for(float value : {-2.0f,-0.0f,0.5f,2.0f,std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
        ScalarNativeMachine machine;machine.regs[3]=std::bit_cast<u32>(value);
        ExecutePredicatedScalarNative(code,machine);
        const float actual=std::bit_cast<float>(machine.regs[3]);
        if(std::isnan(value)) REQUIRE(std::isnan(actual));
        else REQUIRE(actual==std::fmin(value,1.0f));
    }
    auto invalid=code;invalid[0].words[2]|=1U<<18;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Captured unit IMAD carry input retains source predicates",
          "[ampere][fefw][fe-feedback]") {
    for(bool carry : {false,true})
        for(u32 a : {0U,1U,0xffffffffU}) for(u32 c : {0U,1U,0xffffffffU}) {
            const std::array<Native,2> code{{{{0x9a2b7824,1,0x008e0603U,0}},VertexTransform.back()}};
            ScalarNativeMachine machine;machine.regs[154]=a;machine.regs[3]=c;machine.predicates[1]=carry;
            const auto predicates=machine.predicates;
            ExecutePredicatedScalarNative(code,machine);
            REQUIRE(machine.regs[43]==a+c+static_cast<u32>(carry));
            REQUIRE(machine.predicates==predicates);
        }
    const std::array<Native,2> unknown{{{{0x9a2b7824,1,0x008e0f03U,0}},VertexTransform.back()}};
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(unknown));
}

TEST_CASE("Signed high effective addresses include their carry input at 32-bit boundaries",
          "[ampere][fefw][fe-feedback]") {
    for(bool carry : {false,true}) for(u32 shift : {0U,1U,7U,31U})
        for(u32 a : {0U,1U,0x80000000U,0xffffffffU}) for(u32 b : {0U,1U,0xffffffffU}) {
            const u32 mode=(0x008f0effU&~(31U<<11))|(shift<<11);
            const std::array<Native,2> code{{{{0x09038211,7,mode,0}},VertexTransform.back()}};
            ScalarNativeMachine machine;machine.regs[9]=a;machine.regs[7]=b;machine.predicates[1]=carry;
            const auto predicates=machine.predicates;
            ExecutePredicatedScalarNative(code,machine);
            const u64 wide=static_cast<u64>(static_cast<s64>(static_cast<s32>(a)))<<shift;
            REQUIRE(machine.regs[3]==static_cast<u32>(wide>>32)+b+static_cast<u32>(carry));
            REQUIRE(machine.predicates==predicates);
        }
}

TEST_CASE("Fire Emblem constant-addend wide multiply preserves full products and aliases",
          "[ampere][fefw][fe-wide]") {
    constexpr std::array<u32, 6> values{0, 1, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff};
    constexpr std::array<u64, 5> addends{0, 1, 0xffffffffULL, 0x80000000ffffffffULL, ~u64{0}};
    for (const bool is_signed : {false, true}) {
        for (const u32 destination : {0U, 8U, 12U, 32U}) {
            const std::array<Native, 2> input{{
                {{(9U << 24) | (destination << 16) | 0x7625U,
                  (3U << 22) | (0x100U << 6), 0x078e000cU | (is_signed ? 0x200U : 0U), 0}},
                VertexTransform.back(),
            }};
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block{pool};
            TranslateScalarNative(input, block);
            for (const u32 a : values) for (const u32 b : values) for (const u64 c : addends) {
                ScalarNativeMachine machine;
                machine.regs[9] = a;
                machine.regs[12] = b;
                machine.constants[(u64{3} << 32) | 0x100] = static_cast<u32>(c);
                machine.constants[(u64{3} << 32) | 0x104] = static_cast<u32>(c >> 32);
                machine.predicates = {true, false, true, false, true, false, true};
                const auto predicates = machine.predicates;
                const u64 expected = c + (is_signed
                    ? static_cast<u64>(static_cast<s64>(static_cast<s32>(a)) * static_cast<s32>(b))
                    : u64{a} * b);
                machine.Execute(block);
                REQUIRE(machine.regs[destination] == static_cast<u32>(expected));
                REQUIRE(machine.regs[destination + 1] == static_cast<u32>(expected >> 32));
                REQUIRE(machine.predicates == predicates);
                if (destination != 8) REQUIRE(machine.regs[9] == a);
                if (destination != 12) REQUIRE(machine.regs[12] == b);
            }
        }
    }
}

TEST_CASE("Fire Emblem wide constant multiply keeps execution guards and rejects invalid pairs",
          "[ampere][fefw][fe-wide]") {
    const std::array<Native, 2> input{{
        {{0x09022625, (3U << 22) | (0x100U << 6), 0x078e000c, 0}},
        VertexTransform.back(),
    }};
    for (const bool enabled : {false, true}) {
        ScalarNativeMachine machine;
        machine.regs[2] = 0x12345678;
        machine.regs[3] = 0x9abcdef0;
        machine.regs[9] = 0xffffffff;
        machine.regs[12] = 0xffffffff;
        machine.predicates[2] = enabled;
        machine.constants[(u64{3} << 32) | 0x100] = 2;
        machine.constants[(u64{3} << 32) | 0x104] = 0;
        ExecutePredicatedScalarNative(input, machine);
        REQUIRE(machine.regs[2] == (enabled ? 3U : 0x12345678U));
        REQUIRE(machine.regs[3] == (enabled ? 0xfffffffeU : 0x9abcdef0U));
        REQUIRE(machine.predicates[2] == enabled);
    }
    for (const u32 bit : {34U, 59U, 60U, 61U, 62U, 63U, 72U, 74U, 91U}) {
        INFO("reserved bit " << bit);
        auto invalid = input;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    }
    auto invalid = input;
    invalid[0].words[0] |= 1U << 16;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
}

TEST_CASE("Fire Emblem constant wide products preserve aliased carry guards",
          "[ampere][fefw][fe_pipeline_math]") {
    const std::array<Native, 2> input{{
        {{0x09022625, (3U << 22) | (0x100U << 6), 0x0784000c, 0}},
        VertexTransform.back()}};
    for (const bool active : {false, true})
        for (const u32 a : {0U, 1U, 0xffffffffU}) for (const u32 b : {0U, 1U, 0xffffffffU})
            for (const u64 c : {0ULL, 1ULL, ~0ULL}) {
                ScalarNativeMachine machine;
                machine.regs[2] = 0x12345678;
                machine.regs[3] = 0x87654321;
                machine.regs[9] = a;
                machine.regs[12] = b;
                machine.predicates[2] = active;
                machine.constants[(u64{3} << 32) | 0x100] = static_cast<u32>(c);
                machine.constants[(u64{3} << 32) | 0x104] = static_cast<u32>(c >> 32);
                const u64 product = u64{a} * b, expected = product + c;
                ExecutePredicatedScalarNative(input, machine);
                REQUIRE(machine.regs[2] == (active ? static_cast<u32>(expected) : 0x12345678U));
                REQUIRE(machine.regs[3] == (active ? static_cast<u32>(expected >> 32) : 0x87654321U));
                REQUIRE(machine.predicates[2] == (active && expected < product));
            }
}

TEST_CASE("Fire Emblem unsigned 64-bit conversion handles NaNs and saturation endpoints",
          "[ampere][fefw][fe-wide]") {
    constexpr std::array<std::pair<u32, u64>, 13> cases{{
        {0x00000000, 0}, {0x80000000, 0}, {0x00000001, 0}, {0x80000001, 0},
        {0x3fc00000, 1}, {0xbfc00000, 0}, {0x4f800000, 0x100000000ULL},
        {0x5f7fffff, 0xffffff0000000000ULL}, {0x5f800000, ~u64{0}},
        {0x7f800000, ~u64{0}}, {0xff800000, 0},
        {0x7fc00000, 0x8000000000000000ULL}, {0xffc00000, 0x8000000000000000ULL},
    }};
    for (const u32 destination : {0U, 2U, 4U}) {
        const std::array<Native, 2> code{{
            {{(destination << 16) | 0x7311, 3, 0x0021d800, 0}}, VertexTransform.back(),
        }};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(code, block);
        for (const auto [bits, expected] : cases) {
            ScalarNativeMachine machine;
            machine.regs[3] = bits;
            machine.Execute(block);
            REQUIRE(machine.regs[destination] == static_cast<u32>(expected));
            REQUIRE(machine.regs[destination + 1] == static_cast<u32>(expected >> 32));
        }
    }
    const std::array<Native, 2> captured{{
        {{0x00047311, 4, 0x0021d800, 0x000e3000}}, VertexTransform.back(),
    }};
    REQUIRE(Shader::Ampere::LowerToMaxwell(captured));
    for (const u32 bit : {16U, 24U, 40U, 72U, 75U, 78U, 80U, 84U}) {
        auto invalid = captured;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(invalid));
    }
}

TEST_CASE("Fire Emblem three-input constant addition retains both carry predicates",
          "[ampere][fefw][fe-wide]") {
    constexpr std::array<u32, 5> values{0, 1, 0x7fffffff, 0x80000000, 0xffffffff};
    for (const u32 destination : {4U, 5U, 44U}) {
        for (const u32 guard : {2U, 3U, 7U, 10U, 11U}) {
            const std::array<Native, 2> code{{
                {{(44U << 24) | (destination << 16) | (guard << 12) | 0xa10U,
                  0x5c00, 0x07b5e005, 0}}, VertexTransform.back(),
            }};
            for (const bool enabled : {false, true}) {
                if (guard == 7 && !enabled) continue;
                for (const u32 a : values) for (const u32 b : values) for (const u32 c : values) {
                    ScalarNativeMachine machine;
                    machine.regs[destination] = 0x12345678;
                    machine.regs[44] = a;
                    machine.regs[5] = c;
                    machine.constants[0x170] = b;
                    machine.predicates = {true, false, true, false, true, false, true};
                    if (guard != 7) machine.predicates[guard & 7] = enabled != ((guard & 8) != 0);
                    const auto predicates = machine.predicates;
                    const u32 original = machine.regs[destination];
                    ExecutePredicatedScalarNative(code, machine);
                    const u64 sum = u64{a} + b + c;
                    REQUIRE(machine.regs[destination] == (enabled ? static_cast<u32>(sum) : original));
                    REQUIRE(machine.predicates[2] == (enabled ? sum >= (1ULL << 32) : predicates[2]));
                    REQUIRE(machine.predicates[3] == (enabled ? sum >= (2ULL << 32) : predicates[3]));
                    for (const u32 p : {0U, 1U, 4U, 5U, 6U}) REQUIRE(machine.predicates[p] == predicates[p]);
                }
            }
        }
    }
}

TEST_CASE("Fire Emblem high uniform LEA retains unsigned high-word addressing",
          "[ampere][fefw][fe-next-forms]") {
    for (const u32 shift : {0U, 1U, 28U, 31U}) {
        const std::array<Native, 2> code{{
            {{0x0a0a7c11, 4, 0x0f8f00ffU | (shift << 11), 0}}, VertexTransform.back(),
        }};
        Shader::ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        TranslateScalarNative(code, block);
        for (const u32 a : {0U, 1U, 0x7fffffffU, 0x80000000U, 0xffffffffU})
            for (const u32 b : {0U, 1U, 0xffffffffU}) {
                ScalarNativeMachine machine;
                machine.regs[10] = a;
                machine.regs[220] = b;
                machine.Execute(block);
                REQUIRE(machine.regs[10] == static_cast<u32>((u64{a} << shift) >> 32) + b);
                REQUIRE(machine.regs[220] == b);
            }
    }
}

TEST_CASE("Fire Emblem zero-product signed IMAD includes its carry input",
          "[ampere][fefw][fe-next-forms]") {
    for (const u32 destination : {3U, 5U}) {
        const std::array<Native, 2> code{{
            {{0xff007224U | (destination << 16), 255, 0x008e0603, 0}}, VertexTransform.back(),
        }};
        for (const bool carry : {false, true}) for (const u32 a : {0U, 1U, 0x7fffffffU, 0xffffffffU}) {
            ScalarNativeMachine machine;
            machine.regs[3] = a;
            machine.predicates[1] = carry;
            const auto predicates = machine.predicates;
            ExecutePredicatedScalarNative(code, machine);
            REQUIRE(machine.regs[destination] == a + static_cast<u32>(carry));
            REQUIRE(machine.predicates == predicates);
        }
    }
}

TEST_CASE("Fire Emblem extended constant addition includes both carry inputs and third source",
          "[ampere][fefw][fe-next-forms]") {
    for (const u32 destination : {3U, 6U, 45U}) for (const u32 invert : {0U, 1U, 2U, 3U}) {
        const std::array<Native, 2> code{{
            {{0x03000a10U | (destination << 16) | (7U << 12), 0x5d00,
              0x017e6406U ^ ((invert & 1) << 26) ^ ((invert >> 1) << 16), 0}},
            VertexTransform.back(),
        }};
        for (const bool first : {false, true}) for (const bool second : {false, true})
            for (const u32 a : {0U, 1U, 0xffffffffU})
                for (const u32 b : {0U, 1U, 0xffffffffU})
                    for (const u32 c : {0U, 1U, 0xffffffffU}) {
                        ScalarNativeMachine machine;
                        machine.regs[3] = a;
                        machine.regs[6] = c;
                        machine.constants[0x174] = b;
                        machine.predicates[2] = first;
                        machine.predicates[3] = second;
                        const auto predicates = machine.predicates;
                        ExecutePredicatedScalarNative(code, machine);
                        REQUIRE(machine.regs[destination] == a + b + c +
                            static_cast<u32>(first != ((invert & 1) != 0)) +
                            static_cast<u32>(second != ((invert & 2) != 0)));
                        REQUIRE(machine.predicates == predicates);
                    }
    }
}

TEST_CASE("Fire Emblem complemented IMAD carry handles negation and aliased destinations",
          "[ampere][fefw][fe_pipeline_math]") {
    for (const u32 destination : {3U, 6U}) for (const u32 guard : {1U, 7U, 9U})
        for (const u32 modifiers : {0U, 1U, 2U, 3U}) {
            const std::array<Native, 2> code{{
                {{0xff000224U | (destination << 16) | (guard << 12), 255,
                  0x008e0603U | ((modifiers & 1) ? 0x800U : 0U) |
                  ((modifiers & 2) ? 0x04000000U : 0U), 0}}, VertexTransform.back()}};
            for (const bool active : {false, true}) {
                if (guard == 7 && !active) continue;
                for (const u32 value : {0U, 1U, 0x80000000U, 0xffffffffU}) {
                    ScalarNativeMachine machine;
                    machine.regs[6] = 0x12345678;
                    machine.regs[3] = value;
                    machine.predicates[1] = active != bool(guard & 8);
                    const auto before = machine.regs[destination];
                    const auto predicates = machine.predicates;
                    const u32 expected = ((modifiers & 1) ? ~value : value) +
                        static_cast<u32>(predicates[1] != bool(modifiers & 2));
                    ExecutePredicatedScalarNative(code, machine);
                    REQUIRE(machine.regs[destination] == (active ? expected : before));
                    REQUIRE(machine.predicates == predicates);
                }
            }
        }
}

TEST_CASE("Fire Emblem register addition preserves negation carries and guard aliases",
          "[ampere][fefw][fe_pipeline_math]") {
    for (const u32 destination : {0U, 5U, 8U}) for (const u32 negate : {0U,1U,2U,3U,4U,5U,6U,7U})
        for (const u32 guard : {2U,3U,7U,10U}) {
            const std::array<Native,2> code{{
                {{(5U<<24)|(destination<<16)|(guard<<12)|0x210U,
                  7U|((negate&2)?0x80000000U:0U),
                  0x07b5e008U|((negate&1)?0x100U:0U)|((negate&4)?0x800U:0U),0}},VertexTransform.back()}};
            for (const bool enabled : {false,true}) {
                if (guard==7&&!enabled) continue;
                for (const u32 a : {0U,1U,0x80000000U,0xffffffffU})
                    for (const u32 b : {0U,1U,0xffffffffU})
                        for (const u32 c : {0U,1U,0xffffffffU}) {
                            ScalarNativeMachine machine;
                            machine.regs[destination]=0xabcdef01;
                            machine.regs[5]=a;machine.regs[7]=b;machine.regs[8]=c;
                            machine.predicates={true,false,true,false,true,false,true};
                            if(guard!=7) machine.predicates[guard&7]=enabled!=bool(guard&8);
                            const auto predicates=machine.predicates;
                            const auto original=machine.regs[destination];
                            const u64 sum=((negate&1)?u64{~a}+1:a)+
                                ((negate&2)?u64{~b}+1:b)+((negate&4)?u64{~c}+1:c);
                            ExecutePredicatedScalarNative(code,machine);
                            REQUIRE(machine.regs[destination]==(enabled?static_cast<u32>(sum):original));
                            REQUIRE(machine.predicates[2]==(enabled?sum>=(1ULL<<32):predicates[2]));
                            REQUIRE(machine.predicates[3]==(enabled?sum>=(2ULL<<32):predicates[3]));
                            for(const u32 p:{0U,1U,4U,5U,6U})REQUIRE(machine.predicates[p]==predicates[p]);
                        }
            }
        }
}

TEST_CASE("Fire Emblem extended register addition uses bitwise complements and both carries",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const u32 invert:{0U,1U,2U,3U,4U,5U,6U,7U})for(const u32 destination:{3U,6U,45U}) {
        const std::array<Native,2> code{{
            {{(3U<<24)|(destination<<16)|0x7210U,5U|((invert&2)?0x80000000U:0U),
              0x017e6406U|((invert&1)?0x100U:0U)|((invert&4)?0x800U:0U),0}},VertexTransform.back()}};
        for(const bool first:{false,true})for(const bool second:{false,true})
            for(const u32 value:{0U,1U,0x80000000U,0xffffffffU}) {
                ScalarNativeMachine machine;machine.regs[3]=value;machine.regs[5]=0xffffffff;
                machine.regs[6]=1;machine.predicates[2]=first;machine.predicates[3]=second;
                const auto predicates=machine.predicates;
                const u32 expected=((invert&1)?~value:value)+((invert&2)?0U:0xffffffffU)+
                    ((invert&4)?~1U:1U)+static_cast<u32>(first)+static_cast<u32>(second);
                ExecutePredicatedScalarNative(code,machine);
                REQUIRE(machine.regs[destination]==expected);
                REQUIRE(machine.predicates==predicates);
            }
    }
}

TEST_CASE("Fire Emblem LEA carry snapshots aliased bases before predicate writes",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const u32 destination:{8U,9U,12U})for(const u32 shift:{0U,1U,4U,31U})
        for(const u32 guard:{2U,7U,10U}) {
            const std::array<Native,2> code{{
                {{(9U<<24)|(destination<<16)|(guard<<12)|0x211U,12,
                  0x078400ffU|(shift<<11),0}},VertexTransform.back()}};
            for(const bool active:{false,true}){
                if(guard==7&&!active)continue;
                for(const u32 a:{0U,1U,0x80000000U,0xffffffffU})for(const u32 b:{0U,1U,0xffffffffU}){
                    ScalarNativeMachine machine;machine.regs[destination]=0x12345678;
                    machine.regs[9]=a;machine.regs[12]=b;
                    machine.predicates[2]=active!=bool(guard&8);
                    const auto before=machine.regs[destination];const bool prior=machine.predicates[2];
                    const u64 sum=u64{static_cast<u32>(u64{a}<<shift)}+b;
                    ExecutePredicatedScalarNative(code,machine);
                    REQUIRE(machine.regs[destination]==(active?static_cast<u32>(sum):before));
                    REQUIRE(machine.predicates[2]==(active?sum>0xffffffffULL:prior));
                }
            }
        }
}

TEST_CASE("Fire Emblem immediate wide products keep signed corrections and aliased addends",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const bool sign:{false,true})for(const u32 destination:{2U,4U,8U})
        for(const u32 multiplier:{0U,1U,16U,0x10000U,0xffffffffU}){
            const std::array<Native,2> code{{
                {{(9U<<24)|(destination<<16)|0x7825, multiplier,0x078e0004U|(sign?0x200U:0U),0}},VertexTransform.back()}};
            Shader::ObjectPool<IR::Inst> pool;IR::Block block{pool};TranslateScalarNative(code,block);
            for(const u32 a:{0U,1U,0x80000000U,0xffffffffU})for(const u64 c:{0ULL,1ULL,0xffffffffULL,~0ULL}){
                ScalarNativeMachine machine;machine.regs[9]=a;machine.regs[4]=static_cast<u32>(c);machine.regs[5]=static_cast<u32>(c>>32);
                machine.Execute(block);
                const u64 expected=c+(sign?static_cast<u64>(static_cast<s64>(static_cast<s32>(a))*static_cast<s32>(multiplier)):u64{a}*multiplier);
                REQUIRE(machine.regs[destination]==static_cast<u32>(expected));
                REQUIRE(machine.regs[destination+1]==static_cast<u32>(expected>>32));
            }
        }
}

TEST_CASE("Fire Emblem unsigned 64-bit integer conversion snapshots the source pair",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const u32 destination:{0U,2U,3U}){
        const std::array<Native,2> code{{{{(destination<<16)|0x7312,2,0x00301000,0}},VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;IR::Block block{pool};TranslateScalarNative(code,block);
        for(const u64 value:{0ULL,1ULL,0xffffffffULL,0x100000001ULL,0x7fffffffffffffffULL,~0ULL}){
            ScalarNativeMachine machine;machine.regs[2]=static_cast<u32>(value);machine.regs[3]=static_cast<u32>(value>>32);
            machine.Execute(block);
            REQUIRE(machine.regs[destination]==std::bit_cast<u32>(static_cast<float>(value)));
        }
    }
}

TEST_CASE("Fire Emblem extended high LEA includes the high register and preserves predicates",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const u32 destination:{2U,3U,4U})for(const u32 shift:{0U,1U,4U,31U})
        for(const bool carry:{false,true}){
            const std::array<Native,2> code{{{{(2U<<24)|(destination<<16)|0x7211,3,0x000f0404U|(shift<<11),0}},VertexTransform.back()}};
            for(const u32 low:{0U,1U,0x80000000U,0xffffffffU})for(const u32 high:{0U,1U,0xffffffffU}){
                ScalarNativeMachine machine;machine.regs[2]=low;machine.regs[3]=0xffffffff;machine.regs[4]=high;machine.predicates[0]=carry;
                const auto predicates=machine.predicates;ExecutePredicatedScalarNative(code,machine);
                const u32 shifted=static_cast<u32>(((u64{high}<<32|low)<<shift)>>32);
                REQUIRE(machine.regs[destination]==shifted+0xffffffffU+static_cast<u32>(carry));
                REQUIRE(machine.predicates==predicates);
            }
        }
}

TEST_CASE("Fire Emblem directed U64 conversions retain representable bounds",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const u32 rounding:{0U,1U,2U,3U}){
        const std::array<Native,2> code{{{{0x00057312,4,0x00301000U|(rounding<<14),0}},VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;IR::Block block{pool};TranslateScalarNative(code,block);
        for(const u64 value:{0ULL,1ULL,0xffffffULL,0x1000001ULL,0xffffffffULL,0x20000000000001ULL,0x7fffffffffffffffULL,0x8000000000000001ULL,~0ULL}){
            float expected=static_cast<float>(value);
            if(rounding!=0){
                const bool above=expected>=0x1p64f || static_cast<u64>(expected)>value;
                const bool below=expected<0x1p64f && static_cast<u64>(expected)<value;
                if(rounding==2&&below)expected=std::nextafter(expected,std::numeric_limits<float>::infinity());
                if((rounding==1||rounding==3)&&above)expected=std::nextafter(expected,0.0f);
            }
            ScalarNativeMachine machine;machine.regs[4]=static_cast<u32>(value);machine.regs[5]=static_cast<u32>(value>>32);
            machine.Execute(block);
            REQUIRE(machine.regs[5]==std::bit_cast<u32>(expected));
        }
    }
}

TEST_CASE("Fire Emblem paired register negation forwards the zero carry",
          "[ampere][fefw][fe_pipeline_math]") {
    const std::array<Native,3> code{{
        {{0xff0b7210,0x80000008,0x07f1e0ff,0}},
        {{0xff0d7210,0x80000009,0x007fe4ff,0}},VertexTransform.back()}};
    for(const u64 value:{0ULL,1ULL,0x100000000ULL,0x7fffffffffffffffULL,0x8000000000000000ULL,~0ULL}){
        ScalarNativeMachine machine;machine.regs[8]=static_cast<u32>(value);machine.regs[9]=static_cast<u32>(value>>32);
        ExecutePredicatedScalarNative(code,machine);
        const u64 expected=0ULL-value;
        REQUIRE(machine.regs[11]==static_cast<u32>(expected));
        REQUIRE(machine.regs[13]==static_cast<u32>(expected>>32));
    }
}

TEST_CASE("Fire Emblem unsigned high products preserve aliased inputs",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const u32 destination:{0U,9U,12U}){
        const std::array<Native,2> code{{{{(9U<<24)|(destination<<16)|0x7227,12,0x078e00ff,0}},VertexTransform.back()}};
        Shader::ObjectPool<IR::Inst> pool;IR::Block block{pool};TranslateScalarNative(code,block);
        for(const u32 a:{0U,1U,0x7fffffffU,0x80000000U,0xffffffffU})for(const u32 b:{0U,1U,0x7fffffffU,0x80000000U,0xffffffffU}){
            ScalarNativeMachine machine;machine.regs[9]=a;machine.regs[12]=b;machine.Execute(block);
            REQUIRE(machine.regs[destination]==static_cast<u32>((u64{a}*b)>>32));
        }
    }
}

TEST_CASE("Fire Emblem wide and high products retain the 65th-bit carry",
          "[ampere][fefw][fe_pipeline_math]") {
    for(const bool high_only:{false,true})for(const u32 destination:{2U,4U,8U,12U})
        for(const u32 guard:{2U,7U,10U}){
            const std::array<Native,2> code{{
                {{(9U<<24)|(destination<<16)|(guard<<12)|(high_only?0x227U:0x225U),12,0x07840004,0}},VertexTransform.back()}};
            for(const bool active:{false,true}){
                if(guard==7&&!active)continue;
                for(const u32 a:{0U,1U,0xffffffffU})for(const u32 b:{0U,1U,0xffffffffU})
                    for(const u64 c:{0ULL,1ULL,0xffffffffULL,~0ULL}){
                        ScalarNativeMachine machine;machine.regs[destination]=0x12345678;
                        if(!high_only)machine.regs[destination+1]=0x87654321;
                        machine.regs[9]=a;machine.regs[12]=b;machine.regs[4]=static_cast<u32>(c);machine.regs[5]=static_cast<u32>(c>>32);
                        machine.predicates[2]=active!=bool(guard&8);const bool prior=machine.predicates[2];
                        const auto low=machine.regs[destination],high=machine.regs[destination+1];
                        const u64 product=u64{a}*b,expected=product+c;
                        ExecutePredicatedScalarNative(code,machine);
                        REQUIRE(machine.regs[destination]==(active?static_cast<u32>(high_only?expected>>32:expected):low));
                        if(!high_only)REQUIRE(machine.regs[destination+1]==(active?static_cast<u32>(expected>>32):high));
                        REQUIRE(machine.predicates[2]==(active?expected<product:prior));
                    }
            }
        }
}

TEST_CASE("Fire Emblem immediate IMAD carry complements the addend before addition",
          "[ampere][fefw][fe_pipeline_math]") {
    for (const u32 destination : {2U, 7U, 9U}) for (const u32 modifiers : {0U, 1U, 2U, 3U}) {
        const std::array<Native, 2> code{{
            {{(7U << 24) | (destination << 16) | 0x7824U, 1,
              0x010e0602U | ((modifiers & 1) ? 0x800U : 0U) |
              ((modifiers & 2) ? 0x04000000U : 0U), 0}}, VertexTransform.back()}};
        for (const bool carry : {false, true})
            for (const u32 a : {0U, 1U, 0xffffffffU})
                for (const u32 c : {0U, 1U, 0x80000000U, 0xffffffffU}) {
                    ScalarNativeMachine machine;
                    machine.regs[7] = a;
                    machine.regs[2] = c;
                    machine.predicates[2] = carry;
                    const auto predicates = machine.predicates;
                    ExecutePredicatedScalarNative(code, machine);
                    REQUIRE(machine.regs[destination] == a + ((modifiers & 1) ? ~c : c) +
                        static_cast<u32>(carry != bool(modifiers & 2)));
                    REQUIRE(machine.predicates == predicates);
                }
    }
}
