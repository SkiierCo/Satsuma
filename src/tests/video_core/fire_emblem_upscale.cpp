// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/fire_emblem_upscale.h"

namespace FEUpscale = VideoCommon::FireEmblemUpscale;

TEST_CASE("Fire Emblem's DLSS output pass is replaced only for that title and key",
          "[video_core][fire_emblem]") {
    CHECK(FEUpscale::UseSpatialStandIn(FEUpscale::ProgramId, FEUpscale::OutputProgramKey, nullptr));
    CHECK(FEUpscale::UseSpatialStandIn(FEUpscale::ProgramId, FEUpscale::OutputProgramKey, "1"));
    // EDEN_NX2_DLSS_STANDIN=0 keeps the emulated network.
    CHECK_FALSE(FEUpscale::UseSpatialStandIn(FEUpscale::ProgramId, FEUpscale::OutputProgramKey, "0"));
    CHECK_FALSE(FEUpscale::UseSpatialStandIn(FEUpscale::ProgramId, 0x1234, nullptr));
    CHECK_FALSE(FEUpscale::UseSpatialStandIn(0x0400380020056000ULL, FEUpscale::OutputProgramKey, nullptr));
}

TEST_CASE("Fire Emblem's stand-in needs a plain larger B10G11R11 output", "[video_core][fire_emblem]") {
    using namespace Tegra::Texture;
    const auto image = [](u64 address, u32 width, u32 height) {
        TICEntry tic{};
        tic.format.Assign(TextureFormat::B10G11R11);
        tic.header_version.Assign(TICHeaderVersion::BlockLinear);
        tic.texture_type.Assign(TextureType::Texture2D);
        tic.address_low = static_cast<u32>(address);
        tic.address_high.Assign(static_cast<u32>(address >> 32));
        tic.width_minus_one.Assign(width - 1);
        tic.height_minus_1.Assign(height - 1);
        return tic;
    };
    const auto input = image(0x8105fa0000, 1280, 720);
    const auto output = image(0x8100000000, 2560, 1440);
    CHECK(FEUpscale::ValidPair(input, output));
    CHECK_FALSE(FEUpscale::ValidPair(output, input));
    CHECK_FALSE(FEUpscale::ValidPair(input, input));
    auto other_format = output;
    other_format.format.Assign(TextureFormat::A8B8G8R8);
    CHECK_FALSE(FEUpscale::ValidPair(input, other_format));
}

TEST_CASE("Fire Emblem's first DLSS pass hands one sample to the output pass",
          "[video_core][fire_emblem]") {
    using namespace Tegra::Texture;
    const auto image = [](TextureFormat format, u64 address, u32 width, u32 height) {
        TICEntry tic{};
        tic.format.Assign(format);
        tic.header_version.Assign(TICHeaderVersion::BlockLinear);
        tic.texture_type.Assign(TextureType::Texture2D);
        tic.address_low = static_cast<u32>(address);
        tic.address_high.Assign(static_cast<u32>(address >> 32));
        tic.width_minus_one.Assign(width - 1);
        tic.height_minus_1.Assign(height - 1);
        return tic;
    };
    const auto color = image(TextureFormat::B10G11R11, 0x8105fa0000, 1280, 720);
    CHECK(FEUpscale::ValidMotion(color, image(TextureFormat::R16G16, 0x8107f80000, 1280, 720)));
    CHECK_FALSE(FEUpscale::ValidMotion(color, image(TextureFormat::R16G16, 0x8107f80000, 640, 360)));
    CHECK_FALSE(FEUpscale::ValidMotion(color, image(TextureFormat::A8B8G8R8, 0x8107f80000, 1280, 720)));
    CHECK_FALSE(FEUpscale::ValidMotion(color, image(TextureFormat::R16G16, 0x8105fa0000, 1280, 720)));

    FEUpscale::PrepassCapture capture;
    capture.Capture({.color = color.Address(), .width = 1280, .height = 720,
                     .jitter = {0.25f, -0.375f}, .motion = std::nullopt});
    const auto sample = capture.Take();
    REQUIRE(sample);
    CHECK(sample->jitter[1] == -0.375f);
    // Taken once: the next output pass without a new first pass gets nothing.
    CHECK_FALSE(capture.Take());
    // A jitter beyond half a pixel, or not finite, is not used; a bad capture drops the last one.
    capture.Capture({.color = color.Address(), .width = 1280, .height = 720,
                     .jitter = {0.0f, 0.0f}, .motion = std::nullopt});
    capture.Capture({.color = color.Address(), .width = 1280, .height = 720,
                     .jitter = {0.75f, 0.0f}, .motion = std::nullopt});
    CHECK_FALSE(capture.Take());
    capture.Capture({.color = color.Address(), .width = 1280, .height = 720,
                     .jitter = {std::nanf(""), 0.0f}, .motion = std::nullopt});
    CHECK_FALSE(capture.Take());
}
