// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <array>
#include <map>
#include <optional>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nvdrv/devices/nx2_gpu_mapping_table.h"

using Service::Nvidia::Devices::Nx2GpuMappingTable;

TEST_CASE("Nx2 mapping changes match an independent page model", "[nx2_mapping]") {
    constexpr u64 page = 0x1000;
    constexpr size_t count = 64;
    Nx2GpuMappingTable table;
    std::array<std::optional<u64>, count> mappings{}, applied{};
    std::map<u64, std::pair<size_t, size_t>> leases;
    std::mt19937 random{0x534b4949};
    const auto effective = [&] {
        auto result = mappings;
        for (const auto& [id, range] : leases)
            for (size_t i = range.first; i < range.first + range.second; ++i)
                if (!result[i]) result[i] = (i + 1) * page;
        return result;
    };
    for (size_t step = 0; step < 10000; ++step) {
        const size_t first = random() % count;
        const size_t length = 1 + random() % (count - first);
        const u64 address = (first + 1) * page;
        const u64 size = length * page;
        const auto before = effective();
        Nx2GpuMappingTable::Update update;
        switch (random() % 4) {
        case 0: {
            const u64 cpu = (random() & 1) ? address : 0x100000 + first * page;
            bool allowed = true;
            for (const auto& [id, range] : leases)
                if (first < range.first + range.second && range.first < first + length && cpu != address)
                    allowed = false;
            update = table.Map(address, cpu, size);
            REQUIRE(update.success == allowed);
            if (allowed)
                for (size_t i = 0; i < length; ++i) mappings[first + i] = cpu + i * page;
            break;
        }
        case 1:
            update = table.Unmap(address, size);
            REQUIRE(update.success);
            for (size_t i = first; i < first + length; ++i) mappings[i].reset();
            break;
        case 2: {
            bool allowed = true;
            for (size_t i = first; i < first + length; ++i)
                if (mappings[i] && mappings[i] != (i + 1) * page) allowed = false;
            update = table.Acquire(address, size);
            REQUIRE(update.success == allowed);
            if (allowed) leases.emplace(update.lease, std::pair{first, length});
            break;
        }
        default: {
            u64 id = 0;
            if (!leases.empty()) {
                auto it = leases.begin();
                std::advance(it, random() % leases.size());
                id = it->first;
            }
            update = table.Release(id);
            REQUIRE(update.success == (leases.erase(id) != 0));
            break;
        }
        }
        const auto expected = effective();
        REQUIRE(applied == before);
        for (const auto& change : update.changes) {
            REQUIRE(change.range.gpu % page == 0);
            REQUIRE(change.range.size % page == 0);
            const size_t start = change.range.gpu / page - 1;
            const size_t pages = change.range.size / page;
            REQUIRE(start + pages <= count);
            for (size_t i = 0; i < pages; ++i)
                applied[start + i] = change.map ? std::optional{change.range.cpu + i * page} : std::nullopt;
        }
        REQUIRE(applied == expected);
        std::array<std::optional<u64>, count> actual{};
        for (const auto& range : table.Effective())
            for (u64 offset = 0; offset < range.size; offset += page)
                actual[(range.gpu + offset) / page - 1] = range.cpu + offset;
        REQUIRE(actual == expected);
    }
}

TEST_CASE("Nx2 native image mappings validate bound page extents", "[nx2_mapping]") {
    const auto range = Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0x1100, 0x1000);
    REQUIRE(range);
    CHECK(range->gpu == 0x11000);
    CHECK(range->size == 0x2000);
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0x3000, 0x1001));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0x5000, 1));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10001, 0x4000, 0, 0x1000));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0, 0x4000, 0, 0x1000));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(0x10000, 0x4000, 0, 0));
    CHECK_FALSE(Nx2GpuMappingTable::ImageRange(
        std::numeric_limits<u64>::max() - 0x1000, 0x2000, 0, 0x1000));
}

TEST_CASE("Nx2 native image leases share mappings across handles", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    const auto first = table.Acquire(0x10000, 0x3000);
    const auto second = table.Acquire(0x11000, 0x1000);
    REQUIRE(first.success);
    REQUIRE(second.success);
    REQUIRE(first.changes.size() == 1);
    CHECK(first.changes[0].map);
    CHECK(first.changes[0].native);
    CHECK(second.changes.empty());
    const auto released = table.Release(first.lease);
    REQUIRE(released.changes.size() == 2);
    CHECK_FALSE(released.changes[0].map);
    CHECK_FALSE(released.changes[1].map);
    CHECK(table.UsesBacking(0x10000, 0x3000));
    CHECK(table.Release(second.lease).changes.size() == 1);
    CHECK_FALSE(table.UsesBacking(0x10000, 0x3000));
    CHECK_FALSE(table.Release(second.lease).success);
}

TEST_CASE("Nx2 native image leases borrow existing GPU mappings", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    REQUIRE(table.Map(0x10000, 0x10000, 0x4000).success);
    const auto lease = table.Acquire(0x11000, 0x1000);
    REQUIRE(lease.success);
    CHECK(lease.changes.empty());
    CHECK(table.Release(lease.lease).changes.empty());
    CHECK(table.UsesBacking(0x10000, 0x4000));
    CHECK(table.Unmap(0x10000, 0x4000).changes.size() == 1);
}

TEST_CASE("Nx2 native leases survive external unmaps and reject conflicting aliases", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    REQUIRE(table.Map(0x10000, 0x10000, 0x3000).success);
    const auto lease = table.Acquire(0x11000, 0x1000);
    REQUIRE(lease.success);
    const auto unmap = table.Unmap(0x10000, 0x3000);
    REQUIRE(unmap.changes.size() == 2);
    CHECK_FALSE(table.Map(0x11000, 0x90000, 0x1000).success);
    CHECK(table.UsesBacking(0x11000, 0x1000));
    CHECK(table.Release(lease.lease).changes.size() == 1);
    REQUIRE(table.Map(0x11000, 0x90000, 0x1000).success);
    CHECK_FALSE(table.Acquire(0x11000, 0x1000).success);
}

TEST_CASE("Nx2 native backing remains borrowed by other GPU aliases", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    const auto lease = table.Acquire(0x10000, 0x2000);
    REQUIRE(lease.success);
    REQUIRE(table.Map(0x90000, 0x10000, 0x2000).success);
    REQUIRE(table.Map(0x80000, 0x10000, 0x2000).success);
    table.Release(lease.lease);
    CHECK(table.UsesBacking(0x10000, 0x2000));
    table.Unmap(0x80000, 0x1000);
    CHECK(table.UsesBacking(0x10000, 0x2000));
    table.Unmap(0x81000, 0x1000);
    CHECK(table.UsesBacking(0x10000, 0x2000));
    table.Unmap(0x90000, 0x2000);
    CHECK_FALSE(table.UsesBacking(0x10000, 0x2000));
}

TEST_CASE("Nx2 native remaps emit ordered removal then replacement", "[nx2_mapping]") {
    Nx2GpuMappingTable table;
    const auto first = table.Acquire(0x10000, 0x1000);
    const auto unmap = table.Release(first.lease);
    const auto second = table.Acquire(0x10000, 0x1000);
    REQUIRE(unmap.changes.size() == 1);
    REQUIRE(second.changes.size() == 1);
    CHECK_FALSE(unmap.changes[0].map);
    CHECK(second.changes[0].map);
    CHECK_FALSE(table.Release(first.lease).success);
    CHECK(table.UsesBacking(0x10000, 0x1000));
    table.Reset();
    const auto next_session = table.Acquire(0x10000, 0x1000);
    CHECK(next_session.lease != second.lease);
    CHECK_FALSE(table.Release(second.lease).success);
    CHECK(table.UsesBacking(0x10000, 0x1000));
}
