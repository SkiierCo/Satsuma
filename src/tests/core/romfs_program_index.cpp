// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/loader/loader.h"

namespace {
class MountedLoader final : public Loader::AppLoader {
public:
    explicit MountedLoader(FileSys::VirtualFile romfs_) : AppLoader{nullptr}, romfs{std::move(romfs_)} {}
    Loader::FileType GetFileType() const override { return Loader::FileType::DeconstructedRomDirectory; }
    LoadResult Load(Kernel::KProcess&, Core::System&) override {
        return {Loader::ResultStatus::ErrorNotImplemented, std::nullopt};
    }
    Loader::ResultStatus ReadRomFS(FileSys::VirtualFile& out) override {
        out = romfs;
        return Loader::ResultStatus::Success;
    }
    bool IsRomFSUpdatable() const override { return false; }
private:
    FileSys::VirtualFile romfs;
};
}

TEST_CASE("Indexed storage opens the current mounted program without an installed NCA", "[romfs][program_index]") {
    Core::System system;
    Service::FileSystem::FileSystemController filesystem{system};
    FileSys::ManualContentProvider provider;
    const auto data = std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{3, 1, 4}, "romfs");
    MountedLoader loader{data};
    const auto factory = std::make_shared<FileSys::RomFSFactory>(loader, provider, filesystem);
    constexpr u64 base = 0x010019A01E2F2000;
    for (const u8 index : {u8{0}, u8{2}}) {
        Service::FileSystem::RomFsController controller{factory, base + index};
        const auto mounted = controller.OpenPatchedRomFSWithProgramIndex(base + index, index, FileSys::ContentRecordType::Program);
        REQUIRE(mounted == data);
        CHECK(mounted->ReadAllBytes() == std::vector<u8>{3, 1, 4});
        CHECK_FALSE(controller.OpenPatchedRomFSWithProgramIndex(base, index ^ 1, FileSys::ContentRecordType::Program));
        CHECK_FALSE(controller.OpenPatchedRomFSWithProgramIndex(base + 0x2000, index, FileSys::ContentRecordType::Program));
        CHECK_FALSE(controller.OpenPatchedRomFSWithProgramIndex(base, index, FileSys::ContentRecordType::Data));
    }
}
