// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include "common/logging.h"
#include "common/random.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/loose_dlc.h"
#include "core/file_sys/loose_updates.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

#ifndef HAS_NCE
namespace Core::NCE {
class Patcher {};
} // namespace Core::NCE
#endif

namespace Loader {

struct PatchCollection {
    explicit PatchCollection(bool is_application_) : is_application{is_application_} {
        module_patcher_indices.fill(-1);
        patchers.emplace_back();
    }

    std::vector<Core::NCE::Patcher>* GetPatchers() {
        if (is_application && Settings::IsNceEnabled()) {
            return &patchers;
        }
        return nullptr;
    }

    size_t GetTotalPatchSize() const {
        size_t total_size{};
#ifdef HAS_NCE
        for (auto& patcher : patchers) {
            total_size += patcher.GetSectionSize();
        }
#endif
        return total_size;
    }

    void SaveIndex(size_t module) {
        module_patcher_indices[module] = static_cast<s32>(patchers.size() - 1);
    }

    s32 GetIndex(size_t module) const {
        return module_patcher_indices[module];
    }

    s32 GetLastIndex() const {
        return static_cast<s32>(patchers.size()) - 1;
    }

    bool is_application;
    std::vector<Core::NCE::Patcher> patchers;
    std::array<s32, 13> module_patcher_indices{};
};

namespace {

// RomFS for a loose ExeFS: the first .romfs file next to main, else a romfs/ directory next to
// the exefs/ directory (the usual layout of an extracted title).
FileSys::VirtualFile FindLooseRomFS(const FileSys::VirtualDir& exefs_dir) {
    if (exefs_dir == nullptr) {
        return nullptr;
    }
    for (const auto& f : exefs_dir->GetFiles()) {
        if (f->GetExtension() == "romfs") {
            return f;
        }
    }
    if (const auto parent = exefs_dir->GetParentDirectory()) {
        if (const auto romfs_dir = parent->GetSubdirectory("romfs")) {
            LOG_INFO(Loader, "Using RomFS directory next to the ExeFS");
            return FileSys::CreateRomFS(romfs_dir);
        }
    }
    return nullptr;
}

// The display version of a dump without control data, from its RomFS: Nintendo EPD titles name
// their resource size tables after it (System/Resource/ResourceSizeTable.Product.180.* in Mario
// Kart World 1.8.0). A hotfix can keep its minor version's tables (Product.110 in a 1.1.1).
std::string RomFSProductVersion(const FileSys::VirtualDir& romfs_dir) {
    if (romfs_dir == nullptr) {
        return {};
    }
    const auto system_dir = romfs_dir->GetSubdirectory("System");
    const auto resource_dir = system_dir != nullptr ? system_dir->GetSubdirectory("Resource") : nullptr;
    if (resource_dir == nullptr) {
        return {};
    }
    constexpr std::string_view Prefix = "ResourceSizeTable.Product.";
    std::string best;
    for (const auto& file : resource_dir->GetFiles()) {
        const std::string file_name = file->GetName();
        if (!file_name.starts_with(Prefix) || file_name.size() < Prefix.size() + 4) {
            continue;
        }
        const std::string_view digits = std::string_view{file_name}.substr(Prefix.size(), 3);
        if (file_name[Prefix.size() + 3] != '.' ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        if (std::string version{digits}; version > best) {
            best = version;
        }
    }
    if (best.empty()) {
        return {};
    }
    return fmt::format("{}.{}.{}", best[0], best[1], best[2]);
}

// Loose ExeFS dumps carry no control data, but icon/nacp sidecars can be dropped next to the
// ExeFS or in the game folder above it (<game>/exefs/main, <game>/base/exefs/main and
// <game>/updates/<title id>/exefs/main layouts).
std::vector<FileSys::VirtualDir> FindSidecarDirs(const FileSys::VirtualDir& exefs_dir) {
    const auto equals_ignore_case = [](const std::string& a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };

    std::vector<FileSys::VirtualDir> dirs{exefs_dir};
    for (auto dir = exefs_dir; dirs.size() < 4;) {
        const auto parent = dir->GetParentDirectory();
        if (parent == nullptr) {
            break;
        }
        const auto& name = parent->GetName();
        dirs.push_back(parent);
        const bool storage_level =
            equals_ignore_case(name, "base") || equals_ignore_case(name, "update") ||
            equals_ignore_case(name, "updates") ||
            (!name.empty() && name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
        if (!storage_level) {
            break;
        }
        dir = parent;
    }
    return dirs;
}

} // namespace

namespace {

FileSys::VirtualFile LayerSwitch2EditionDlc(Core::System& system,
                                            const FileSys::ProgramMetadata& metadata,
                                            FileSys::VirtualFile romfs) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_EDITION_DLC");
        return value == nullptr || value[0] != '0';
    }();
    const u64 title_id = metadata.GetTitleID();
    // Super Mario Bros. Wonder's Switch 2 Edition program keeps the Switch 1 NPDM layout and
    // compression, so nothing in its files says Switch 2: EDEN_NX2_EDITION_DLC_<program ID>=1
    // names such a game.
    const auto per_game = std::getenv(fmt::format("EDEN_NX2_EDITION_DLC_{:016X}", title_id).c_str());
    const bool named = per_game != nullptr && per_game[0] == '1';
    if (!enabled || (title_id >> 56) != 0x01 || (!metadata.IsSwitch2() && !named)) {
        return romfs;
    }
    const auto& disabled = Settings::values.disabled_addons[title_id];
    if (std::find(disabled.begin(), disabled.end(), "DLC") != disabled.end()) {
        return romfs;
    }
    const u64 base = FileSys::GetBaseTitleID(title_id);
    const auto nand = system.GetFileSystemController().GetUserNANDContentDirectory();
    std::vector<std::pair<u64, FileSys::VirtualFile>> dlc;
    for (const u64 id : FileSys::ListInstalledLooseDlc(nand)) {
        if (FileSys::GetBaseTitleID(id) == base) {
            if (auto file = FileSys::OpenInstalledLooseDlc(nand, id)) {
                dlc.emplace_back(id, std::move(file));
            }
        }
    }
    const auto& provider = system.GetContentProvider();
    for (const auto& entry :
         provider.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data)) {
        if (FileSys::GetBaseTitleID(entry.title_id) != base ||
            std::any_of(dlc.begin(), dlc.end(),
                        [&](const auto& item) { return item.first == entry.title_id; })) {
            continue;
        }
        if (const auto nca = provider.GetEntry(entry.title_id, FileSys::ContentRecordType::Data);
            nca != nullptr && nca->GetRomFS() != nullptr) {
            dlc.emplace_back(entry.title_id, nca->GetRomFS());
        }
    }
    if (dlc.empty()) {
        return romfs;
    }
    // Later add-ons over earlier ones, all over the game's files.
    std::sort(dlc.begin(), dlc.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<FileSys::VirtualDir> layers;
    for (const auto& [id, file] : dlc) {
        if (auto dir = FileSys::ExtractRomFS(file)) {
            LOG_INFO(Loader, "Switch 2 Edition: add-on {:016X} laid over the game's RomFS", id);
            layers.push_back(std::move(dir));
        }
    }
    auto game = FileSys::ExtractRomFS(romfs);
    if (layers.empty() || game == nullptr) {
        return romfs;
    }
    layers.push_back(std::move(game));
    return FileSys::CreateRomFS(FileSys::LayeredVfsDirectory::MakeLayeredDirectory(std::move(layers)));
}

} // namespace

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_)
    : AppLoader(std::move(file_))
    , override_update(override_update_)
{
    auto file_dir = file->GetContainingDirectory();

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr) {
        const auto res = metadata.Load(npdm);
        if (res == ResultStatus::Success)
            title_id = metadata.GetTitleID();
    }

    const auto loose = FileSys::DiscoverLooseProgram(file_dir, title_id);
    loose_exefs = loose.base_exefs;
    resolved_exefs = FileSys::ResolveLooseExeFS(loose, Settings::values.disabled_addons[title_id]);
    if (const auto* update = FileSys::SelectLooseUpdate(loose, Settings::values.disabled_addons[title_id])) {
        file_dir = update->exefs;
        romfs_version = fmt::format("Version {}", update->version);
    } else if (loose.base_version) {
        romfs_version = fmt::format("Version {}", *loose.base_version);
    }
    if (resolved_exefs) {
        metadata.Load(resolved_exefs->GetFile("main.npdm"));
    }
    auto sidecar_dirs = FindSidecarDirs(file_dir);
    if (loose_exefs && loose_exefs != file_dir) {
        const auto base_sidecars = FindSidecarDirs(loose_exefs);
        sidecar_dirs.insert(sidecar_dirs.end(), base_sidecars.begin(), base_sidecars.end());
    }
    if (loose.root) {
        sidecar_dirs.push_back(loose.root);
    }

    // Icon
    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& sidecar_dir : sidecar_dirs) {
        for (const auto& language : FileSys::LANGUAGE_NAMES) {
            icon_file = sidecar_dir->GetFile("icon_" + std::string(language) + ".dat");
            if (icon_file != nullptr) {
                break;
            }
        }
        if (icon_file != nullptr) {
            icon_data = icon_file->ReadAllBytes();
            break;
        }
    }

    if (icon_data.empty()) {
        // Any png, jpeg, or bmp file
        for (const auto& sidecar_dir : sidecar_dirs) {
            if (const auto preferred = sidecar_dir->GetFile("icon.png")) {
                icon_data = preferred->ReadAllBytes();
                break;
            }
            const auto& files = sidecar_dir->GetFiles();
            const auto icon_iter =
                std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) {
                    return f->GetExtension() == "png" || f->GetExtension() == "jpg" ||
                           f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
                });
            if (icon_iter != files.end()) {
                icon_data = (*icon_iter)->ReadAllBytes();
                break;
            }
        }
    }

    // Metadata
    FileSys::VirtualFile nacp_file = nullptr;
    for (const auto& sidecar_dir : sidecar_dirs) {
        nacp_file = sidecar_dir->GetFile("control.nacp");
        if (nacp_file != nullptr) {
            break;
        }
        const auto& files = sidecar_dir->GetFiles();
        const auto nacp_iter =
            std::find_if(files.begin(), files.end(),
                         [](const FileSys::VirtualFile& f) { return f->GetExtension() == "nacp"; });
        if (nacp_iter != files.end()) {
            nacp_file = *nacp_iter;
            break;
        }
    }

    if (nacp_file != nullptr) {
        control.emplace(nacp_file);
        name = control->GetApplicationName();
    }

    // Host display metadata for loose dumps. This does not synthesize guest NACP fields.
    if (name.empty()) {
        for (const auto& sidecar_dir : sidecar_dirs) {
            const auto title = sidecar_dir->GetFile("title.txt");
            if (!title || title->GetSize() == 0 || title->GetSize() > 4096) {
                continue;
            }
            const auto bytes = title->ReadAllBytes();
            std::string text(bytes.begin(), bytes.end());
            if (text.starts_with("\xEF\xBB\xBF")) {
                text.erase(0, 3);
            }
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                continue;
            }
            text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
            if (text.find_first_of(std::string_view{"\0\r\n", 3}) == std::string::npos) {
                name = std::move(text);
                break;
            }
        }
    }

    // An absent NACP cannot provide an official name/icon, but the dump's folder is usable.
    if (name.empty() && loose.root) {
        name = loose.root->GetName();
    }
    romfs = FileSys::ResolveLooseRomFS(loose, Settings::values.disabled_addons[title_id]);
    if (romfs_version.empty()) {
        if (const auto title_dir = file_dir->GetParentDirectory()) {
            romfs_version = RomFSProductVersion(title_dir->GetSubdirectory("romfs"));
        }
    }
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    FileSys::VirtualDir directory, bool override_update_)
    : AppLoader(directory->GetFile("main"))
    , dir(std::move(directory))
    , override_update(override_update_)
{
    romfs = FindLooseRomFS(dir);
}

FileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile& dir_file) {
    if (FileSys::IsDirectoryExeFS(dir_file->GetContainingDirectory())) {
        return FileType::DeconstructedRomDirectory;
    }

    return FileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(
    Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    // Loaded directly from a loose ExeFS (not wrapped by the NCA/NSP loaders, which register
    // the process with the filesystem controller themselves).
    const bool standalone = dir == nullptr;
    if (dir == nullptr) {
        if (file == nullptr) {
            return {ResultStatus::ErrorNullFile, {}};
        }

        dir = resolved_exefs ? resolved_exefs : file->GetContainingDirectory();
    }

    // Read meta to determine title ID
    FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result = metadata.Load(npdm);
    if (result != ResultStatus::Success) {
        return {result, {}};
    }

    if (override_update || standalone) {
        const FileSys::PatchManager patch_manager(
            metadata.GetTitleID(), system.GetFileSystemController(), system.GetContentProvider(),
            standalone ? loose_exefs : nullptr);
        dir = patch_manager.PatchExeFS(dir);
    }

    // Reread in case PatchExeFS affected the main.npdm
    npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result2 = metadata.Reload(npdm);
    if (result2 != ResultStatus::Success) {
        return {result2, {}};
    }
    metadata.Print();

    // A Switch 2 Edition of a Switch 1 game (Switch 1 program ID, Switch 2 program or named by a
    // per-game switch): its upgrade
    // DLC holds files laid out like the game's own (Super Mario Bros. Wonder's Meetup in
    // Bellabel Park), and the game only mounts its RomFS, never the add-on content, so the DLC
    // goes over the game's files. EDEN_NX2_EDITION_DLC=0 leaves them as they are.
    if (romfs != nullptr) {
        romfs = LayerSwitch2EditionDlc(system, metadata, std::move(romfs));
    }

    // Loose-ExeFS loads bypass the NCA/NSP loaders, which are the ones that normally apply
    // LayeredFS to the RomFS; apply it here so mods in user/load/<titleid>/ work too.
    if (romfs != nullptr) {
        const FileSys::PatchManager patch_manager(metadata.GetTitleID(),
                                                  system.GetFileSystemController(),
                                                  system.GetContentProvider());
        auto patched = patch_manager.PatchRomFS(nullptr, romfs);
        if (patched != nullptr) {
            romfs = std::move(patched);
            LOG_INFO(Loader, "Applied RomFS patches (LayeredFS) for title {:016X}",
                     metadata.GetTitleID());
        }
    }

    // Enable NCE only for applications with 39-bit address space.
    const bool is_39bit =
        metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
    const bool is_application = metadata.GetPoolPartition() == FileSys::PoolPartition::Application;
    Settings::SetNceEnabled(is_39bit);

    const std::array static_modules = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                       "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                       "subsdk8", "subsdk9", "sdk"};

    std::size_t code_size{};

    // Define an nce patch context for each potential module.
    PatchCollection patch_ctx{is_application};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, code_size, should_pass_arguments, false, {},
            patch_ctx.GetPatchers(), patch_ctx.GetLastIndex());
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        patch_ctx.SaveIndex(i);
        code_size = *tentative_next_load_addr;
    }

    // Enable direct memory mapping in case of NCE.
    const u64 fastmem_base = [&]() -> size_t {
        if (is_application && Settings::IsNceEnabled()) {
            auto& buffer = system.DeviceMemory().buffer;
            buffer.EnableDirectMappedAddress();
            return reinterpret_cast<u64>(buffer.VirtualBasePointer());
        }
        return 0;
    }();

    // Add patch size to the total module size
    code_size += patch_ctx.GetTotalPatchSize();

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, code_size, fastmem_base, aslr_offset).IsError()) {
        return {ResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }

    // Load NSO modules
    modules.clear();
    const VAddr base_address{GetInteger(process.GetEntryPoint())};
    VAddr next_load_addr{base_address};
    const FileSys::PatchManager pm{metadata.GetTitleID(), system.GetFileSystemController(),
                                   system.GetContentProvider()};
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, load_addr, should_pass_arguments, true, pm,
            patch_ctx.GetPatchers(), patch_ctx.GetIndex(i));
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#x}", module, load_addr);
    }

    if (standalone) {
        // Without this, fsp-srv SetCurrentProcess fails with ResultTargetNotFound.
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), metadata.GetTitleID(),
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                    system.GetFileSystemController()));
    }

    is_loaded = true;
    return {ResultStatus::Success,
            LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize()}};
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir) {
    if (romfs == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(std::vector<u8>& out_buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    out_buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(u64& out_program_id) {
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(std::string& out_title) {
    if (name.empty()) {
        return ResultStatus::ErrorNoControl;
    }

    out_title = name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadControlData(FileSys::NACP& out_control) {
    if (!control) {
        return ResultStatus::ErrorNoControl;
    }
    out_control = *control;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadDisplayVersion(std::string& version) {
    if (control && !control->GetVersionString().empty()) {
        version = control->GetVersionString();
        return ResultStatus::Success;
    }
    if (!romfs_version.empty()) {
        version = romfs_version;
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const {
    return loose_exefs != nullptr;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules) {
    if (!is_loaded) {
        return ResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
