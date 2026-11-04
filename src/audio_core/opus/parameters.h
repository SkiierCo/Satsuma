// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/logging.h"

namespace AudioCore::OpusDecoder {
constexpr size_t OpusStreamCountMax = 255;
constexpr size_t MaxChannels = 2;

struct OpusParameters {
    /* 0x00 */ u32 sample_rate;
    /* 0x04 */ u32 channel_count;
}; // size = 0x8
static_assert(sizeof(OpusParameters) == 0x8, "OpusParameters has the wrong size!");

struct OpusParametersEx {
    /* 0x00 */ u32 sample_rate;
    /* 0x04 */ u32 channel_count;
    /* 0x08 */ bool use_large_frame_size;
    /* 0x09 */ INSERT_PADDING_BYTES_NOINIT(7);
}; // size = 0x10
static_assert(sizeof(OpusParametersEx) == 0x10, "OpusParametersEx has the wrong size!");

struct OpusMultiStreamParameters {
    /* 0x00 */ u32 sample_rate;
    /* 0x04 */ u32 channel_count;
    /* 0x08 */ u32 total_stream_count;
    /* 0x0C */ u32 stereo_stream_count;
    /* 0x10 */ std::array<u8, OpusStreamCountMax + 1> mappings;
}; // size = 0x110
static_assert(sizeof(OpusMultiStreamParameters) == 0x110,
              "OpusMultiStreamParameters has the wrong size!");

struct OpusMultiStreamParametersEx {
    /* 0x00 */ u32 sample_rate;
    /* 0x04 */ u32 channel_count;
    /* 0x08 */ u32 total_stream_count;
    /* 0x0C */ u32 stereo_stream_count;
    /* 0x10 */ bool use_large_frame_size;
    /* 0x11 */ INSERT_PADDING_BYTES_NOINIT(7);
    /* 0x18 */ std::array<u8, OpusStreamCountMax + 1> mappings;
}; // size = 0x118
static_assert(sizeof(OpusMultiStreamParametersEx) == 0x118,
              "OpusMultiStreamParametersEx has the wrong size!");

struct OpusPacketHeader {
    /* 0x00 */ u32 size;
    /* 0x04 */ u32 final_range;
}; // size = 0x8
static_assert(sizeof(OpusPacketHeader) == 0x8, "OpusPacketHeader has the wrong size!");

inline bool IsValidOpusSampleRate(u32 sample_rate) {
    return sample_rate == 8'000 || sample_rate == 12'000 || sample_rate == 16'000 ||
           sample_rate == 24'000 || sample_rate == 48'000;
}

/// Cyberpunk 2077 sizes and opens its decoders at 0 Hz, then at an invalid rate: the rate comes
/// from the Switch 2 hardware codec session (hwopus 100), whose replies are undocumented and
/// empty here (the decoder settings it sends that session name 48000 Hz). An unset rate decodes
/// at 48 kHz, the rate Opus decodes at internally and the console mixes at; for Switch 2 titles
/// (switch2) so does any other invalid rate, which otherwise fails as on the console.
template <typename Params>
Params WithDefaultSampleRate(const Params& requested, bool switch2 = false) {
    Params params{requested};
    if (params.sample_rate == 0) {
        static bool logged{};
        if (!std::exchange(logged, true)) {
            LOG_WARNING(Audio, "Opus decoder without a sample rate; decoding at 48 kHz");
        }
        params.sample_rate = 48'000;
    } else if (switch2 && !IsValidOpusSampleRate(params.sample_rate)) {
        static u32 logged{};
        if (logged++ < 8) {
            LOG_WARNING(Audio, "Opus decoder at invalid sample rate {:#x}; decoding at 48 kHz",
                        params.sample_rate);
        }
        params.sample_rate = 48'000;
    }
    return params;
}
} // namespace AudioCore::OpusDecoder
