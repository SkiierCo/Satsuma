// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "common/common_types.h"
#include "core/savestate/archive.h"

namespace Core::SaveState {

// Field-by-field little-endian component payloads. Every section starts with a tag and version,
// so a reader rejects a payload written by an incompatible schema instead of misreading it.
// Pod() copies trivially copyable host structures and is only used for guest-defined layouts
// (registers, hardware register files, guest structures) that are little-endian by definition.
class Writer {
public:
    void U8(u8 v) {
        data.push_back(v);
    }
    void Bool(bool v) {
        U8(v ? 1 : 0);
    }
    void U16(u16 v) {
        Raw(&v, sizeof(v));
    }
    void U32(u32 v) {
        Raw(&v, sizeof(v));
    }
    void U64(u64 v) {
        Raw(&v, sizeof(v));
    }
    void S32(s32 v) {
        U32(static_cast<u32>(v));
    }
    void S64(s64 v) {
        U64(static_cast<u64>(v));
    }
    void F32(f32 v) {
        Raw(&v, sizeof(v));
    }
    void String(std::string_view v) {
        U32(static_cast<u32>(v.size()));
        Raw(v.data(), v.size());
    }
    void Blob(std::span<const u8> v) {
        U64(v.size());
        Raw(v.data(), v.size());
    }
    template <typename T>
    void Pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        U32(static_cast<u32>(sizeof(T)));
        Raw(&v, sizeof(T));
    }
    template <typename T>
    void PodVector(std::span<const T> v) {
        static_assert(std::is_trivially_copyable_v<T>);
        U32(static_cast<u32>(sizeof(T)));
        U64(v.size());
        Raw(v.data(), v.size_bytes());
    }
    // Starts a named, versioned section.
    void Section(u32 tag, u32 version) {
        U32(tag);
        U32(version);
    }
    void Raw(const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        data.insert(data.end(), b, b + n);
    }

    std::vector<u8> data;
};

class Reader {
public:
    explicit Reader(std::span<const u8> data_) : data{data_} {}

    u8 U8() {
        return Take(1)[0];
    }
    bool Bool() {
        const u8 v = U8();
        if (v > 1) {
            throw Error("Invalid boolean in save state");
        }
        return v != 0;
    }
    u16 U16() {
        return Get<u16>();
    }
    u32 U32() {
        return Get<u32>();
    }
    u64 U64() {
        return Get<u64>();
    }
    s32 S32() {
        return static_cast<s32>(U32());
    }
    s64 S64() {
        return static_cast<s64>(U64());
    }
    f32 F32() {
        return Get<f32>();
    }
    std::string String() {
        const u32 size = U32();
        const auto bytes = Take(size);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::vector<u8> Blob() {
        const u64 size = U64();
        if (size > Remaining()) {
            throw Error("Truncated save state");
        }
        const auto bytes = Take(static_cast<size_t>(size));
        return {bytes.begin(), bytes.end()};
    }
    template <typename T>
    void Pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (U32() != sizeof(T)) {
            throw Error("Save state structure size differs from this build");
        }
        std::memcpy(&v, Take(sizeof(T)).data(), sizeof(T));
    }
    template <typename T>
    T Pod() {
        T v{};
        Pod(v);
        return v;
    }
    template <typename T>
    std::vector<T> PodVector() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (U32() != sizeof(T)) {
            throw Error("Save state structure size differs from this build");
        }
        const u64 count = U64();
        if (count > Remaining() / sizeof(T)) {
            throw Error("Truncated save state");
        }
        std::vector<T> out(static_cast<size_t>(count));
        std::memcpy(out.data(), Take(out.size() * sizeof(T)).data(), out.size() * sizeof(T));
        return out;
    }
    // Checks a section header written by Writer::Section.
    void Section(u32 tag, u32 version) {
        const u32 got_tag = U32();
        const u32 got_version = U32();
        if (got_tag != tag) {
            throw Error("Save state section is missing or out of order");
        }
        if (got_version != version) {
            throw Error("Save state section version is not supported by this build");
        }
    }
    std::span<const u8> Take(size_t n) {
        if (n > data.size()) {
            throw Error("Truncated save state");
        }
        const auto out = data.first(n);
        data = data.subspan(n);
        return out;
    }
    size_t Remaining() const {
        return data.size();
    }
    void Finish() const {
        if (!data.empty()) {
            throw Error("Save state has unexpected trailing data");
        }
    }

private:
    template <typename T>
    T Get() {
        T v;
        std::memcpy(&v, Take(sizeof(T)).data(), sizeof(T));
        return v;
    }

    std::span<const u8> data;
};

constexpr u32 Tag(const char (&s)[5]) {
    return static_cast<u32>(s[0]) | (static_cast<u32>(s[1]) << 8) |
           (static_cast<u32>(s[2]) << 16) | (static_cast<u32>(s[3]) << 24);
}

} // namespace Core::SaveState
