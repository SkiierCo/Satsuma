// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/loose_updates.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace {
constexpr u64 Title = 0x0400123400000000;
using namespace FileSys;

// Minimal NPDM with independent ACI/FAC blocks, not a copied game asset.
VirtualFile Npdm(u64 id, const VirtualDir& parent) {
    std::vector<u8> bytes(0x400);
    const auto write = [&](size_t offset, u64 value, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            bytes[offset + i] = static_cast<u8>(value >> (i * 8));
        }
    };
    std::memcpy(bytes.data(), "META", 4);
    write(0x70, 0x340, 4); // ACI
    write(0x78, 0x80, 4);  // ACID
    std::memcpy(bytes.data() + 0x280, "ACID", 4);
    write(0x2A0, 0x240, 4); // FAC offset relative to ACID
    std::memcpy(bytes.data() + 0x340, "ACI0", 4);
    write(0x350, id, 8);
    write(0x360, 0x40, 4); // FAH offset relative to ACI
    return std::make_shared<VectorVfsFile>(std::move(bytes), "main.npdm", parent);
}

VirtualDir Directory(const VirtualDir& parent, const std::string& name) {
    auto dir = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
        std::vector<VirtualDir>{}, name, parent);
    std::static_pointer_cast<VectorVfsDirectory>(parent)->AddDirectory(dir);
    return dir;
}

void File(const VirtualDir& dir, const std::string& name, u8 value) {
    std::static_pointer_cast<VectorVfsDirectory>(dir)->AddFile(
        std::make_shared<VectorVfsFile>(std::vector<u8>{value}, name, dir));
}

VirtualDir Content(const VirtualDir& parent, const std::string& name, u8 marker, u64 id = Title) {
    const auto content = Directory(parent, name);
    const auto exefs = Directory(content, "exefs");
    File(exefs, "main", marker);
    std::static_pointer_cast<VectorVfsDirectory>(exefs)->AddFile(Npdm(id, exefs));
    File(Directory(content, "romfs"), "shared", marker);
    return exefs;
}

struct Dump {
    VirtualDir root = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
        std::vector<VirtualDir>{}, "Game");
    VirtualDir base = Content(root, "base", 1);
    VirtualDir updates = Directory(root, "updates");
    VirtualDir old = Content(updates, "65536", 2);
    VirtualDir latest = Content(updates, "131072", 3);
};
} // namespace

TEST_CASE("Loose updates share one canonical game and select only matching programs", "[loose_updates]") {
    Dump dump;
    Content(dump.updates, "196608", 4, Title + 0x2000);
    Directory(dump.updates, "262144"); // Incomplete update without executable metadata.
    for (const auto& exefs : {dump.base, dump.old, dump.latest}) {
        const auto program = DiscoverLooseProgram(exefs, Title);
        REQUIRE(program.root == dump.root);
        REQUIRE(program.base_exefs == dump.base);
        REQUIRE(program.updates.size() == 2);
        CHECK(program.updates[0].version == 131072);
        CHECK(program.updates[1].version == 65536);
        REQUIRE(SelectLooseUpdate(program, {}));
        CHECK(SelectLooseUpdate(program, {})->exefs == dump.latest);
        CHECK(SelectLooseUpdate(program, {"Update@131072"})->exefs == dump.old);
        CHECK_FALSE(SelectLooseUpdate(program, {"Update@131072", "Update@65536"}));
        CHECK_FALSE(SelectLooseUpdate(program, {"Update"}));
    }
}

TEST_CASE("Loose update selection overlays ExeFS and RomFS together with base fallback", "[loose_updates]") {
    Dump dump;
    File(dump.base, "sdk", 7);
    File(dump.base->GetParentDirectory()->GetSubdirectory("romfs"), "base-only", 8);
    const auto program = DiscoverLooseProgram(dump.base, Title);
    for (const auto& disabled : std::vector<std::vector<std::string>>{
             {}, {"Update@131072"}, {"Update"}}) {
        const u8 expected = disabled.empty() ? 3 : disabled[0] == "Update" ? 1 : 2;
        auto exefs = ResolveLooseExeFS(program, disabled);
        CHECK(exefs->GetFile("main")->ReadAllBytes() == std::vector<u8>{expected});
        CHECK(exefs->GetFile("sdk")->ReadAllBytes() == std::vector<u8>{7});
        auto romfs = ExtractRomFS(ResolveLooseRomFS(program, disabled));
        REQUIRE(romfs);
        CHECK(romfs->GetFile("shared")->ReadAllBytes() == std::vector<u8>{expected});
        CHECK(romfs->GetFile("base-only")->ReadAllBytes() == std::vector<u8>{8});
    }
    // A packed RomFS needs no repacking when no overlay is selected.
    const auto packed = std::make_shared<VectorVfsFile>(
        ResolveLooseRomFS(program, {"Update"})->ReadAllBytes(), "game.romfs", dump.base);
    std::static_pointer_cast<VectorVfsDirectory>(dump.base)->AddFile(packed);
    CHECK(ResolveLooseRomFS(program, {"Update"}) == packed);

}

TEST_CASE("Numeric-only dumps are discovered without requiring a nonexistent base", "[loose_updates]") {
    auto root = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
        std::vector<VirtualDir>{}, "Fire Emblem folder");
    const auto only = Content(root, "131072", 9);
    const auto mains = ListLooseProgramMains(root);
    REQUIRE(mains.size() == 1);
    CHECK(mains[0] == only->GetFile("main"));
    CHECK(FindLooseProgramMain(root) == only->GetFile("main"));
    CHECK(FindLooseProgramMain(only) == only->GetFile("main"));
    const auto program = DiscoverLooseProgram(only, Title);
    CHECK(program.root == root);
    CHECK(program.base_exefs == only);
    CHECK(program.base_version == 131072U);
    CHECK(program.updates.empty());
    CHECK(ResolveLooseExeFS(program, {"Update"}) == only);

    Loader::AppLoader_DeconstructedRomDirectory loader{only->GetFile("main")};
    u64 id{};
    CHECK(loader.ReadProgramId(id) == Loader::ResultStatus::Success);
    CHECK(id == Title);
    std::string name;
    CHECK(loader.ReadTitle(name) == Loader::ResultStatus::Success);
    CHECK(name == "Fire Emblem folder");
    NACP control;
    CHECK(loader.ReadControlData(control) == Loader::ResultStatus::ErrorNoControl);
}

TEST_CASE("Loose display sidecars supply a title and icon without inventing guest control data", "[loose_updates]") {
    Dump dump;
    const std::string title = "\xEF\xBB\xBF  Mario Kart World\r\n";
    std::static_pointer_cast<VectorVfsDirectory>(dump.root)->AddFile(
        std::make_shared<VectorVfsFile>(std::vector<u8>(title.begin(), title.end()), "title.txt", dump.root));
    // Prefer the named artwork over an arbitrary image in the same sidecar directory.
    File(dump.root, "another.png", 4);
    File(dump.root, "icon.png", 9);
    for (const auto& exefs : {dump.base, dump.latest}) {
        Loader::AppLoader_DeconstructedRomDirectory loader{exefs->GetFile("main")};
        std::string name;
        REQUIRE(loader.ReadTitle(name) == Loader::ResultStatus::Success);
        CHECK(name == "Mario Kart World");
        std::vector<u8> icon;
        REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
        CHECK(icon == std::vector<u8>{9});
        u64 id{};
        CHECK(loader.ReadProgramId(id) == Loader::ResultStatus::Success);
        CHECK(id == Title);
        NACP control;
        CHECK(loader.ReadControlData(control) == Loader::ResultStatus::ErrorNoControl);
    }
}

TEST_CASE("Existing add-on patch flags select the same loose version as the loader", "[loose_updates]") {
    Dump dump;
    auto& disabled = Settings::values.disabled_addons[Title];
    const auto saved = disabled;
    SCOPE_EXIT { disabled = saved; };
    Core::System system;
    ContentProviderUnion provider{};
    const PatchManager patches{Title, system.GetFileSystemController(), provider, dump.base};
    for (const auto& keys : std::vector<std::vector<std::string>>{
             {}, {"Update@131072"}, {"Update"}}) {
        disabled = keys;
        const auto list = patches.GetPatches();
        REQUIRE(list.size() == 2);
        CHECK(list[0].numeric_version == 131072);
        CHECK(list[1].numeric_version == 65536);
        CHECK(list[0].enabled == keys.empty());
        CHECK(list[1].enabled == (keys == std::vector<std::string>{"Update@131072"}));
        CHECK(patches.GetGameVersion() == (keys.empty() ? std::optional<u32>{131072} :
              keys[0] == "Update" ? std::nullopt : std::optional<u32>{65536}));
        const auto patched_exefs = patches.PatchExeFS(dump.base);
        REQUIRE(patched_exefs);
        const u8 selected_marker = keys.empty() ? 3 : keys[0] == "Update" ? 1 : 2;
        CHECK(patched_exefs->GetFile("main")->ReadAllBytes() == std::vector<u8>{selected_marker});
        Loader::AppLoader_DeconstructedRomDirectory loader{dump.base->GetFile("main")};
        VirtualFile romfs;
        REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
        const u8 expected = keys.empty() ? 3 : keys[0] == "Update" ? 1 : 2;
        CHECK(ExtractRomFS(romfs)->GetFile("shared")->ReadAllBytes() == std::vector<u8>{expected});
    }
}

TEST_CASE("Loose version parsing rejects title IDs, overflow and partial names", "[loose_updates]") {
    CHECK(ParseLooseUpdateVersion("0") == 0U);
    CHECK(ParseLooseUpdateVersion("917504") == 917504U);
    CHECK_FALSE(ParseLooseUpdateVersion("0400123400000000"));
    CHECK_FALSE(ParseLooseUpdateVersion("4294967296"));
    CHECK_FALSE(ParseLooseUpdateVersion("131072-old"));
    CHECK_FALSE(ParseLooseUpdateVersion("-1"));
    CHECK_FALSE(ParseLooseUpdateVersion(""));
}

TEST_CASE("Game-folder opening rejects ambiguous titles and probes nested title version layouts", "[loose_updates]") {
    Dump dump;
    CHECK(FindLooseProgramMain(dump.root) == dump.base->GetFile("main"));
    auto title = Directory(dump.updates, "0400123400000000");
    auto nested = Content(title, "262144", 5);
    const auto program = DiscoverLooseProgram(nested, Title);
    REQUIRE(program.updates.size() == 3);
    CHECK(program.updates.front().version == 262144);
    CHECK(FindLooseProgramMain(dump.root) == dump.base->GetFile("main"));
    CHECK(ListLooseProgramMains(dump.root).size() == 4);
    Content(dump.updates, "196608", 4, Title + 0x2000);
    CHECK_FALSE(FindLooseProgramMain(dump.root));
}
