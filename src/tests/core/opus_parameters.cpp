// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "audio_core/opus/parameters.h"

TEST_CASE("Opus decoders without a sample rate decode at 48 kHz", "[audio_core][opus]") {
    using namespace AudioCore::OpusDecoder;
    const OpusParametersEx unset{.sample_rate = 0, .channel_count = 2, .use_large_frame_size = false};
    CHECK(WithDefaultSampleRate(unset).sample_rate == 48'000);
    CHECK(WithDefaultSampleRate(unset).channel_count == 2);

    // A rate the game chose is kept, valid or not; validation still rejects invalid ones.
    for (const u32 rate : {8'000U, 24'000U, 48'000U, 44'100U, 1U}) {
        const OpusParametersEx chosen{.sample_rate = rate, .channel_count = 1, .use_large_frame_size = true};
        CHECK(WithDefaultSampleRate(chosen).sample_rate == rate);
        CHECK(WithDefaultSampleRate(chosen).use_large_frame_size);
    }

    // Switch 2 titles decode an invalid rate at 48 kHz too (Cyberpunk 2077's second decoder).
    for (const u32 rate : {44'100U, 1U, 0x8e302d40U}) {
        const OpusParametersEx chosen{.sample_rate = rate, .channel_count = 2, .use_large_frame_size = false};
        CHECK(WithDefaultSampleRate(chosen, true).sample_rate == 48'000);
        CHECK(WithDefaultSampleRate(chosen, true).channel_count == 2);
    }
    for (const u32 rate : {8'000U, 12'000U, 16'000U, 24'000U}) {
        const OpusParametersEx chosen{.sample_rate = rate, .channel_count = 1, .use_large_frame_size = false};
        CHECK(WithDefaultSampleRate(chosen, true).sample_rate == rate);
    }

    OpusMultiStreamParametersEx multi{};
    multi.channel_count = 6;
    multi.total_stream_count = 4;
    CHECK(WithDefaultSampleRate(multi).sample_rate == 48'000);
    CHECK(WithDefaultSampleRate(multi).total_stream_count == 4);
}
