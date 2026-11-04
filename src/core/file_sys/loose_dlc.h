// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

enum class LooseDlcInstallResult { Success, Overwrite, InvalidContent, CopyFailed, Cancelled };
using LooseDlcProgress = std::function<bool(u64 total, u64 copied)>;

std::optional<u64> ParseLooseDlcTitleId(std::string_view name);
std::optional<u64> FindLooseDlcTitleId(VirtualDir directory);
VirtualDir FindLooseDlcRomFS(const VirtualDir& directory);
bool IsLooseDlcRomFS(const VirtualFile& file);
std::vector<u64> ListInstalledLooseDlc(const VirtualDir& nand_contents);
VirtualFile OpenInstalledLooseDlc(const VirtualDir& nand_contents, u64 title_id);
LooseDlcInstallResult InstallLooseDlc(const VirtualDir& nand_contents, u64 title_id,
                                    const VirtualFile& romfs,
                                    const LooseDlcProgress& progress = {});

} // namespace FileSys
