// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

inline std::optional<VkSurfaceFormatKHR> SelectSwapSurfaceFormat(
    std::span<const VkSurfaceFormatKHR> formats, bool force_sdr, bool mutable_format = false) {
    if (formats.empty()) {
        return std::nullopt;
    }
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        if (force_sdr && formats[0].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return std::nullopt;
        }
        return VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }
    const auto preferred = std::ranges::find_if(formats, [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (preferred != formats.end()) {
        return *preferred;
    }
    if (!force_sdr) {
        return formats.front();
    }
    const auto sdr = std::ranges::find_if(formats, [mutable_format](const auto& format) {
        return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
               (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                (mutable_format && (format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                                    format.format == VK_FORMAT_R8G8B8A8_SRGB)));
    });
    return sdr != formats.end() ? std::make_optional(*sdr) : std::nullopt;
}

inline VkFormat SwapSurfaceViewFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

} // namespace Vulkan
