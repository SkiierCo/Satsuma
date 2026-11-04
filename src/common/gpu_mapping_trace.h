// SPDX-FileCopyrightText: Copyright 2026 Satsuma
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"

/// The last GPU address space and device memory mapping operations, to explain a GPU address
/// whose memory lacks what the game wrote (Mario Kart World's command lists, Yoshi's shader
/// code): which device memory the address pointed at over time, and which process address
/// backed that device memory.
namespace Common::GpuMappingTrace {

enum class Op : u8 {
    GpuMap,      ///< GPU address -> device address (target)
    GpuSparse,   ///< GPU address reserved without memory
    GpuUnmap,    ///< GPU address range unmapped
    DeviceMap,   ///< device address -> process address (target)
    DeviceUnmap, ///< device address range unmapped
};

struct Record {
    s64 time_ms;
    Op op;
    u64 address;
    u64 size;
    u64 target;
    u64 extra; ///< address space id or ASID
};

inline std::mutex mutex;
inline std::array<Record, 65536> records{};
inline u64 count{};

inline s64 NowMs() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

inline void Add(Op op, u64 address, u64 size, u64 target, u64 extra) {
    const s64 now = NowMs();
    std::scoped_lock lock{mutex};
    records[count++ % records.size()] = Record{now, op, address, size, target, extra};
}

/// The latest operations covering `address`, GPU-side ones when `gpu` is true, device-side ones
/// otherwise, newest first; one line each.
inline std::string Describe(bool gpu, u64 address, size_t max = 6) {
    static constexpr std::array names{"map", "sparse", "unmap", "map", "unmap"};
    const s64 now = NowMs();
    std::string out;
    std::scoped_lock lock{mutex};
    const u64 available = count < records.size() ? count : records.size();
    size_t found = 0;
    for (u64 i = 0; i < available && found < max; ++i) {
        const Record& r = records[(count - 1 - i) % records.size()];
        const bool gpu_op = r.op == Op::GpuMap || r.op == Op::GpuSparse || r.op == Op::GpuUnmap;
        if (gpu_op != gpu || address < r.address || address - r.address >= r.size) {
            continue;
        }
        ++found;
        out += fmt::format("\n    {} ms ago: {} {:#x}+{:#x}", now - r.time_ms,
                           names[static_cast<size_t>(r.op)], r.address, r.size);
        if (r.op == Op::GpuMap || r.op == Op::DeviceMap) {
            out += fmt::format(" -> {:#x}", r.target + (address - r.address));
        }
        out += fmt::format(" ({} {})", gpu ? "address space" : "asid", r.extra);
    }
    if (found == 0) {
        out += fmt::format("\n    no {} mapping operation recorded (oldest kept is {} ms old)",
                           gpu ? "GPU" : "device",
                           available ? now - records[(count - available) % records.size()].time_ms
                                     : 0);
    }
    return out;
}

} // namespace Common::GpuMappingTrace
