// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/hle/service/hle_ipc.h"
#include "core/savestate/service_sessions.h"

namespace {
using namespace Core::SaveState;

class TestHandler : public Service::SessionRequestHandler {
public:
    explicit TestHandler(Kernel::KernelCore& kernel)
        : SessionRequestHandler{kernel, "state-test"} {}
    Result HandleSyncRequest(Kernel::KServerSession&, Service::HLERequestContext&) override {
        return ResultSuccess;
    }
};

ServiceSessionState ExampleSession() {
    return {true, false, true, 7, {7, 0, 8, 7}};
}

TEST_CASE("Service session state preserves domain holes, aliases and initialization flags",
          "[core][savestate][service_sessions]") {
    const auto state = ExampleSession();
    CHECK(DecodeServiceSession(EncodeServiceSession(state)) == state);
    const ServiceSessionState pending{false, true, false, 7, {}};
    CHECK(DecodeServiceSession(EncodeServiceSession(pending)) == pending);
    auto closed_root = state;
    closed_root.domain_handlers[0] = 0;
    CHECK(DecodeServiceSession(EncodeServiceSession(closed_root)) == closed_root);
}

TEST_CASE("Invalid domain routing cannot be serialized", "[core][savestate][service_sessions]") {
    auto state = ExampleSession();
    SECTION("Non-domain with virtual objects") {
        state.domain = false;
    }
    SECTION("Domain without a root") {
        state.root_handler = 0;
    }
    SECTION("Domain with no slots") {
        state.domain_handlers.clear();
    }
    SECTION("Mismatched root") {
        state.domain_handlers[0] = 9;
    }
    SECTION("Already converted with a pending conversion") {
        state.convert_on_request_end = true;
    }
    SECTION("Oversized domain table") {
        state.domain_handlers.resize(65537);
    }
    CHECK_THROWS_AS(EncodeServiceSession(state), Error);
}

TEST_CASE("Service session decoder rejects truncated and future schemas",
          "[core][savestate][service_sessions]") {
    const auto saved = EncodeServiceSession(ExampleSession());
    for (size_t size = 0; size < saved.size(); ++size) {
        CHECK_THROWS_AS(DecodeServiceSession(std::span{saved}.first(size)), Error);
    }
    auto invalid = saved;
    invalid[0] = 2;
    CHECK_THROWS_AS(DecodeServiceSession(invalid), Error);
    invalid = saved;
    invalid[4] = 0x80;
    CHECK_THROWS_AS(DecodeServiceSession(invalid), Error);
    invalid = saved;
    invalid.push_back(0);
    CHECK_THROWS_AS(DecodeServiceSession(invalid), Error);
}

TEST_CASE("Service handler resolution preserves shared ownership and closed domain slots",
          "[core][savestate][service_sessions]") {
    Core::System system;
    auto root = std::make_shared<TestHandler>(system.Kernel());
    auto child = std::make_shared<TestHandler>(system.Kernel());
    u32 resolutions{};
    auto resolved = ResolveServiceSession(ExampleSession(), [&](u64 id) -> HandlerPointer {
        ++resolutions;
        return id == 7 ? root : child;
    });
    CHECK(resolutions == 2);
    CHECK(resolved.root == root);
    REQUIRE(resolved.domains.size() == 4);
    CHECK(resolved.domains[0] == root);
    CHECK_FALSE(resolved.domains[1]);
    CHECK(resolved.domains[2] == child);
    CHECK(resolved.domains[3] == root);
    CHECK(root.use_count() == 4);
    CHECK(child.use_count() == 2);
}

TEST_CASE("Failed service resolution releases all temporary handler references",
          "[core][savestate][service_sessions]") {
    Core::System system;
    auto root = std::make_shared<TestHandler>(system.Kernel());
    CHECK_THROWS_AS(
        ResolveServiceSession(ExampleSession(),
                              [&](u64 id) -> HandlerPointer { return id == 7 ? root : nullptr; }),
        Error);
    CHECK(root.use_count() == 1);
}
} // namespace
