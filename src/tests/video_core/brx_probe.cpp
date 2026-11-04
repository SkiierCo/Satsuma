// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostics, not run by default: tests.exe "[.brxprobe]" with SATSUMA_BRX_DUMP set to a folder
// of dumped lowered programs (*.ash). For each BRX, prints the instructions before it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/indirect_branch_table_track.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"

TEST_CASE("Lowered BRX neighbourhoods", "[.brxprobe]") {
    const char* folder = std::getenv("SATSUMA_BRX_DUMP");
    REQUIRE(folder != nullptr);
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() != ".ash") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        std::vector<u64> code(bytes.size() / 8);
        std::memcpy(code.data(), bytes.data(), code.size() * 8);
        for (size_t i = 0; i < code.size(); ++i) {
            if (i % 4 == 2) continue; // .ash starts at 0x50: control words sit at 32-byte boundaries
            Shader::Maxwell::Opcode opcode;
            try {
                opcode = Shader::Maxwell::Decode(code[i]);
            } catch (...) {
                continue;
            }
            if (opcode != Shader::Maxwell::Opcode::BRX) continue;
            std::printf("%s @%#zx BRX src R%u offset %lld\n", entry.path().filename().string().c_str(),
                        0x50 + i * 8, static_cast<unsigned>((code[i] >> 8) & 0xff),
                        static_cast<long long>(static_cast<s64>(code[i] << 20) >> 40));
            for (size_t j = i > 24 ? i - 24 : 0; j < i; ++j) {
                if (j % 4 == 2) continue;
                const char* name = "?";
                try {
                    name = Shader::Maxwell::NameOf(Shader::Maxwell::Decode(code[j]));
                } catch (...) {
                }
                std::printf("  %#zx %016llx %-12s d=R%u a=R%u b=R%u\n", 0x50 + j * 8,
                            static_cast<unsigned long long>(code[j]), name,
                            static_cast<unsigned>(code[j] & 0xff),
                            static_cast<unsigned>((code[j] >> 8) & 0xff),
                            static_cast<unsigned>((code[j] >> 20) & 0xff));
            }
            break;
        }
    }
}

namespace {
class DumpEnvironment final : public Shader::Environment {
public:
    explicit DumpEnvironment(std::vector<u64> code_) : code{std::move(code_)} {}
    u64 ReadInstruction(u32 address) override {
        const size_t index = (address - 0x50) / 8;
        return address >= 0x50 && index < code.size() ? code[index] : 0;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
    std::vector<u64> code;
};
} // Anonymous namespace

TEST_CASE("Lowered BRX tracking", "[.brxprobe]") {
    const char* folder = std::getenv("SATSUMA_BRX_DUMP");
    REQUIRE(folder != nullptr);
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() != ".ash") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        std::vector<u64> code(bytes.size() / 8);
        std::memcpy(code.data(), bytes.data(), code.size() * 8);
        DumpEnvironment env{code};
        for (size_t i = 0; i < code.size(); ++i) {
            if (i % 4 == 2) continue;
            bool brx = false;
            try {
                brx = Shader::Maxwell::Decode(code[i]) == Shader::Maxwell::Opcode::BRX;
            } catch (...) {
            }
            if (!brx) continue;
            const u32 address = static_cast<u32>(0x50 + i * 8);
            const auto table = Shader::Maxwell::TrackIndirectBranchTable(
                env, Shader::Maxwell::Location{address}, Shader::Maxwell::Location{0x50});
            if (table) {
                std::printf("TRACK OK   %s @%#x c%u[%#x] %u entries\n",
                            entry.path().filename().string().c_str(), address, table->cbuf_index,
                            table->cbuf_offset, table->num_entries);
            } else {
                std::printf("TRACK FAIL %s @%#x\n", entry.path().filename().string().c_str(),
                            address);
            }
        }
    }
}
