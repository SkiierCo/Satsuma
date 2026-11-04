// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "video_core/textures/texture.h"

// Fire Emblem: Fortune's Weave upscales its 3D scene with a DLSS-style tensor-core network that
// keeps temporal history. Emulated in software the network left the scene smeared into streaks;
// its output pass is replaced by an upscale of its input instead: temporal, along the motion
// vectors and camera jitter its first DLSS pass reads, else spatial (from Eglantine-nx2-zortos,
// which found the program keys, slots and the temporal upscale).
namespace VideoCommon::FireEmblemUpscale {

constexpr u64 ProgramId = 0x04007EF01F8AE000ULL;
// NativeProgramKey of the captured DLSS final output pass, not its lowered shader hash.
constexpr u64 OutputProgramKey = 0x14b4222bbe23abeeULL;
/// The network's first pass: input colour at cbuf0+0x278, motion vectors at +0x290, depth (Z32,
/// the input's size; from Eglantine build 187) at +0x298 and this frame's camera jitter (x, y in
/// input pixels) at +0x1b0.
constexpr u64 PrepassProgramKey = 0x4cbf6eff32525aecULL;

/// What the first pass read this frame, for the output pass.
struct PrepassSample {
    GPUVAddr color{};
    u32 width{};
    u32 height{};
    std::array<f32, 2> jitter{};
    std::optional<Tegra::Texture::TICEntry> motion;
    std::optional<Tegra::Texture::TICEntry> depth; ///< for NVIDIA DLSS and FSR 3.1
};

/// The motion vectors are R16G16 float at the input's size.
inline bool ValidMotion(const Tegra::Texture::TICEntry& color,
                        const Tegra::Texture::TICEntry& motion) {
    using namespace Tegra::Texture;
    return motion.format == TextureFormat::R16G16 && motion.Address() != 0 &&
           motion.IsBlockLinear() && motion.Width() == color.Width() &&
           motion.Height() == color.Height() && motion.Address() != color.Address();
}

/// The depth is Z32 at the input's size; NVIDIA DLSS and FSR 3.1 need it, the stand-in doesn't.
inline bool ValidDepth(const Tegra::Texture::TICEntry& color,
                       const Tegra::Texture::TICEntry& depth) {
    using namespace Tegra::Texture;
    return depth.format == TextureFormat::Z32 && depth.Address() != 0 && depth.IsBlockLinear() &&
           depth.Width() == color.Width() && depth.Height() == color.Height() &&
           depth.Address() != color.Address();
}

/// One sample per frame: each output attempt takes it, usable or not.
class PrepassCapture {
public:
    void Capture(PrepassSample next) {
        sample.reset();
        // A Halton jitter stays within half a pixel; anything else is not a jitter.
        if (next.color == 0 || next.width == 0 || next.height == 0 ||
            !std::isfinite(next.jitter[0]) || !std::isfinite(next.jitter[1]) ||
            std::abs(next.jitter[0]) > 0.5f || std::abs(next.jitter[1]) > 0.5f) {
            return;
        }
        sample = std::move(next);
    }

    std::optional<PrepassSample> Take() {
        return std::exchange(sample, std::nullopt);
    }

private:
    std::optional<PrepassSample> sample;
};

/// EDEN_NX2_DLSS_STANDIN=0 keeps the emulated network for comparisons.
inline bool UseSpatialStandIn(u64 program_id, u64 native_key, const char* option) {
    return program_id == ProgramId && native_key == OutputProgramKey &&
           (option == nullptr || std::string_view{option} != "0");
}

/// The output pass reads the input colour through cbuf0+0x328 and writes the DLSS output
/// through cbuf0+0x348: plain B10G11R11 2D images, the output larger (720p to 1440p docked).
inline bool ValidPair(const Tegra::Texture::TICEntry& input,
                      const Tegra::Texture::TICEntry& output) {
    using namespace Tegra::Texture;
    const auto plain_image = [](const TICEntry& image) {
        return (image.texture_type == TextureType::Texture2D ||
                image.texture_type == TextureType::Texture2DNoMipmap) &&
               image.format == TextureFormat::B10G11R11 && image.IsBlockLinear() &&
               image.Depth() == 1 && image.BaseLayer() == 0 && image.max_mip_level == 0 &&
               image.Address() != 0;
    };
    return plain_image(input) && plain_image(output) && input.Address() != output.Address() &&
           output.Width() > input.Width() && output.Height() > input.Height();
}

/// Thrown from the compute shader lookup when the output pass was replaced: a handled dispatch,
/// never a rejection to remember.
struct DispatchReplaced {};

} // namespace VideoCommon::FireEmblemUpscale
