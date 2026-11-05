// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostics, not run by default: tests.exe "[.spirvprobe]" with SATSUMA_SPIRV_DUMP set to an
// EDEN_NX2_SHADER_DUMP folder (native *-cs-*.bin / *-gfx-*.bin). Each compute program is lowered,
// translated and emitted to SPIR-V as the Vulkan pipeline cache does, and the first error printed.
// SATSUMA_SPIRV_FILTER=<text> keeps only file names containing it; SATSUMA_SPIRV_IR=<folder> writes
// each translated program's IR there.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"

namespace {
class LoweredDumpEnvironment final : public Shader::Environment {
public:
    explicit LoweredDumpEnvironment(std::vector<u64> code_) : code{std::move(code_)} {
        stage = Shader::Stage::Compute;
        start_address = 0x50;
    }
    u64 ReadInstruction(u32 address) override {
        const size_t index = address / 8;
        return index < code.size() ? code[index] : 0;
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) {
            return *translated;
        }
        return address;
    }
    u32 NativeProgramSize() const override {
        return Shader::Ampere::NativeProgramSize(code);
    }
    bool IsNativeProgram() const override { return true; }
    bool HasNativeInstructions() const override { return true; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0x1000; }
    u32 SharedMemorySize() const override { return 0x8000; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::vector<u64> code;
};

// The lowered program laid out as ShaderEnvironment does: a 0x50-byte header, a scheduling word
// at every 32-byte boundary, a padding word, then the native address map.
std::vector<u64> LayOut(const Shader::Ampere::LoweredProgram& lowered) {
    std::vector<u64> code(0x50 / sizeof(u64), 0);
    for (const u64 instruction : lowered.instructions) {
        if ((code.size() * sizeof(u64)) % 32 == 0) {
            code.push_back(0);
        }
        code.push_back(instruction);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    return code;
}

// An NVIDIA desktop GPU, as the Vulkan pipeline cache builds the profile on Fil's machine.
Shader::Profile ProbeProfile() {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.unified_descriptor_binding = true;
    profile.support_int8 = true;
    profile.support_uniform_and_storage_buffer_8bit = true;
    profile.support_storage_buffer_8bit = true;
    profile.support_int16 = true;
    profile.support_uniform_and_storage_buffer_16bit = true;
    profile.support_storage_buffer_16bit = true;
    profile.support_int64 = true;
    profile.support_float_controls = true;
    profile.support_vote = true;
    profile.support_shuffle_relative = true;
    profile.support_quad_shuffles = true;
    profile.supported_subgroup_stages = 0x7f;
    profile.support_typeless_image_loads = true;
    profile.support_demote_to_helper_invocation = true;
    profile.support_int64_atomics = true;
    profile.support_shared_int64_atomics = true;
    profile.support_derivative_control = true;
    profile.support_sampled_image_array_nonuniform_indexing = true;
    profile.support_storage_image_array_nonuniform_indexing = true;
    profile.max_subgroup_size = 32;
    profile.min_ssbo_alignment = 16;
    return profile;
}

Shader::HostTranslateInfo ProbeHostInfo() {
    Shader::HostTranslateInfo info{};
    info.min_ssbo_alignment = 16;
    info.support_float64 = true;
    info.support_float16 = true;
    info.support_int64 = true;
    info.support_snorm_render_buffer = true;
    info.support_conditional_barrier = true;
    info.support_dynamic_texture_handles = true;
    info.ApplyDescriptorLimitPolicy();
    return info;
}
} // Anonymous namespace

TEST_CASE("Native compute programs reach SPIR-V", "[.spirvprobe]") {
    const char* folder = std::getenv("SATSUMA_SPIRV_DUMP");
    REQUIRE(folder != nullptr);
    const char* filter = std::getenv("SATSUMA_SPIRV_FILTER");
    const auto profile = ProbeProfile();
    const auto host_info = ProbeHostInfo();
    int ok = 0, failed = 0, rejected = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".bin" || name.find("-cs-") == std::string::npos ||
            name.find(".mapped") != std::string::npos) {
            continue;
        }
        if (filter && name.find(filter) == std::string::npos) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        std::vector<Shader::Ampere::Instruction> native(bytes.size() /
                                                        sizeof(Shader::Ampere::Instruction));
        std::memcpy(native.data(), bytes.data(), native.size() * sizeof(Shader::Ampere::Instruction));
        const auto lowered = Shader::Ampere::LowerToMaxwell(native, false, {}, true, true, true);
        if (!lowered) {
            ++rejected;
            std::printf("LOWER FAIL %s @%#x: %.*s\n", name.c_str(), lowered.error_offset,
                        static_cast<int>(lowered.error.size()), lowered.error.data());
            continue;
        }
        try {
            LoweredDumpEnvironment env{LayOut(lowered)};
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
            Shader::ObjectPool<Shader::IR::Inst> inst_pool;
            Shader::ObjectPool<Shader::IR::Block> block_pool;
            Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
            auto program =
                Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info);
            if (const char* ir_folder = std::getenv("SATSUMA_SPIRV_IR")) {
                std::ofstream{std::filesystem::path{ir_folder} / (name + ".ir.txt")}
                    << Shader::IR::DumpProgram(program);
            }
            const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
            ++ok;
            if (std::getenv("SATSUMA_SPIRV_VERBOSE")) {
                std::printf("OK         %s (%zu words)\n", name.c_str(), spirv.size());
            }
        } catch (const std::exception& e) {
            ++failed;
            std::printf("SPIRV FAIL %s: %s\n", name.c_str(), e.what());
        }
    }
    std::printf("spirv ok=%d failed=%d lowering rejected=%d\n", ok, failed, rejected);
}
