// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <istream>
#include <memory>
#include <ostream>

#include <openssl/evp.h>
#include <zstd.h>

#include "core/savestate/archive.h"

namespace Core::SaveState {
namespace {
constexpr std::array<u8, 8> Magic{'E', 'G', 'S', 'T', 'A', 'T', 'E', 0};
constexpr size_t HeaderSize = 88;
constexpr size_t RecordSize = 24;
constexpr u32 MaxPackedBytes = MaxChunkBytes + 128 * 1024;

std::array<u8, 32> Hash(std::span<const u8> bytes) {
    std::array<u8, 32> result{};
    unsigned int length{};
    if (EVP_Digest(bytes.data(), bytes.size(), result.data(), &length, EVP_sha256(), nullptr) !=
            1 ||
        length != result.size()) {
        throw Error{"State checksum failed"};
    }
    return result;
}

void WriteBytes(std::ostream& stream, std::span<const u8> bytes) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!stream) {
        throw Error{"State write failed"};
    }
}

std::vector<u8> ReadBytes(std::istream& stream, size_t size) {
    std::vector<u8> bytes(size);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        throw Error{"State is truncated or unreadable"};
    }
    return bytes;
}

size_t ComponentIndex(Component component) {
    const u32 number = static_cast<u32>(component);
    if (number < 1 || number > 7) {
        throw Error{"Unknown state component"};
    }
    return number - 1;
}

void CheckComplete(const std::array<u32, 7>& counts) {
    if (std::ranges::any_of(counts, [](u32 count) { return count == 0; })) {
        throw Error{"State is missing a machine component"};
    }
}

Encoder Record(u32 component, u32 version, u32 index, u32 raw_size, u32 packed_size) {
    Encoder record;
    for (const u32 value : {component, version, index, raw_size, packed_size, 0U}) {
        record.U32(value);
    }
    return record;
}

std::vector<u8> Unpack(std::span<const u8> packed, u32 raw_size) {
    if (ZSTD_getFrameContentSize(packed.data(), packed.size()) != raw_size ||
        ZSTD_findFrameCompressedSize(packed.data(), packed.size()) != packed.size()) {
        throw Error{"Invalid state compression frame"};
    }
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{ZSTD_createDCtx(), ZSTD_freeDCtx};
    if (!context || ZSTD_isError(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 24))) {
        throw Error{"State decompressor initialization failed"};
    }
    std::vector<u8> bytes(raw_size);
    const size_t result = ZSTD_decompressDCtx(context.get(), bytes.data(), bytes.size(),
                                              packed.data(), packed.size());
    if (ZSTD_isError(result) || result != raw_size) {
        throw Error{"Invalid compressed state data"};
    }
    return bytes;
}
} // namespace

void Encoder::U32(u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        data.push_back(static_cast<u8>(value >> shift));
    }
}

void Encoder::U64(u64 value) {
    U32(static_cast<u32>(value));
    U32(static_cast<u32>(value >> 32));
}

void Encoder::Bytes(std::span<const u8> value) {
    data.insert(data.end(), value.begin(), value.end());
}

std::span<const u8> Decoder::Bytes(size_t size) {
    if (size > data.size()) {
        throw Error{"State field is truncated"};
    }
    const auto result = data.first(size);
    data = data.subspan(size);
    return result;
}

u32 Decoder::U32() {
    const auto bytes = Bytes(4);
    return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) | (u32{bytes[3]} << 24);
}

u64 Decoder::U64() {
    const u64 low = U32();
    return low | (u64{U32()} << 32);
}

void Decoder::Finish() const {
    if (!data.empty()) {
        throw Error{"State field has unexpected trailing data"};
    }
}

ArchiveWriter::ArchiveWriter(std::ostream& stream_, const Identity& identity) : stream{stream_} {
    Encoder header;
    header.Bytes(Magic);
    header.U32(FormatVersion);
    header.U32(0);
    header.U64(identity.program_id);
    header.Bytes(identity.program_hash);
    header.Bytes(identity.configuration_hash);
    const auto digest = Hash(header.data);
    WriteBytes(stream, header.data);
    WriteBytes(stream, digest);
    integrity.Bytes(digest);
}

void ArchiveWriter::Add(Component component, std::span<const u8> bytes) {
    if (finished || chunks == MaxChunks || bytes.size() > MaxChunkBytes ||
        bytes.size() > MaxStateBytes - total_bytes) {
        throw Error{"State chunk exceeds format limits or archive is closed"};
    }
    auto& count = counts[ComponentIndex(component)];
    std::vector<u8> packed(ZSTD_compressBound(bytes.size()));
    const size_t size = ZSTD_compress(packed.data(), packed.size(), bytes.data(), bytes.size(), 1);
    if (ZSTD_isError(size) || size > MaxPackedBytes) {
        throw Error{"State compression failed"};
    }
    packed.resize(size);
    auto record = Record(static_cast<u32>(component), ComponentVersion, count,
                         static_cast<u32>(bytes.size()), static_cast<u32>(packed.size()));
    Encoder checked = record;
    checked.Bytes(packed);
    const auto digest = Hash(checked.data);
    WriteBytes(stream, record.data);
    WriteBytes(stream, digest);
    WriteBytes(stream, packed);
    integrity.Bytes(digest);
    total_bytes += bytes.size();
    ++count;
    ++chunks;
}

void ArchiveWriter::Finish() {
    if (finished) {
        throw Error{"State archive is already closed"};
    }
    CheckComplete(counts);
    const auto end = Record(0, FormatVersion, chunks, 0, 0);
    integrity.Bytes(end.data);
    WriteBytes(stream, end.data);
    WriteBytes(stream, Hash(integrity.data));
    stream.flush();
    if (!stream) {
        throw Error{"State flush failed"};
    }
    finished = true;
}

ArchiveReader::ArchiveReader(std::istream& stream_, const Identity& expected) : stream{stream_} {
    const auto header = ReadBytes(stream, HeaderSize);
    const auto digest = Hash(header);
    if (!std::ranges::equal(digest, ReadBytes(stream, digest.size()))) {
        throw Error{"State header checksum mismatch"};
    }
    Decoder fields{header};
    if (!std::ranges::equal(fields.Bytes(Magic.size()), Magic) || fields.U32() != FormatVersion ||
        fields.U32() != 0) {
        throw Error{"Unsupported state format"};
    }
    Identity identity;
    identity.program_id = fields.U64();
    std::ranges::copy(fields.Bytes(32), identity.program_hash.begin());
    std::ranges::copy(fields.Bytes(32), identity.configuration_hash.begin());
    fields.Finish();
    if (identity != expected) {
        throw Error{"State belongs to a different game, game update or configuration"};
    }
    std::array<u32, 7> counts{};
    Encoder integrity;
    integrity.Bytes(digest);
    u64 total_bytes{};
    for (;;) {
        const auto record = ReadBytes(stream, RecordSize);
        Decoder item{record};
        const u32 component = item.U32();
        const u32 version = item.U32();
        const u32 index = item.U32();
        const u32 raw_size = item.U32();
        const u32 packed_size = item.U32();
        if (item.U32() != 0) {
            throw Error{"Unsupported state record flags"};
        }
        const auto checksum = ReadBytes(stream, 32);
        if (component == 0) {
            CheckComplete(counts);
            integrity.Bytes(record);
            if (version != FormatVersion || index != chunks.size() || raw_size != 0 ||
                packed_size != 0 || !std::ranges::equal(checksum, Hash(integrity.data))) {
                throw Error{"State completion record is invalid"};
            }
            if (stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
                throw Error{"Unexpected data after state completion"};
            }
            stream.clear();
            return;
        }
        auto& count = counts[ComponentIndex(static_cast<Component>(component))];
        if (version != ComponentVersion || index != count || chunks.size() == MaxChunks ||
            raw_size > MaxChunkBytes || packed_size > MaxPackedBytes || packed_size == 0 ||
            raw_size > MaxStateBytes - total_bytes) {
            throw Error{"Unsupported state component version, sequence or size"};
        }
        const auto offset = stream.tellg();
        if (offset < 0) {
            throw Error{"State stream is not seekable"};
        }
        const auto packed = ReadBytes(stream, packed_size);
        Encoder checked;
        checked.Bytes(record);
        checked.Bytes(packed);
        const auto actual = Hash(checked.data);
        if (!std::ranges::equal(checksum, actual)) {
            throw Error{"State chunk checksum mismatch"};
        }
        Unpack(packed, raw_size);
        chunks.push_back({static_cast<Component>(component), index, raw_size, packed_size,
                          static_cast<u64>(offset), actual});
        integrity.Bytes(actual);
        total_bytes += raw_size;
        ++count;
    }
}

std::vector<u8> ArchiveReader::Read(const Chunk& chunk) {
    // Recheck each chunk in case the backing file changed after preflight.
    stream.clear();
    stream.seekg(chunk.offset);
    if (!stream) {
        throw Error{"State seek failed"};
    }
    if (chunk.raw_size > MaxChunkBytes || chunk.packed_size > MaxPackedBytes) {
        throw Error{"State chunk exceeds format limits"};
    }
    const auto packed = ReadBytes(stream, chunk.packed_size);
    auto checked = Record(static_cast<u32>(chunk.component), ComponentVersion, chunk.index,
                          chunk.raw_size, chunk.packed_size);
    checked.Bytes(packed);
    if (Hash(checked.data) != chunk.digest) {
        throw Error{"State chunk changed after validation"};
    }
    return Unpack(packed, chunk.raw_size);
}

} // namespace Core::SaveState
