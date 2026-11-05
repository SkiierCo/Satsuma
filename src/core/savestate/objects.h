// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/savestate/archive.h"

namespace Core::SaveState {

// Capture: stable identities for kernel objects. IDs start at 1 and follow discovery order.
// A component that recreates an object itself on restore (a service's event, shared memory or
// session) marks it as provided, so the kernel component does not try to rebuild it.
class ObjectIds {
public:
    u64 Id(Kernel::KAutoObject* object) {
        if (object == nullptr) {
            return 0;
        }
        const auto [it, inserted] = ids.try_emplace(object, ids.size() + 1);
        if (inserted) {
            order.push_back(object);
        }
        return it->second;
    }
    u64 Provide(Kernel::KAutoObject* object, std::string_view owner) {
        const u64 id = Id(object);
        providers.try_emplace(object, owner);
        return id;
    }
    bool IsProvided(Kernel::KAutoObject* object) const {
        return providers.contains(object);
    }
    bool Has(Kernel::KAutoObject* object) const {
        return ids.contains(object);
    }
    // Objects in discovery order; the vector can grow while a caller walks it by index.
    const std::vector<Kernel::KAutoObject*>& Objects() const {
        return order;
    }

private:
    std::unordered_map<Kernel::KAutoObject*, u64> ids;
    std::unordered_map<Kernel::KAutoObject*, std::string> providers;
    std::vector<Kernel::KAutoObject*> order;
};

// Restore: saved identity -> recreated object.
class ObjectTable {
public:
    void Bind(u64 id, Kernel::KAutoObject* object) {
        if (id == 0 || object == nullptr) {
            throw Error("Invalid object binding in save state");
        }
        if (!objects.try_emplace(id, object).second) {
            throw Error(fmt::format("Save state object {} was restored twice", id));
        }
    }
    Kernel::KAutoObject* Find(u64 id) const {
        const auto it = objects.find(id);
        return it == objects.end() ? nullptr : it->second;
    }
    template <typename T>
    T* Get(u64 id) const {
        if (id == 0) {
            return nullptr;
        }
        Kernel::KAutoObject* object = Find(id);
        if (object == nullptr) {
            throw Error(fmt::format("Save state object {} was not restored", id));
        }
        T* typed = object->DynamicCast<T*>();
        if (typed == nullptr) {
            throw Error(fmt::format("Save state object {} is a {}, not a {}", id,
                                    object->GetTypeName(), T::GetStaticTypeName()));
        }
        return typed;
    }

private:
    std::unordered_map<u64, Kernel::KAutoObject*> objects;
};

} // namespace Core::SaveState
