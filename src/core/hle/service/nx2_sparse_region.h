// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/div_ceil.h"

namespace Service {

struct Nx2SparseRegion {
    static constexpr u64 BlockSize = 4ULL << 20;
    static constexpr u64 PageSize = 0x1000;

    /// Written: a block of a region the game maps its own tiles into (nx2:gpu 216) that holds
    /// data, from a tile or a write. It never gets memory of its own.
    enum class Block : u8 { Empty, Backed, Failed, Pending, Written };

    struct PendingBlock {
        u64 index;
        u64 generation;
        u64 size;
    };

    Nx2SparseRegion(u64 size_, u64 generation_)
        : size{size_}, blocks(Common::DivCeil(size, BlockSize)), generation{generation_} {}

    std::optional<PendingBlock> BeginBacking(u64 block) {
        if (tiles || block >= blocks.size() || blocks[block] != Block::Empty) {
            return std::nullopt;
        }
        blocks[block] = Block::Pending;
        return PendingBlock{block, generation, (std::min)(BlockSize, size - block * BlockSize)};
    }

    bool IsPending(const PendingBlock& pending) const {
        return generation == pending.generation && pending.index < blocks.size() &&
               blocks[pending.index] == Block::Pending;
    }

    bool FinishBacking(const PendingBlock& pending, bool success) {
        if (!IsPending(pending)) {
            return false;
        }
        blocks[pending.index] = success ? Block::Backed : Block::Failed;
        return true;
    }

    /// The game started mapping tiles while the block was being backed: it gets no memory and
    /// counts as written. False when the block is no longer pending.
    bool AbandonBacking(const PendingBlock& pending) {
        if (!IsPending(pending)) {
            return false;
        }
        blocks[pending.index] = Block::Written;
        return true;
    }

    /// A GPU write reaches a block of a region with tiles: it lands in the game's memory (a tile
    /// it mapped) or nowhere. True when the block counts as written only now.
    bool NoteTileWrite(u64 block) {
        if (!tiles || block >= blocks.size() || blocks[block] != Block::Empty) {
            return false;
        }
        blocks[block] = Block::Written;
        return true;
    }

    /// The game maps its own memory into the region (216): GPU writes outside its tiles are
    /// dropped from now on, as on the console. Returns the blocks backed before, which now count
    /// as written and whose memory the caller gives back. A pending block stays pending; its
    /// backing resolves it (AbandonBacking).
    std::vector<u64> StartTiles() {
        std::vector<u64> released;
        if (tiles) {
            return released;
        }
        tiles = true;
        tile_pages.assign(Common::DivCeil(size, PageSize), false);
        for (u64 block = 0; block < blocks.size(); ++block) {
            if (blocks[block] == Block::Backed) {
                blocks[block] = Block::Written;
                released.push_back(block);
            }
        }
        return released;
    }

    /// A tile maps [offset, +length) of the region: its blocks count as written (the game's memory
    /// already holds what the tile shows). Returns the first and last block it touches.
    std::pair<u64, u64> MarkMapped(u64 offset, u64 length) {
        const u64 first = offset / BlockSize;
        const u64 last = (std::min<u64>)((offset + length - 1) / BlockSize, blocks.size() - 1);
        for (u64 block = first; block <= last; ++block) {
            if (blocks[block] == Block::Empty) {
                blocks[block] = Block::Written;
            }
        }
        return {first, last};
    }

    /// Records the pages [offset, +length) a tile maps (mapped) or no longer maps. Returns the
    /// change in bytes mapped.
    s64 MarkTiles(u64 offset, u64 length, bool mapped) {
        if (!tiles || length == 0) {
            return 0;
        }
        s64 delta = 0;
        const u64 last = (std::min)(Common::DivCeil(offset + length, PageSize),
                                    static_cast<u64>(tile_pages.size()));
        for (u64 page = offset / PageSize; page < last; ++page) {
            if (tile_pages[page] != mapped) {
                tile_pages[page] = mapped;
                delta += mapped ? static_cast<s64>(PageSize) : -static_cast<s64>(PageSize);
            }
        }
        return delta;
    }

    u64 MappedTileBytes() const {
        return PageSize * static_cast<u64>(std::count(tile_pages.begin(), tile_pages.end(), true));
    }

    u64 size;
    std::vector<Block> blocks;
    bool warned{};
    u64 generation;
    bool tiles{};                 ///< The game maps its own memory into it (216): no blocks
    std::vector<bool> tile_pages; ///< Pages a tile maps, once `tiles`
};

/// nn::gpu::sdk::SparseMapRequest (nx2:gpu 216), filled from NvRm's nvgpu_as_remap_op.
struct Nx2SparseMapRequest {
    u64 gpu_address;
    u64 size;
    u8 no_write;
    u8 cacheable;
    u8 unknown_12;
    u8 kind;          ///< Wanted: the compressible kind if there is one
    u8 fallback_kind; ///< The incompressible kind
    u8 unknown_15;    ///< Remap op flag bit 15
    u16 padding_16;
    u32 memory_handle; ///< Sub 101's handle; 0 unmaps the range
    u32 padding_1c;
    u64 offset; ///< Into that memory
};
static_assert(sizeof(Nx2SparseMapRequest) == 40);

/// The requests of a 216 A buffer (a trailing partial request is left out).
inline std::vector<Nx2SparseMapRequest> Nx2ParseSparseMapRequests(std::span<const u8> data) {
    std::vector<Nx2SparseMapRequest> requests(data.size() / sizeof(Nx2SparseMapRequest));
    if (!requests.empty()) {
        std::memcpy(requests.data(), data.data(), requests.size() * sizeof(Nx2SparseMapRequest));
    }
    return requests;
}

/// True when [address, +size) is not empty and does not wrap the address space.
inline bool Nx2ValidRange(u64 address, u64 size) {
    return size != 0 && size <= std::numeric_limits<u64>::max() - address;
}

/// The entry of `ranges` (start -> value with a size) that holds all of [address, +size), or end.
template <typename Map, typename SizeOf>
auto Nx2RangeHolding(Map& ranges, u64 address, u64 size, SizeOf size_of) -> decltype(ranges.end()) {
    auto it = ranges.upper_bound(address);
    if (it == ranges.begin()) {
        return ranges.end();
    }
    --it;
    const u64 range_size = size_of(it->second);
    if (size > range_size || address - it->first > range_size - size) {
        return ranges.end();
    }
    return it;
}

} // namespace Service
