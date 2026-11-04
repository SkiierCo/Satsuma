// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>

#include "core/savestate/archive.h"
#include "video_core/native_qmd.h"

namespace Tegra {

void NativeQmdScheduler::ResetForNewSession() {
    states.clear();
}

std::vector<std::uint8_t> NativeQmdScheduler::SaveState() {
    Core::SaveState::Encoder output;
    output.U32(1);
    output.U32(static_cast<u32>(states.size()));
    std::vector<Address> addresses;
    addresses.reserve(states.size());
    for (const auto& [address, state] : states) {
        addresses.push_back(address);
    }
    std::ranges::sort(addresses);
    for (const auto address : addresses) {
        const auto& state = states.at(address);
        output.U64(address);
        output.U32(std::bit_cast<u32>(state.dependencies));
        output.U32(std::bit_cast<u32>(state.deferred));
        output.U32((state.loaded ? 1U : 0U) | (state.scheduled ? 2U : 0U) |
                   (state.completed ? 4U : 0U));
        for (const auto word : state.descriptor) {
            output.U32(word);
        }
    }
    return std::move(output.data);
}

void NativeQmdScheduler::LoadState(std::span<const std::uint8_t> bytes) {
    using Core::SaveState::Error;
    Core::SaveState::Decoder input{bytes};
    if (input.U32() != 1) {
        throw Error{"Unsupported GPU QMD state version"};
    }
    const auto count = input.U32();
    if (count > 4096) {
        throw Error{"GPU QMD state table exceeds its limit"};
    }
    decltype(states) restored;
    for (u32 index = 0; index < count; ++index) {
        const auto address = input.U64();
        State state;
        state.dependencies = std::bit_cast<s32>(input.U32());
        state.deferred = std::bit_cast<s32>(input.U32());
        const auto flags = input.U32();
        state.loaded = (flags & 1) != 0;
        state.scheduled = (flags & 2) != 0;
        state.completed = (flags & 4) != 0;
        for (auto& word : state.descriptor) {
            word = input.U32();
        }
        if ((flags & ~7U) || (state.completed && (!state.loaded || !state.scheduled)) ||
            (state.scheduled && !state.loaded) ||
            (state.loaded &&
             ((state.descriptor[18] & 0xff) != 0x30 || (state.descriptor[4] & 0x100))) ||
            !restored.emplace(address, state).second) {
            throw Error{"Invalid or duplicate GPU QMD state"};
        }
    }
    input.Finish();
    states = std::move(restored);
}

} // namespace Tegra
