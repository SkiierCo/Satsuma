// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/renderer_vulkan/surface_format.h"

TEST_CASE("SDR output is the default and rejects HDR-only surfaces", "[video_core][surface_format]") {
    CHECK(Settings::values.force_sdr_output.GetDefault());
    const std::array formats{
        VkSurfaceFormatKHR{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        VkSurfaceFormatKHR{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
    };
    CHECK_FALSE(Vulkan::SelectSwapSurfaceFormat(formats, true));
    const auto automatic = Vulkan::SelectSwapSurfaceFormat(formats, false);
    REQUIRE(automatic);
    CHECK(automatic->format == formats[0].format);
    CHECK(automatic->colorSpace == formats[0].colorSpace);
}

TEST_CASE("SDR selection skips HDR and matches RGBA channel order", "[video_core][surface_format]") {
    const std::array formats{
        VkSurfaceFormatKHR{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    const auto selected = Vulkan::SelectSwapSurfaceFormat(formats, true, true);
    REQUIRE(selected);
    CHECK(selected->format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(selected->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    CHECK(Vulkan::SwapSurfaceViewFormat(selected->format) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_FALSE(Vulkan::SelectSwapSurfaceFormat(formats, true, false));
}

TEST_CASE("Normal SDR presentation retains its preferred format", "[video_core][surface_format]") {
    const std::array formats{
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    for (const bool force : {false, true}) {
        const auto selected = Vulkan::SelectSwapSurfaceFormat(formats, force);
        REQUIRE(selected);
        CHECK(selected->format == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(Vulkan::SwapSurfaceViewFormat(selected->format) == VK_FORMAT_B8G8R8A8_UNORM);
    }
}

TEST_CASE("SDR format selection handles empty and unrestricted format lists", "[video_core][surface_format]") {
    CHECK_FALSE(Vulkan::SelectSwapSurfaceFormat({}, true));
    const std::array unrestricted{
        VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    const auto selected = Vulkan::SelectSwapSurfaceFormat(unrestricted, true);
    REQUIRE(selected);
    CHECK(selected->format == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(selected->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    const std::array hdr_only{
        VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_HDR10_ST2084_EXT},
    };
    CHECK_FALSE(Vulkan::SelectSwapSurfaceFormat(hdr_only, true));
}
