// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

#include "common/common_types.h"

// EDEN_NX2_RT_REPORT=1: per-window record of the colour render targets draws and clears wrote,
// with how many draws ran or were skipped, so a black frame can be traced to the pass that
// produced (or failed to produce) it. The present path reads each target back and logs it.
namespace VideoCommon::Nx2RtReport {

struct Target {
    u64 device_addr{};
    u32 width{};
    u32 height{};
    u32 format{};
    u32 bytes_per_pixel{};
    u64 draws{};
    u64 clears{};
    std::array<f32, 4> last_clear{};
    u64 first_ps{}; ///< pixel shader of the first draw into it
};

struct Window {
    u64 draws{};
    u64 skipped{};
    u64 clears{};
    std::map<u64, Target> targets; ///< by GPU address
};

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_RT_REPORT") != nullptr;
    return enabled;
}

namespace Detail {
inline std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}
inline Window& Current() {
    static Window window;
    return window;
}
} // namespace Detail

inline void NoteSkipped() {
    std::scoped_lock lock{Detail::Mutex()};
    ++Detail::Current().skipped;
}

inline void NoteDraw() {
    std::scoped_lock lock{Detail::Mutex()};
    ++Detail::Current().draws;
}

inline void NoteClear() {
    std::scoped_lock lock{Detail::Mutex()};
    ++Detail::Current().clears;
}

inline void NoteTarget(u64 gpu_addr, u64 device_addr, u32 width, u32 height, u32 format,
                       u32 bytes_per_pixel, bool clear, const std::array<f32, 4>& clear_color,
                       u64 pixel_shader) {
    std::scoped_lock lock{Detail::Mutex()};
    auto& target = Detail::Current().targets[gpu_addr];
    target.device_addr = device_addr;
    target.width = width;
    target.height = height;
    target.format = format;
    target.bytes_per_pixel = bytes_per_pixel;
    if (clear) {
        ++target.clears;
        target.last_clear = clear_color;
    } else {
        if (target.draws++ == 0) {
            target.first_ps = pixel_shader;
        }
    }
}

/// The window so far; starts a new one.
inline Window Take() {
    std::scoped_lock lock{Detail::Mutex()};
    return std::exchange(Detail::Current(), Window{});
}

} // namespace VideoCommon::Nx2RtReport
