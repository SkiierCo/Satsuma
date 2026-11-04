// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {
struct LooseUpdate {
    u32 version;
    VirtualDir exefs;
};

struct LooseProgram {
    VirtualDir root;
    VirtualDir base_exefs;
    std::optional<u32> base_version;
    std::vector<LooseUpdate> updates; // Descending version order.
};

// Only bounded, known dump layouts are inspected; each executable must have the same NPDM ID.
[[nodiscard]] std::optional<u32> ParseLooseUpdateVersion(std::string_view name);
[[nodiscard]] std::vector<VirtualFile> ListLooseProgramMains(const VirtualDir& root);
[[nodiscard]] VirtualFile FindLooseProgramMain(const VirtualDir& root);
[[nodiscard]] LooseProgram DiscoverLooseProgram(VirtualDir exefs, u64 program_id);
[[nodiscard]] const LooseUpdate* SelectLooseUpdate(const LooseProgram& program,
                                                 const std::vector<std::string>& disabled);
[[nodiscard]] VirtualDir ResolveLooseExeFS(const LooseProgram& program,
                                         const std::vector<std::string>& disabled);
[[nodiscard]] VirtualFile ResolveLooseRomFS(const LooseProgram& program,
                                          const std::vector<std::string>& disabled);
} // namespace FileSys
