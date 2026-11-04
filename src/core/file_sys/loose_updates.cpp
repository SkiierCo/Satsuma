// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include "core/file_sys/loose_updates.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/loader/loader.h"

namespace FileSys {
namespace {
bool IsTitleDirectory(std::string_view name) {
    return name.size() == 16 && name.find_first_not_of("0123456789abcdefABCDEF") == name.npos;
}

bool MatchesProgram(const VirtualDir& exefs, u64 program_id) {
    if (!exefs || !exefs->GetFile("main")) {
        return false;
    }
    const auto npdm = exefs->GetFile("main.npdm");
    ProgramMetadata metadata;
    return npdm && program_id != 0 && metadata.Load(npdm) == Loader::ResultStatus::Success &&
           metadata.GetTitleID() == program_id;
}

VirtualFile RomFSFile(const VirtualDir& exefs) {
    if (!exefs) {
        return nullptr;
    }
    for (const auto& file : exefs->GetFiles()) {
        if (file->GetExtension() == "romfs") {
            return file;
        }
    }
    const auto parent = exefs->GetParentDirectory();
    const auto directory = parent ? parent->GetSubdirectory("romfs") : nullptr;
    return directory ? CreateRomFS(directory) : nullptr;
}
} // namespace

std::optional<u32> ParseLooseUpdateVersion(std::string_view name) {
    // Sixteen-character directories identify titles, not content versions.
    if (name.empty() || name.size() > 10) {
        return std::nullopt;
    }
    u32 version{};
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), version);
    if (error != std::errc{} || end != name.data() + name.size()) {
        return std::nullopt;
    }
    return version;
}

std::vector<VirtualFile> ListLooseProgramMains(const VirtualDir& root) {
    std::vector<VirtualFile> result;
    if (!root) {
        return result;
    }
    const auto add = [&](const VirtualDir& content) {
        const auto exefs = content ? content->GetSubdirectory("exefs") : nullptr;
        if (auto main = exefs ? exefs->GetFile("main") : nullptr) {
            result.push_back(std::move(main));
        }
    };
    add(root);
    add(root->GetSubdirectory("base"));
    for (const auto& child : root->GetSubdirectories()) {
        if (ParseLooseUpdateVersion(child->GetName())) {
            add(child);
        }
    }
    for (const auto name : {"updates", "update"}) {
        if (const auto updates = root->GetSubdirectory(name)) {
            for (const auto& child : updates->GetSubdirectories()) {
                if (ParseLooseUpdateVersion(child->GetName())) {
                    add(child);
                } else if (IsTitleDirectory(child->GetName())) {
                    for (const auto& version : child->GetSubdirectories()) {
                        if (ParseLooseUpdateVersion(version->GetName())) {
                            add(version);
                        }
                    }
                }
            }
        }
    }
    return result;
}

LooseProgram DiscoverLooseProgram(VirtualDir exefs, u64 program_id) {
    LooseProgram result{.base_exefs = exefs};
    if (!exefs || exefs->GetName() != "exefs" || !MatchesProgram(exefs, program_id)) {
        return result;
    }
    auto root = exefs->GetParentDirectory();
    // <game>/{base,version,updates/version,updates/title/version}/exefs
    for (unsigned depth = 0; root && depth < 4; ++depth) {
        const auto name = root->GetName();
        if (name != "base" && name != "updates" && name != "update" &&
            !ParseLooseUpdateVersion(name) && !IsTitleDirectory(name)) {
            break;
        }
        root = root->GetParentDirectory();
    }
    if (!root) {
        return result;
    }
    result.root = root;
    std::vector<LooseUpdate> versions;
    const auto add_version = [&](const VirtualDir& directory) {
        const auto version = ParseLooseUpdateVersion(directory->GetName());
        const auto candidate = directory->GetSubdirectory("exefs");
        if (version && MatchesProgram(candidate, program_id)) {
            versions.push_back({*version, candidate});
        }
    };
    for (const auto& directory : root->GetSubdirectories()) {
        add_version(directory);
    }
    for (const auto name : {"updates", "update"}) {
        if (const auto updates = root->GetSubdirectory(name)) {
            for (const auto& directory : updates->GetSubdirectories()) {
                add_version(directory);
                if (IsTitleDirectory(directory->GetName())) {
                    for (const auto& version : directory->GetSubdirectories()) {
                        add_version(version);
                    }
                }
            }
        }
    }
    std::stable_sort(versions.begin(), versions.end(),
                     [](const auto& a, const auto& b) { return a.version > b.version; });
    std::erase_if(versions, [last = std::optional<u32>{}](const LooseUpdate& entry) mutable {
        const bool duplicate = last && *last == entry.version;
        last = entry.version;
        return duplicate;
    });

    auto base = root->GetSubdirectory("exefs");
    if (!MatchesProgram(base, program_id)) {
        const auto base_root = root->GetSubdirectory("base");
        base = base_root ? base_root->GetSubdirectory("exefs") : nullptr;
    }
    if (MatchesProgram(base, program_id)) {
        result.base_exefs = base;
        std::erase_if(versions, [](const LooseUpdate& update) { return update.version == 0; });
    } else if (!versions.empty()) {
        // A lone versioned dump remains bootable. Do not pretend an absent base was installed.
        result.base_exefs = versions.back().exefs;
        result.base_version = versions.back().version;
        versions.pop_back();
    }
    result.updates = std::move(versions);
    return result;
}

VirtualFile FindLooseProgramMain(const VirtualDir& root) {
    if (!root) {
        return nullptr;
    }
    if (auto main = root->GetFile("main")) {
        return main;
    }
    VirtualFile selected;
    std::optional<u64> title_id;
    for (const auto& main : ListLooseProgramMains(root)) {
        const auto exefs = main->GetContainingDirectory();
        const auto npdm = exefs->GetFile("main.npdm");
        ProgramMetadata metadata;
        if (!npdm || metadata.Load(npdm) != Loader::ResultStatus::Success ||
            metadata.GetTitleID() == 0) {
            continue;
        }
        if (title_id && *title_id != metadata.GetTitleID()) {
            // A library containing different games is not a single bootable game folder.
            return nullptr;
        }
        title_id = metadata.GetTitleID();
        const auto family = DiscoverLooseProgram(exefs, *title_id);
        selected = family.base_exefs->GetFile("main");
    }
    return selected;
}

const LooseUpdate* SelectLooseUpdate(const LooseProgram& program,
                                   const std::vector<std::string>& disabled) {
    if (std::find(disabled.begin(), disabled.end(), "Update") != disabled.end()) {
        return nullptr;
    }
    for (const auto& update : program.updates) {
        if (std::find(disabled.begin(), disabled.end(), fmt::format("Update@{}", update.version)) ==
            disabled.end()) {
            return &update;
        }
    }
    return nullptr;
}

VirtualDir ResolveLooseExeFS(const LooseProgram& program, const std::vector<std::string>& disabled) {
    const auto* update = SelectLooseUpdate(program, disabled);
    return update ? LayeredVfsDirectory::MakeLayeredDirectory({update->exefs, program.base_exefs})
                  : program.base_exefs;
}

VirtualFile ResolveLooseRomFS(const LooseProgram& program, const std::vector<std::string>& disabled) {
    auto base = RomFSFile(program.base_exefs);
    const auto* update = SelectLooseUpdate(program, disabled);
    auto overlay = update ? RomFSFile(update->exefs) : nullptr;
    if (!overlay) {
        return base;
    }
    if (!base) {
        return overlay;
    }
    auto base_directory = ExtractRomFS(base);
    auto update_directory = ExtractRomFS(overlay);
    if (!base_directory || !update_directory) {
        return nullptr;
    }
    return CreateRomFS(LayeredVfsDirectory::MakeLayeredDirectory(
        {std::move(update_directory), std::move(base_directory)}));
}
} // namespace FileSys
