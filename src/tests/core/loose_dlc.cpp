// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/loose_dlc.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"

namespace {
constexpr u64 PrimeDlc = 0x010019A01E2F3001;
struct TemporaryNand {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("skii-loose-dlc-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    FileSys::RealVfsFilesystem vfs;
    FileSys::VirtualDir root = vfs.CreateDirectory(Common::FS::PathToUTF8String(path));
    ~TemporaryNand() {
        root.reset();
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

FileSys::VirtualFile Payload(std::vector<u8> bytes = {}) {
    auto directory = std::make_shared<FileSys::VectorVfsDirectory>();
    directory->AddFile(std::make_shared<FileSys::VectorVfsFile>(
        std::move(bytes), "Switch2EditionUnlock.unlock"));
    return FileSys::CreateRomFS(directory);
}

class TestLoader final : public Loader::AppLoader {
public:
    TestLoader() : AppLoader{nullptr} {}
    Loader::FileType GetFileType() const override { return Loader::FileType::DeconstructedRomDirectory; }
    LoadResult Load(Kernel::KProcess&, Core::System&) override {
        return {Loader::ResultStatus::ErrorNotImplemented, std::nullopt};
    }
    Loader::ResultStatus ReadRomFS(FileSys::VirtualFile& out) override {
        out = Payload();
        return Loader::ResultStatus::Success;
    }
};
}

TEST_CASE("Decrypted DLC persists in NAND and mounts the supplied unlock file", "[loose_dlc]") {
    TemporaryNand nand;
    REQUIRE(nand.root);
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload()) ==
            FileSys::LooseDlcInstallResult::Success);
    // Use another filesystem instance, as on an emulator restart.
    FileSys::RealVfsFilesystem restarted;
    auto contents = restarted.OpenDirectory(Common::FS::PathToUTF8String(nand.path));
    CHECK(FileSys::ListInstalledLooseDlc(contents) == std::vector<u64>{PrimeDlc});
    auto mounted = FileSys::OpenInstalledLooseDlc(contents, PrimeDlc);
    REQUIRE(mounted);
    const auto extracted = FileSys::ExtractRomFS(mounted);
    REQUIRE(extracted);
    const auto unlock = extracted->GetFile("Switch2EditionUnlock.unlock");
    REQUIRE(unlock);
    CHECK(unlock->GetSize() == 0);
    CHECK_FALSE(FileSys::OpenInstalledLooseDlc(contents, PrimeDlc + 0x2000));
    CHECK_FALSE(FileSys::OpenInstalledLooseDlc(contents, PrimeDlc - 0x1001));
}

TEST_CASE("DLC replacement and cancellation preserve the complete previous installation", "[loose_dlc]") {
    TemporaryNand nand;
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload({1, 2, 3})) ==
            FileSys::LooseDlcInstallResult::Success);
    const auto larger = Payload(std::vector<u8>(0x300000, 7));
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, larger,
        [](u64, u64 copied) { return copied >= 0x100000; }) ==
            FileSys::LooseDlcInstallResult::Cancelled);
    auto read_unlock = [&] {
        return FileSys::ExtractRomFS(FileSys::OpenInstalledLooseDlc(nand.root, PrimeDlc))
            ->GetFile("Switch2EditionUnlock.unlock")->ReadAllBytes();
    };
    CHECK(read_unlock() == std::vector<u8>{1, 2, 3});
    CHECK_FALSE(nand.root->GetSubdirectory("loose_dlc")->GetFile("010019A01E2F3001.pending"));
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload({4, 5})) ==
            FileSys::LooseDlcInstallResult::Overwrite);
    CHECK(read_unlock() == std::vector<u8>{4, 5});
    CHECK(FileSys::ListInstalledLooseDlc(nand.root) == std::vector<u64>{PrimeDlc});
}

TEST_CASE("Unfinished DLC imports never appear as installed content", "[loose_dlc]") {
    TemporaryNand nand;
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload({9})) ==
            FileSys::LooseDlcInstallResult::Success);
    const auto root = nand.root->GetSubdirectory("loose_dlc");
    REQUIRE(root->GetFile("010019A01E2F3001.romfs")->Rename("010019A01E2F3001.previous"));
    // Simulate interruption between moving the previous payload and publishing its replacement.
    REQUIRE(root->CreateFile("010019A01E2F3001.pending"));
    CHECK(FileSys::ListInstalledLooseDlc(nand.root) == std::vector<u64>{PrimeDlc});
    REQUIRE(root->CreateFile("010019A01E2F3001.romfs"));
    CHECK(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload({8}),
        [](u64, u64) { return true; }) == FileSys::LooseDlcInstallResult::Cancelled);
    auto recovered = FileSys::ExtractRomFS(FileSys::OpenInstalledLooseDlc(nand.root, PrimeDlc));
    REQUIRE(recovered);
    CHECK(recovered->GetFile("Switch2EditionUnlock.unlock")->ReadAllBytes() == std::vector<u8>{9});
    recovered.reset();
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload({8})) ==
            FileSys::LooseDlcInstallResult::Overwrite);
    CHECK_FALSE(root->GetFile("010019A01E2F3001.previous"));
    CHECK_FALSE(root->GetFile("010019A01E2F3001.pending"));
    REQUIRE(FileSys::VfsRawCopy(Payload(), root->CreateFile("010019A01E2F3002.pending")));
    CHECK(FileSys::ListInstalledLooseDlc(nand.root) == std::vector<u64>{PrimeDlc});
}

TEST_CASE("DLC import rejects invalid IDs and malformed RomFS without replacing content", "[loose_dlc]") {
    TemporaryNand nand;
    CHECK_FALSE(FileSys::ParseLooseDlcTitleId("010019A01E2F2000"));
    CHECK_FALSE(FileSys::ParseLooseDlcTitleId("010019A01E2F2800"));
    CHECK_FALSE(FileSys::ParseLooseDlcTitleId("../010019a01e2f3001"));
    CHECK_FALSE(FileSys::ParseLooseDlcTitleId("010019A01E2F300G"));
    CHECK(FileSys::ParseLooseDlcTitleId("010019a01e2f3001") == PrimeDlc);
    REQUIRE(FileSys::InstallLooseDlc(nand.root, PrimeDlc, Payload()) ==
            FileSys::LooseDlcInstallResult::Success);
    auto bad = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(0x50, 0xff));
    CHECK(FileSys::InstallLooseDlc(nand.root, PrimeDlc, bad) ==
          FileSys::LooseDlcInstallResult::InvalidContent);
    auto bytes = Payload()->ReadAllBytes();
    std::fill(bytes.begin() + 8, bytes.begin() + 24, 0xff);
    bad = std::make_shared<FileSys::VectorVfsFile>(bytes);
    CHECK_FALSE(FileSys::IsLooseDlcRomFS(bad));
    CHECK(FileSys::ListInstalledLooseDlc(nand.root) == std::vector<u64>{PrimeDlc});
}

TEST_CASE("Decrypted DLC folder resolution selects the newest numeric version", "[loose_dlc]") {
    TemporaryNand nand;
    auto title = nand.root->CreateSubdirectory("010019a01e2f3001");
    auto old = title->CreateDirectoryRelative("9/romfs");
    auto latest = title->CreateDirectoryRelative("10/romfs");
    REQUIRE(old);
    REQUIRE(latest);
    CHECK(FileSys::FindLooseDlcRomFS(title)->GetFullPath() == latest->GetFullPath());
    CHECK(FileSys::FindLooseDlcTitleId(latest) == PrimeDlc);
    CHECK(FileSys::FindLooseDlcRomFS(latest)->GetFullPath() == latest->GetFullPath());
}

TEST_CASE("Installed decrypted DLC is available through storage and the existing add-on setting", "[loose_dlc]") {
    TemporaryNand nand;
    using Common::FS::EdenPath;
    const std::array paths{EdenPath::NANDDir, EdenPath::SDMCDir, EdenPath::LoadDir, EdenPath::DumpDir};
    std::map<EdenPath, std::filesystem::path> previous_paths;
    for (const auto key : paths) {
        previous_paths.emplace(key, Common::FS::GetEdenPath(key));
        const auto target = nand.path / std::to_string(static_cast<int>(key));
        std::filesystem::create_directory(target);
        Common::FS::SetEdenPath(key, target);
    }
    const auto previous_external = Settings::values.external_content_dirs;
    const auto previous_disabled = Settings::values.disabled_addons;
    SCOPE_EXIT {
        for (const auto& [key, path] : previous_paths) {
            Common::FS::SetEdenPath(key, path);
        }
        Settings::values.external_content_dirs = previous_external;
        Settings::values.disabled_addons = previous_disabled;
    };
    Settings::values.external_content_dirs.clear();
    Settings::values.disabled_addons.clear();
    Core::System system;
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    auto& filesystem = system.GetFileSystemController();
    filesystem.CreateFactories(nand.vfs);
    REQUIRE(FileSys::InstallLooseDlc(filesystem.GetUserNANDContentDirectory(), PrimeDlc, Payload({6})) ==
            FileSys::LooseDlcInstallResult::Success);
    FileSys::ManualContentProvider provider;
    TestLoader loader;
    FileSys::RomFSFactory factory(loader, provider, filesystem);
    REQUIRE(factory.Open(PrimeDlc, FileSys::StorageId::NandUser, FileSys::ContentRecordType::Data));
    REQUIRE(factory.OpenPatchedRomFS(PrimeDlc, FileSys::ContentRecordType::Data));
    CHECK_FALSE(factory.Open(PrimeDlc, FileSys::StorageId::NandSystem, FileSys::ContentRecordType::Data));
    CHECK_FALSE(factory.Open(PrimeDlc, FileSys::StorageId::NandUser, FileSys::ContentRecordType::Program));
    constexpr u64 base = PrimeDlc - 0x1001;
    FileSys::PatchManager manager(base, filesystem, system.GetContentProvider());
    auto patches = manager.GetPatches();
    const auto is_dlc = [](const auto& patch) { return patch.type == FileSys::PatchType::DLC; };
    REQUIRE(std::count_if(patches.begin(), patches.end(), is_dlc) == 1);
    CHECK(std::find_if(patches.begin(), patches.end(), is_dlc)->enabled);
    Settings::values.disabled_addons[base] = {"DLC"};
    CHECK_FALSE(factory.Open(PrimeDlc, FileSys::StorageId::NandUser, FileSys::ContentRecordType::Data));
    CHECK_FALSE(factory.OpenPatchedRomFS(PrimeDlc, FileSys::ContentRecordType::Data));
    patches = manager.GetPatches();
    REQUIRE(std::count_if(patches.begin(), patches.end(), is_dlc) == 1);
    CHECK_FALSE(std::find_if(patches.begin(), patches.end(), is_dlc)->enabled);
    FileSys::PatchManager other(base + 0x2000, filesystem, system.GetContentProvider());
    const auto unrelated = other.GetPatches();
    CHECK(std::none_of(unrelated.begin(), unrelated.end(), is_dlc));
}

TEST_CASE("Renaming an open host VFS file retains access through the new path", "[loose_dlc][vfs]") {
    TemporaryNand nand;
    auto file = nand.root->CreateFile("before");
    REQUIRE(file);
    REQUIRE(file->WriteBytes(std::vector<u8>{2, 4, 6}) == 3);
    REQUIRE(file->Rename("after"));
    CHECK(file->GetName() == "after");
    CHECK(file->ReadAllBytes() == std::vector<u8>{2, 4, 6});
    CHECK_FALSE(nand.root->GetFile("before"));
    REQUIRE(nand.root->GetFile("after"));
}
