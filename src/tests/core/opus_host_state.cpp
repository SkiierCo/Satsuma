// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "audio_core/adsp/apps/opus/opus_decode_object.h"
#include "audio_core/adsp/apps/opus/opus_multistream_decode_object.h"

namespace {
using namespace AudioCore::ADSP::OpusDecoder;

TEST_CASE("Opus decoder host state respects guest buffers and session teardown", "[audio][opus]") {
    const auto size = OpusDecodeObject::GetWorkBufferSize(2);
    std::vector<u64> backing((size + 7) / 8 + 8);
    auto* bytes = reinterpret_cast<u8*>(backing.data());
    std::fill(bytes + size, bytes + backing.size() * sizeof(u64), 0xa5);
    const auto address = reinterpret_cast<u64>(bytes);
    auto& decoder = OpusDecodeObject::Initialize(address, address);
    REQUIRE(decoder.InitializeDecoder(48000, 2) == OPUS_OK);
    REQUIRE(decoder.ResetDecoder() == OPUS_OK);
    OpusDecodeObject::ReleaseAll();
    REQUIRE(decoder.ResetDecoder() == OPUS_INVALID_STATE);
    REQUIRE(decoder.InitializeDecoder(48000, 2) == OPUS_OK);
    REQUIRE(decoder.ResetDecoder() == OPUS_OK);
    REQUIRE(decoder.Shutdown() == OPUS_OK);
    REQUIRE(std::all_of(bytes + size, bytes + backing.size() * sizeof(u64),
                        [](u8 value) { return value == 0xa5; }));
    REQUIRE(OpusDecodeObject::GetWorkBufferSize(0) == 0);
    REQUIRE(OpusDecodeObject::GetWorkBufferSize(3) == 0);
}

TEST_CASE("Opus multistream host state releases and recreates safely", "[audio][opus]") {
    const auto size = OpusMultiStreamDecodeObject::GetWorkBufferSize(1, 1);
    std::vector<u64> backing((size + 7) / 8 + 8);
    auto* bytes = reinterpret_cast<u8*>(backing.data());
    std::fill(bytes + size, bytes + backing.size() * sizeof(u64), 0xa5);
    const auto address = reinterpret_cast<u64>(bytes);
    auto& decoder = OpusMultiStreamDecodeObject::Initialize(address, address);
    std::array<u8, 2> mapping{0, 1};
    REQUIRE(decoder.InitializeDecoder(48000, 1, 2, 1, mapping.data()) == OPUS_OK);
    REQUIRE(decoder.ResetDecoder() == OPUS_OK);
    OpusMultiStreamDecodeObject::ReleaseAll();
    REQUIRE(decoder.ResetDecoder() == OPUS_INVALID_STATE);
    REQUIRE(decoder.InitializeDecoder(48000, 1, 2, 1, mapping.data()) == OPUS_OK);
    REQUIRE(decoder.Shutdown() == OPUS_OK);
    REQUIRE(std::all_of(bytes + size, bytes + backing.size() * sizeof(u64),
                        [](u8 value) { return value == 0xa5; }));
    REQUIRE(OpusMultiStreamDecodeObject::GetWorkBufferSize(0, 0) == 0);
    REQUIRE(OpusMultiStreamDecodeObject::GetWorkBufferSize(1, 2) == 0);
}
} // namespace
