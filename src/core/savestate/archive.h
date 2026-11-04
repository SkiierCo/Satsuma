// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <iosfwd>
#include <span>
#include <stdexcept>
#include <vector>

#include "common/common_types.h"

namespace Core::SaveState {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Explicit little-endian fields, independent of host pointers, padding and compiler ABI.
class Encoder {
public:
    void U32(u32 value);
    void U64(u64 value);
    void Bytes(std::span<const u8> value);
    std::vector<u8> data;
};

class Decoder {
public:
    explicit Decoder(std::span<const u8> data_) : data{data_} {}
    u32 U32();
    u64 U64();
    std::span<const u8> Bytes(size_t size);
    void Finish() const;

private:
    std::span<const u8> data;
};

enum class Component : u32 {
    Cpu = 1,
    Memory,
    Kernel,
    Services,
    Gpu,
    Audio,
    Timing,
};

struct Identity {
    u64 program_id{};
    std::array<u8, 32> program_hash{};
    std::array<u8, 32> configuration_hash{};
    auto operator<=>(const Identity&) const = default;
};

// Component schemas, rather than the executable's build tag, control compatibility.
inline constexpr u32 FormatVersion = 1;
inline constexpr u32 ComponentVersion = 1;
inline constexpr u32 MaxChunkBytes = 16 * 1024 * 1024;
inline constexpr u32 MaxChunks = 65536;
inline constexpr u64 MaxStateBytes = 64ULL * 1024 * 1024 * 1024;

class ArchiveWriter {
public:
    ArchiveWriter(std::ostream& stream_, const Identity& identity);
    void Add(Component component, std::span<const u8> bytes);
    // All seven components must have been supplied. Does not publish or replace a state file.
    void Finish();

private:
    std::ostream& stream;
    std::array<u32, 7> counts{};
    Encoder integrity;
    u64 total_bytes{};
    u32 chunks{};
    bool finished{};
};

struct Chunk {
    Component component;
    u32 index;
    u32 raw_size;
    u32 packed_size;
    u64 offset;
    std::array<u8, 32> digest;
};

class ArchiveReader {
public:
    // Preflights the entire archive before a caller may change emulation state. The caller must
    // keep the seekable stream open and immutable until restore has completed.
    ArchiveReader(std::istream& stream_, const Identity& expected);
    const std::vector<Chunk>& Chunks() const {
        return chunks;
    }
    std::vector<u8> Read(const Chunk& chunk);

private:
    std::istream& stream;
    std::vector<Chunk> chunks;
};

} // namespace Core::SaveState
