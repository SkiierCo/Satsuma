// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"

namespace Service::VI {

inline int ParseDisplayRefreshRate(std::string_view text) {
    if (text.empty()) {
        return 60;
    }
    int rate{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rate);
    return error == std::errc{} && end == text.data() + text.size() && rate >= 30 && rate <= 240
               ? rate
               : 60;
}

inline int DisplayRefreshRate() {
    static const int rate = [] {
        const char* text = std::getenv("EDEN_NX2_VSYNC_HZ");
        return text ? ParseDisplayRefreshRate(text) : 60;
    }();
    return rate;
}

inline std::chrono::nanoseconds DisplayFramePeriod() {
    return std::chrono::nanoseconds{1'000'000'000LL / DisplayRefreshRate()};
}

/// What a Switch 2 present asks of the display: word 12 is (swap interval << 16) | refresh rate.
/// Fire Emblem: Fortune's Weave sends 0x1003c during its boot logos and 0x2003c (two 60 Hz
/// vblanks per frame, 30 FPS) after. The display follows the requested rate, as a variable
/// refresh panel does, up to the emulated display's own rate. A word without a rate keeps the
/// display rate. forced_interval (EDEN_NX2_SWAP_INTERVAL) overrides the interval when non-zero.
struct PresentTiming {
    u32 swap_interval;
    int refresh_rate;
};

inline PresentTiming GamePresentTiming(u32 word, int display_rate, u32 forced_interval = 0) {
    const u32 requested_rate{word & 0xffff};
    const int refresh_rate{requested_rate >= 30 && requested_rate <= 240
                               ? (std::min)(static_cast<int>(requested_rate), display_rate)
                               : display_rate};
    const u32 requested_interval{(std::min)((std::max)(word >> 16, 1U), 4U)};
    return {forced_interval != 0 ? forced_interval : requested_interval, refresh_rate};
}

/// Time between presents for that timing at the speed limit (percent; 100 is normal speed).
inline std::chrono::nanoseconds PresentPeriod(PresentTiming timing, u32 speed_limit) {
    return std::chrono::nanoseconds{100'000'000'000LL * timing.swap_interval /
                                    timing.refresh_rate / (std::max)(speed_limit, 1U)};
}

} // namespace Service::VI
