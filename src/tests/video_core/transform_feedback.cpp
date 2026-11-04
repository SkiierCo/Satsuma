// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/exception.h"
#include "video_core/transform_feedback.h"

TEST_CASE("Transform feedback rejects counts beyond its fixed location table", "[video_core][xfb]") {
    for (size_t buffer = 0; buffer < 4; ++buffer) {
        for (u32 count : {129U, 0x7150U, 0x1e00738eU, 0x1a70b4bfU, 0x8000068bU,
                          std::numeric_limits<u32>::max()}) {
            VideoCommon::TransformFeedbackState state{};
            state.layouts[buffer].varying_count = count;
            REQUIRE_THROWS_AS(VideoCommon::MakeTransformFeedbackVaryings(state),
                              Shader::InvalidArgument);
        }
    }
}

TEST_CASE("Transform feedback retains the full valid table", "[video_core][xfb]") {
    VideoCommon::TransformFeedbackState state{};
    state.layouts[0] = {.stream = 0, .varying_count = 128, .stride = 512};
    for (u32 word = 0; word < 32; ++word) {
        auto& locations = state.varyings[0][word];
        locations.attribute0.Assign(word * 4);
        locations.attribute1.Assign(word * 4 + 1);
        locations.attribute2.Assign(word * 4 + 2);
        locations.attribute3.Assign(word * 4 + 3);
    }
    const auto [varyings, count] = VideoCommon::MakeTransformFeedbackVaryings(state);
    REQUIRE(count == 125);
    REQUIRE(varyings[124].offset == 496);
    REQUIRE(varyings[124].components == 4);
    REQUIRE(varyings[124].stride == 512);
}
