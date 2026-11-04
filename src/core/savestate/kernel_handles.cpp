// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <unordered_map>

#include "core/hle/kernel/k_handle_table.h"
#include "core/savestate/kernel_handles.h"

namespace Core::SaveState {
namespace {
void Validate(const HandleTableState& table) {
    const auto size = table.slots.size();
    if (size == 0 || size > Kernel::KHandleTable::MaxTableSize || table.allocated_count > size ||
        table.maximum_count < table.allocated_count || table.maximum_count > size ||
        table.next_serial == 0 || table.next_serial > 0x7fff) {
        throw Error{"Invalid kernel handle table dimensions or counters"};
    }
    std::vector<bool> free(size);
    u32 free_count{};
    for (s32 index = table.free_head; index != -1;) {
        if (index < 0 || static_cast<size_t>(index) >= size || free[index]) {
            throw Error{"Invalid or cyclic kernel handle free list"};
        }
        free[index] = true;
        ++free_count;
        const auto& slot = table.slots[index];
        if (slot.object_id != 0 || slot.entry_info > 0xffff) {
            throw Error{"Kernel handle free list contains an object or invalid link"};
        }
        index = slot.entry_info == 0xffff ? -1 : static_cast<s32>(slot.entry_info);
    }
    if (size - free_count != table.allocated_count) {
        throw Error{"Kernel handle allocation count disagrees with its free list"};
    }
    for (size_t index = 0; index < size; ++index) {
        const auto& slot = table.slots[index];
        if (slot.entry_info > 0xffff ||
            (slot.object_id != 0 && (slot.entry_info == 0 || slot.entry_info > 0x7fff))) {
            throw Error{"Invalid kernel handle serial"};
        }
        // Allocated null slots are reservations awaiting IPC handle registration. Their
        // EntryInfo still contains the previous free link until Register assigns its serial.
    }
}
} // namespace

std::vector<u8> EncodeHandleTable(const HandleTableState& table) {
    Validate(table);
    Encoder output;
    output.U32(1);
    output.U32(static_cast<u32>(table.slots.size()));
    output.U32(table.allocated_count);
    output.U32(table.maximum_count);
    output.U32(table.next_serial);
    output.U32(std::bit_cast<u32>(table.free_head));
    for (const auto& slot : table.slots) {
        output.U64(slot.object_id);
        output.U32(slot.entry_info);
    }
    return std::move(output.data);
}

HandleTableState DecodeHandleTable(std::span<const u8> bytes) {
    Decoder input{bytes};
    if (input.U32() != 1) {
        throw Error{"Unsupported kernel handle table version"};
    }
    const auto size = input.U32();
    if (size == 0 || size > Kernel::KHandleTable::MaxTableSize) {
        throw Error{"Kernel handle table exceeds its limit"};
    }
    HandleTableState table;
    table.allocated_count = input.U32();
    table.maximum_count = input.U32();
    table.next_serial = input.U32();
    table.free_head = std::bit_cast<s32>(input.U32());
    table.slots.resize(size);
    for (auto& slot : table.slots) {
        slot.object_id = input.U64();
        slot.entry_info = input.U32();
    }
    input.Finish();
    Validate(table);
    return table;
}

HandleTableState HandleTableAccess::Capture(
    const Kernel::KHandleTable& table, const std::function<u64(Kernel::KAutoObject*)>& identify) {
    HandleTableState state{
        table.m_count, table.m_max_count, table.m_next_linear_id, table.m_free_head_index, {}};
    if (table.m_table_size > Kernel::KHandleTable::MaxTableSize) {
        throw Error{"Kernel handle table exceeds its limit"};
    }
    state.slots.reserve(table.m_table_size);
    for (size_t index = 0; index < table.m_table_size; ++index) {
        const auto object = table.m_objects[index];
        const u64 id = object ? identify(object) : 0;
        if (object && id == 0) {
            throw Error{"Kernel object has no checkpoint identity"};
        }
        state.slots.push_back({id, table.m_entry_infos[index].GetLinearId()});
    }
    Validate(state);
    return state;
}

void HandleTableAccess::RestoreFresh(Kernel::KernelCore& kernel, Kernel::KHandleTable& table,
                                     const HandleTableState& state,
                                     const std::function<Kernel::KAutoObject*(u64)>& resolve) {
    Validate(state);
    if (table.m_count != 0 || table.m_max_count != 0 ||
        std::ranges::any_of(table.m_objects, [](const auto object) { return object != nullptr; })) {
        throw Error{"Kernel handle restore requires a fresh table"};
    }
    std::array<Kernel::KAutoObject*, Kernel::KHandleTable::MaxTableSize> objects{};
    std::unordered_map<u64, Kernel::KAutoObject*> identities;
    for (size_t index = 0; index < state.slots.size(); ++index) {
        const u64 id = state.slots[index].object_id;
        if (id == 0) {
            continue;
        }
        auto [entry, inserted] = identities.try_emplace(id, nullptr);
        if (inserted) {
            entry->second = resolve(id);
            if (!entry->second) {
                throw Error{"Kernel handle refers to a missing checkpoint object"};
            }
        }
        objects[index] = entry->second;
    }
    // Establish references only after every ID resolves. Failure leaves the table untouched.
    for (size_t index = 0; index < state.slots.size(); ++index) {
        if (objects[index] && !objects[index]->Open(kernel)) {
            for (size_t prior = 0; prior < index; ++prior) {
                if (objects[prior]) {
                    objects[prior]->Close(kernel);
                }
            }
            throw Error{"Kernel handle refers to an inactive checkpoint object"};
        }
    }
    table.m_objects = objects;
    for (size_t index = 0; index < state.slots.size(); ++index) {
        table.m_entry_infos[index].linear_id = static_cast<u16>(state.slots[index].entry_info);
    }
    table.m_table_size = static_cast<u16>(state.slots.size());
    table.m_count = static_cast<u16>(state.allocated_count);
    table.m_max_count = static_cast<u16>(state.maximum_count);
    table.m_next_linear_id = static_cast<u16>(state.next_serial);
    table.m_free_head_index = state.free_head;
}

} // namespace Core::SaveState
