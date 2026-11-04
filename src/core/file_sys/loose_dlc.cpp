// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <fmt/format.h>
#include "common/scope_exit.h"
#include "common/swap.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/loose_dlc.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

std::optional<u64> ParseLooseDlcTitleId(std::string_view name) {
    if (name.size() != 16) {
        return std::nullopt;
    }
    u64 id{};
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), id, 16);
    if (error != std::errc{} || end != name.data() + name.size() || GetBaseTitleID(id) == 0 ||
        (id & 0x1800) != 0x1000) {
        return std::nullopt;
    }
    return id;
}

std::optional<u64> FindLooseDlcTitleId(VirtualDir directory) {
    for (unsigned depth = 0; directory && depth < 6; ++depth) {
        if (const auto id = ParseLooseDlcTitleId(directory->GetName())) {
            return id;
        }
        directory = directory->GetParentDirectory();
    }
    return std::nullopt;
}

VirtualDir FindLooseDlcRomFS(const VirtualDir& directory) {
    if (!directory) {
        return nullptr;
    }
    if (directory->GetName() == "romfs") {
        return directory;
    }
    if (const auto romfs = directory->GetSubdirectory("romfs")) {
        return romfs;
    }
    VirtualDir result;
    u64 highest{};
    for (const auto& child : directory->GetSubdirectories()) {
        const auto name = child->GetName();
        u64 version{};
        const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), version);
        if (error != std::errc{} || end != name.data() + name.size()) {
            continue;
        }
        if (const auto romfs = child->GetSubdirectory("romfs"); romfs && (!result || version > highest)) {
            result = romfs;
            highest = version;
        }
    }
    return result;
}

bool IsLooseDlcRomFS(const VirtualFile& file) {
    std::array<u64_le, 10> header{};
    if (!file || file->ReadObject(&header) != sizeof(header) || header[0] != sizeof(header)) {
        return false;
    }
    const u64 size = file->GetSize();
    for (size_t i = 1; i < 9; i += 2) {
        const u64 offset = header[i];
        const u64 length = header[i + 1];
        if (offset < sizeof(header) || offset > size || length > size - offset) {
            return false;
        }
    }
    return header[4] >= 0x18 && header[9] >= sizeof(header) && header[9] <= size;
}

VirtualFile OpenInstalledLooseDlc(const VirtualDir& nand_contents, u64 title_id) {
    if (!nand_contents || !ParseLooseDlcTitleId(fmt::format("{:016X}", title_id))) {
        return nullptr;
    }
    const auto root = nand_contents->GetSubdirectory("loose_dlc");
    if (!root) {
        return nullptr;
    }
    const auto stem = fmt::format("{:016X}", title_id);
    for (const auto suffix : {".romfs", ".previous"}) {
        auto file = root->GetFile(stem + suffix);
        if (IsLooseDlcRomFS(file)) {
            return file;
        }
    }
    return nullptr;
}

std::vector<u64> ListInstalledLooseDlc(const VirtualDir& nand_contents) {
    std::vector<u64> result;
    const auto root = nand_contents ? nand_contents->GetSubdirectory("loose_dlc") : nullptr;
    if (!root) {
        return result;
    }
    for (const auto& file : root->GetFiles()) {
        const auto name = file->GetName();
        if (!name.ends_with(".romfs") && !name.ends_with(".previous")) {
            continue;
        }
        if (const auto id = ParseLooseDlcTitleId(std::string_view{name}.substr(0, name.find('.')));
            id && OpenInstalledLooseDlc(nand_contents, *id)) {
            result.push_back(*id);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

LooseDlcInstallResult InstallLooseDlc(const VirtualDir& nand_contents, u64 title_id,
                                    const VirtualFile& romfs, const LooseDlcProgress& progress) {
    if (!nand_contents || !ParseLooseDlcTitleId(fmt::format("{:016X}", title_id)) ||
        !IsLooseDlcRomFS(romfs)) {
        return LooseDlcInstallResult::InvalidContent;
    }
    const auto root = GetOrCreateDirectoryRelative(nand_contents, "loose_dlc");
    if (!root) {
        return LooseDlcInstallResult::CopyFailed;
    }
    const auto stem = fmt::format("{:016X}", title_id);
    const auto target_name = stem + ".romfs";
    const auto pending_name = stem + ".pending";
    const auto previous_name = stem + ".previous";
    // Restore an interrupted replacement before attempting another import.
    if (auto previous = root->GetFile(previous_name)) {
        auto target = root->GetFile(target_name);
        const bool target_valid = IsLooseDlcRomFS(target);
        const bool target_exists = target != nullptr;
        target.reset();
        if (target_valid) {
            previous.reset();
            if (!root->DeleteFile(previous_name)) {
                return LooseDlcInstallResult::CopyFailed;
            }
        } else {
            if ((target_exists && !root->DeleteFile(target_name)) || !previous->Rename(target_name)) {
                return LooseDlcInstallResult::CopyFailed;
            }
        }
    }
    if (root->GetFile(pending_name) && !root->DeleteFile(pending_name)) {
        return LooseDlcInstallResult::CopyFailed;
    }
    auto pending = root->CreateFile(pending_name);
    SCOPE_EXIT {
        pending.reset();
        if (root->GetFile(pending_name)) {
            root->DeleteFile(pending_name);
        }
    };
    if (!pending || !pending->Resize(romfs->GetSize())) {
        return LooseDlcInstallResult::CopyFailed;
    }
    constexpr size_t BlockSize = 0x100000;
    std::vector<u8> buffer(std::min(BlockSize, romfs->GetSize()));
    for (size_t offset = 0; offset < romfs->GetSize();) {
        if (progress && progress(romfs->GetSize(), offset)) {
            return LooseDlcInstallResult::Cancelled;
        }
        const size_t size = std::min(buffer.size(), romfs->GetSize() - offset);
        if (romfs->Read(buffer.data(), size, offset) != size ||
            pending->Write(buffer.data(), size, offset) != size) {
            return LooseDlcInstallResult::CopyFailed;
        }
        offset += size;
    }
    if (!IsLooseDlcRomFS(pending)) {
        return LooseDlcInstallResult::CopyFailed;
    }
    if (progress && progress(romfs->GetSize(), romfs->GetSize())) {
        return LooseDlcInstallResult::Cancelled;
    }
    const bool overwrite = root->GetFile(target_name) != nullptr;
    if (auto target = root->GetFile(target_name); target && !target->Rename(previous_name)) {
        return LooseDlcInstallResult::CopyFailed;
    }
    if (!pending->Rename(target_name)) {
        if (auto previous = root->GetFile(previous_name)) {
            previous->Rename(target_name);
        }
        return LooseDlcInstallResult::CopyFailed;
    }
    if (overwrite) {
        root->DeleteFile(previous_name);
    }
    return overwrite ? LooseDlcInstallResult::Overwrite : LooseDlcInstallResult::Success;
}

} // namespace FileSys
