// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>

#include "core/savestate/archive.h"

namespace Service {
class SessionRequestHandler;
class SessionRequestManager;
} // namespace Service

namespace Core::SaveState {

struct ServiceSessionState {
    bool domain{};
    bool convert_on_request_end{};
    bool initialized_for_sm{};
    u64 root_handler{};
    std::vector<u64> domain_handlers;
    auto operator<=>(const ServiceSessionState&) const = default;
};

using HandlerPointer = std::shared_ptr<Service::SessionRequestHandler>;
using HandlerResolver = std::function<HandlerPointer(u64)>;

struct ResolvedServiceSession {
    HandlerPointer root;
    std::vector<HandlerPointer> domains;
};

std::vector<u8> EncodeServiceSession(const ServiceSessionState& state);
ServiceSessionState DecodeServiceSession(std::span<const u8> bytes);
ResolvedServiceSession ResolveServiceSession(const ServiceSessionState& state,
                                             const HandlerResolver& resolve);

class ServiceSessionAccess {
public:
    // The registry must already capture/reconstruct each concrete service handler's own state.
    // These operations serialize session routing and domain references, not request stacks.
    // All service threads must be quiescent; only a fresh candidate manager may be restored.
    static ServiceSessionState Capture(const Service::SessionRequestManager& manager,
                                       const std::function<u64(const HandlerPointer&)>& identify);
    static void RestoreFresh(Service::SessionRequestManager& manager,
                             const ServiceSessionState& state, const HandlerResolver& resolve);
};

} // namespace Core::SaveState
