// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_sparse_region.h"
#include "video_core/nx2.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

using Service::Nx2SparseMapRequest;
using Service::Nx2SparseRegion;

namespace {

using Ranges = std::vector<std::pair<u64, u64>>;

std::vector<u8> RequestBytes(std::span<const Nx2SparseMapRequest> requests) {
    std::vector<u8> bytes(requests.size() * sizeof(Nx2SparseMapRequest));
    std::memcpy(bytes.data(), requests.data(), bytes.size());
    return bytes;
}

struct CleanSparseState {
    CleanSparseState() {
        Tegra::Nx2ClearSparseRegions();
    }
    ~CleanSparseState() {
        Tegra::Nx2ClearSparseRegions();
    }
};

VideoCommon::ImageInfo R8Image(u32 size, s32 levels) {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::R8_UNORM;
    info.size = {size, size, 1};
    info.resources.levels = levels;
    return info;
}

bool SameRect(const VideoCommon::LevelRect& rect, s32 level, u32 x, u32 y, u32 width, u32 height) {
    return rect.level == level && rect.x == x && rect.y == y && rect.width == width &&
           rect.height == height;
}

} // Anonymous namespace

TEST_CASE("Nx2 216 requests keep NvRm's 40-byte layout", "[nx2_mapping]") {
    std::array<u8, 40> raw{};
    const u64 gpu = 0x816d7f0000;
    const u64 size = 0x10000;
    const u32 handle = 7;
    const u64 offset = 0x20000;
    std::memcpy(raw.data() + 0x00, &gpu, sizeof(gpu));
    std::memcpy(raw.data() + 0x08, &size, sizeof(size));
    raw[0x13] = 0xdb; // wanted kind
    raw[0x14] = 0xfe; // fallback kind
    std::memcpy(raw.data() + 0x18, &handle, sizeof(handle));
    std::memcpy(raw.data() + 0x20, &offset, sizeof(offset));

    const auto requests = Service::Nx2ParseSparseMapRequests(raw);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].gpu_address == gpu);
    CHECK(requests[0].size == size);
    CHECK(requests[0].kind == 0xdb);
    CHECK(requests[0].fallback_kind == 0xfe);
    CHECK(requests[0].memory_handle == handle);
    CHECK(requests[0].offset == offset);
}

TEST_CASE("Nx2 216 parsing leaves out a trailing partial request", "[nx2_mapping]") {
    const std::array requests{
        Nx2SparseMapRequest{.gpu_address = 0x10000, .size = 0x10000, .memory_handle = 1},
        Nx2SparseMapRequest{.gpu_address = 0x20000, .size = 0x10000, .memory_handle = 0},
    };
    auto bytes = RequestBytes(requests);
    bytes.resize(bytes.size() + 12);
    const auto parsed = Service::Nx2ParseSparseMapRequests(bytes);
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].memory_handle == 1);
    CHECK(parsed[1].gpu_address == 0x20000);
    CHECK(parsed[1].memory_handle == 0);
    CHECK(Service::Nx2ParseSparseMapRequests({}).empty());
    CHECK(Service::Nx2ParseSparseMapRequests(std::span<const u8>{bytes.data(), 39}).empty());
}

TEST_CASE("Nx2 216 ranges resolve to the region that holds them whole", "[nx2_mapping]") {
    std::map<u64, u64> regions{{0x100000, 0x10000}, {0x200000, 0x20000}};
    const auto size_of = [](u64 size) { return size; };
    CHECK(Service::Nx2RangeHolding(regions, 0x100000, 0x10000, size_of)->first == 0x100000);
    CHECK(Service::Nx2RangeHolding(regions, 0x21f000, 0x1000, size_of)->first == 0x200000);
    CHECK(Service::Nx2RangeHolding(regions, 0x10f000, 0x2000, size_of) == regions.end());
    CHECK(Service::Nx2RangeHolding(regions, 0x0ff000, 0x2000, size_of) == regions.end());
    CHECK(Service::Nx2RangeHolding(regions, 0x150000, 0x1000, size_of) == regions.end());
    CHECK(Service::Nx2RangeHolding(regions, 0x200000, 0x30000, size_of) == regions.end());
    CHECK(Service::Nx2ValidRange(0x1000, 0x1000));
    CHECK_FALSE(Service::Nx2ValidRange(0x1000, 0));
    CHECK_FALSE(Service::Nx2ValidRange(~u64{0} - 0xfff, 0x2000));
}

TEST_CASE("Nx2 tiles release backed blocks and stop new ones", "[nx2_mapping]") {
    Nx2SparseRegion region{3 * Nx2SparseRegion::BlockSize, 1};
    const auto backed = region.BeginBacking(0);
    REQUIRE(backed);
    REQUIRE(region.FinishBacking(*backed, true));
    const auto pending = region.BeginBacking(2);
    REQUIRE(pending);

    CHECK(region.StartTiles() == std::vector<u64>{0});
    CHECK(region.tiles);
    CHECK(region.blocks[0] == Nx2SparseRegion::Block::Written);
    CHECK(region.blocks[1] == Nx2SparseRegion::Block::Empty);
    CHECK(region.StartTiles().empty());

    // No more blocks get memory; a write marks its block once.
    CHECK_FALSE(region.BeginBacking(1));
    CHECK(region.NoteTileWrite(1));
    CHECK_FALSE(region.NoteTileWrite(1));
    CHECK(region.blocks[1] == Nx2SparseRegion::Block::Written);

    // The block that was being backed meanwhile gets none.
    CHECK(region.IsPending(*pending));
    CHECK(region.AbandonBacking(*pending));
    CHECK(region.blocks[2] == Nx2SparseRegion::Block::Written);
    CHECK_FALSE(region.AbandonBacking(*pending));
    CHECK_FALSE(region.FinishBacking(*pending, true));
}

TEST_CASE("Nx2 tile writes are only noted once tiles are mapped", "[nx2_mapping]") {
    Nx2SparseRegion region{Nx2SparseRegion::BlockSize, 1};
    CHECK_FALSE(region.NoteTileWrite(0));
    CHECK(region.blocks[0] == Nx2SparseRegion::Block::Empty);
    CHECK(region.MarkTiles(0, 0x10000, true) == 0);
}

TEST_CASE("Nx2 mapped tiles count their blocks as written and their pages once", "[nx2_mapping]") {
    Nx2SparseRegion region{3 * Nx2SparseRegion::BlockSize, 1};
    region.StartTiles();
    const u64 offset = Nx2SparseRegion::BlockSize - 0x10000;
    CHECK(region.MarkMapped(offset, 0x20000) == std::pair<u64, u64>{0, 1});
    CHECK(region.blocks[0] == Nx2SparseRegion::Block::Written);
    CHECK(region.blocks[1] == Nx2SparseRegion::Block::Written);
    CHECK(region.blocks[2] == Nx2SparseRegion::Block::Empty);

    CHECK(region.MarkTiles(offset, 0x20000, true) == 0x20000);
    CHECK(region.MarkTiles(offset, 0x10000, true) == 0);
    CHECK(region.MappedTileBytes() == 0x20000);
    CHECK(region.MarkTiles(offset + 0x8000, 0x10000, false) == -0x10000);
    CHECK(region.MappedTileBytes() == 0x10000);
    // Clipped to the region.
    CHECK(region.MarkTiles(region.size - 0x1000, 0x10000, true) == 0x1000);
}

TEST_CASE("Nx2 sparse data ranges follow written blocks until tiles are mapped", "[nx2_mapping]") {
    CleanSparseState clean;
    constexpr u64 Region = 0x81'0000'0000;
    constexpr u64 Block = 4ULL << 20;
    Tegra::Nx2SparseRegionCreated(Region, 4 * Block, Block);
    CHECK(Tegra::Nx2SparseDataRanges(Region, 4 * Block).empty());

    Tegra::Nx2SparseBlockWritten(Region, 1);
    Tegra::Nx2SparseBlockWritten(Region, 2);
    CHECK(Tegra::Nx2SparseDataRanges(Region, 4 * Block) == Ranges{{Block, 2 * Block}});
    CHECK(Tegra::Nx2SparseDataRanges(Region + Block + 0x1000, 0x2000) == Ranges{{0, 0x2000}});

    // Once the game maps tiles, only they hold data.
    Tegra::Nx2SparseTilesMapped(Region, 0x20000, 0x20000, true);
    Tegra::Nx2SparseTilesMapped(Region, 3 * Block, 0x8000, true);
    CHECK(Tegra::Nx2SparseDataRanges(Region, 4 * Block) ==
          Ranges{{0x20000, 0x20000}, {3 * Block, Tegra::Nx2SparseTileSize}});
    CHECK(Tegra::Nx2SparseDataRanges(Region + 0x30000, 0x20000) == Ranges{{0, 0x10000}});

    Tegra::Nx2SparseTilesMapped(Region, 0x20000, 0x10000, false);
    CHECK(Tegra::Nx2SparseDataRanges(Region, 0x100000) == Ranges{{0x30000, 0x10000}});

    // Outside the region, or not in one at all.
    CHECK(Tegra::Nx2SparseDataRanges(Region + 3 * Block, 2 * Block).empty());
    CHECK(Tegra::Nx2SparseDataRanges(Region - 0x1000, 0x1000).empty());
    Tegra::Nx2SparseTilesMapped(Region, 4 * Block, 0x10000, true); // past the end: ignored
    Tegra::Nx2SparseRegionDestroyed(Region);
    CHECK(Tegra::Nx2SparseDataRanges(Region, Block).empty());
}

TEST_CASE("Nx2 sparse tile rectangles cover the host blocks the guest ranges touch",
          "[nx2_mapping]") {
    // R8, block height 1 GOB: guest blocks are 64x8 texels (512 bytes), 16 to a row of 1024;
    // host sparse blocks are 256x256.
    const auto info = R8Image(1024, 2);
    const Ranges first_block{{0, 512}};
    auto rects = VideoCommon::SparseTileRects(info, first_block, 256, 256, 2);
    REQUIRE(rects.size() == 1);
    CHECK(SameRect(rects[0], 0, 0, 0, 256, 256));
    CHECK(VideoCommon::UnswizzledRectsBytes(info, rects) == 256 * 256);

    // A whole row of guest blocks touches four host blocks: one run.
    const Ranges first_row{{0, 16 * 512}};
    rects = VideoCommon::SparseTileRects(info, first_row, 256, 256, 2);
    REQUIRE(rects.size() == 1);
    CHECK(SameRect(rects[0], 0, 0, 0, 1024, 256));

    // Guest block row 32 starts at texel row 256; the last block of a row is at x 960.
    const Ranges row_32{{32 * 16 * 512 + 15 * 512, 512}};
    rects = VideoCommon::SparseTileRects(info, row_32, 256, 256, 2);
    REQUIRE(rects.size() == 1);
    CHECK(SameRect(rects[0], 0, 768, 256, 256, 256));

    // Levels from the mip tail on come whole.
    const Ranges level_1{{1024 * 1024, 1}};
    rects = VideoCommon::SparseTileRects(info, level_1, 256, 256, 1);
    REQUIRE(rects.size() == 1);
    CHECK(SameRect(rects[0], 1, 0, 0, 512, 512));

    CHECK(VideoCommon::SparseTileRects(info, Ranges{}, 256, 256, 2).empty());
}
