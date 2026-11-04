// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/am/frontend/applet_software_keyboard_types.h"

TEST_CASE("Native software keyboard settings keep the UTF-8 request", "[core][swkbd]") {
    using namespace Service::AM::Frontend;
    // Fire Emblem: Fortune's Weave's name entry: revision 0x160012, 0x530 bytes, UTF-8 output.
    std::vector<u8> data(sizeof(SwkbdConfigNative));
    data[offsetof(SwkbdConfigNative, use_utf8)] = 1;
    const u32 max_length = 8;
    std::memcpy(data.data() + offsetof(SwkbdConfigNative, max_text_length), &max_length,
                sizeof(max_length));
    SwkbdConfigCommon common{};
    SwkbdConfigNew config{};
    REQUIRE(DecodeNativeSwkbdConfig(data, common, config));
    CHECK(common.use_utf8);
    CHECK(common.max_text_length == 8);
    // Read with the Switch 1 layout instead, the request is lost: the text went back as UTF-16.
    SwkbdConfigCommon old{};
    std::memcpy(&old, data.data(), sizeof(old));
    CHECK_FALSE(old.use_utf8);

    data.resize(sizeof(SwkbdConfigCommon) + sizeof(SwkbdConfigNew));
    CHECK_FALSE(DecodeNativeSwkbdConfig(data, common, config));
}
