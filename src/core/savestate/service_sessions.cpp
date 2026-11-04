// SPDX-License-Identifier: GPL-3.0-or-later

#include <unordered_map>

#include "core/hle/service/hle_ipc.h"
#include "core/savestate/service_sessions.h"

namespace Core::SaveState {
namespace {
constexpr u32 MaxDomainHandlers = 65536;

void Validate(const ServiceSessionState& state) {
    if (state.domain_handlers.size() > MaxDomainHandlers ||
        (!state.domain && !state.domain_handlers.empty()) ||
        (state.domain && (state.domain_handlers.empty() || state.convert_on_request_end ||
                          state.root_handler == 0)) ||
        (state.convert_on_request_end && state.root_handler == 0)) {
        throw Error{"Invalid service session domain state"};
    }
    if (state.domain && state.domain_handlers[0] != 0 &&
        state.domain_handlers[0] != state.root_handler) {
        throw Error{"Service domain root does not match its session handler"};
    }
}
} // namespace

std::vector<u8> EncodeServiceSession(const ServiceSessionState& state) {
    Validate(state);
    Encoder output;
    output.U32(1);
    output.U32((state.domain ? 1U : 0U) | (state.convert_on_request_end ? 2U : 0U) |
               (state.initialized_for_sm ? 4U : 0U));
    output.U64(state.root_handler);
    output.U32(static_cast<u32>(state.domain_handlers.size()));
    for (const u64 id : state.domain_handlers) {
        output.U64(id);
    }
    return std::move(output.data);
}

ServiceSessionState DecodeServiceSession(std::span<const u8> bytes) {
    Decoder input{bytes};
    if (input.U32() != 1) {
        throw Error{"Unsupported service session state version"};
    }
    const u32 flags = input.U32();
    if (flags & ~7U) {
        throw Error{"Unsupported service session state flags"};
    }
    ServiceSessionState state;
    state.domain = (flags & 1) != 0;
    state.convert_on_request_end = (flags & 2) != 0;
    state.initialized_for_sm = (flags & 4) != 0;
    state.root_handler = input.U64();
    const u32 count = input.U32();
    if (count > MaxDomainHandlers) {
        throw Error{"Service domain state exceeds its limit"};
    }
    state.domain_handlers.resize(count);
    for (auto& id : state.domain_handlers) {
        id = input.U64();
    }
    input.Finish();
    Validate(state);
    return state;
}

ResolvedServiceSession ResolveServiceSession(const ServiceSessionState& state,
                                             const HandlerResolver& resolve) {
    Validate(state);
    std::unordered_map<u64, HandlerPointer> identities;
    const auto reference = [&](u64 id) -> HandlerPointer {
        if (id == 0) {
            return nullptr;
        }
        auto [entry, inserted] = identities.try_emplace(id);
        if (inserted) {
            entry->second = resolve(id);
            if (!entry->second) {
                throw Error{"Service session refers to a missing checkpoint handler"};
            }
        }
        return entry->second;
    };
    ResolvedServiceSession result;
    result.root = reference(state.root_handler);
    result.domains.reserve(state.domain_handlers.size());
    for (const u64 id : state.domain_handlers) {
        result.domains.push_back(reference(id));
    }
    return result;
}

ServiceSessionState ServiceSessionAccess::Capture(
    const Service::SessionRequestManager& manager,
    const std::function<u64(const HandlerPointer&)>& identify) {
    const auto id = [&](const HandlerPointer& handler) -> u64 {
        if (!handler) {
            return 0;
        }
        const u64 identity = identify(handler);
        if (identity == 0) {
            throw Error{"Service handler has no checkpoint identity"};
        }
        return identity;
    };
    ServiceSessionState state{manager.is_domain,
                              manager.convert_to_domain,
                              manager.is_initialized_for_sm,
                              id(manager.session_handler),
                              {}};
    if (manager.domain_handlers.size() > MaxDomainHandlers) {
        throw Error{"Service domain state exceeds its limit"};
    }
    state.domain_handlers.reserve(manager.domain_handlers.size());
    for (const auto& handler : manager.domain_handlers) {
        state.domain_handlers.push_back(id(handler));
    }
    Validate(state);
    return state;
}

void ServiceSessionAccess::RestoreFresh(Service::SessionRequestManager& manager,
                                        const ServiceSessionState& state,
                                        const HandlerResolver& resolve) {
    if (manager.is_domain || manager.convert_to_domain || manager.is_initialized_for_sm ||
        manager.session_handler || !manager.domain_handlers.empty()) {
        throw Error{"Service session restore requires a fresh manager"};
    }
    auto resolved = ResolveServiceSession(state, resolve);
    manager.session_handler = std::move(resolved.root);
    manager.domain_handlers = std::move(resolved.domains);
    manager.is_domain = state.domain;
    manager.convert_to_domain = state.convert_on_request_end;
    manager.is_initialized_for_sm = state.initialized_for_sm;
}

} // namespace Core::SaveState
