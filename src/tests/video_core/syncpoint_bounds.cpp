// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/syncpoint_manager.h"

TEST_CASE("Out-of-pool syncpoints cannot access counter or callback storage",
          "[video_core][syncpoint_bounds]") {
    Tegra::Host1x::SyncpointManager manager;
    u32 callbacks{};
    manager.IncrementGuest(191);
    manager.IncrementHost(191);
    for (const u32 id : {192U, 0x3d0U, std::numeric_limits<u32>::max()}) {
        manager.IncrementGuest(id);
        manager.IncrementHost(id);
        CHECK(manager.GetGuestSyncpointValue(id) == 0);
        CHECK(manager.GetHostSyncpointValue(id) == 0);
        CHECK_FALSE(manager.IsReadyGuest(id, 0));
        CHECK_FALSE(manager.IsReadyHost(id, 0));
        const auto guest = manager.RegisterGuestAction(id, 0, [&] { ++callbacks; });
        const auto host = manager.RegisterHostAction(id, 0, [&] { ++callbacks; });
        manager.DeregisterGuestAction(id, guest);
        manager.DeregisterHostAction(id, host);
        manager.WaitGuest(id, 1);
        manager.WaitHost(id, 1);
        CHECK(callbacks == 0);
        CHECK(manager.GetGuestSyncpointValue(191) == 1);
        CHECK(manager.GetHostSyncpointValue(191) == 1);
    }
}

TEST_CASE("Valid syncpoint callbacks retain ordering and host/guest independence",
          "[video_core][syncpoint_bounds]") {
    Tegra::Host1x::SyncpointManager manager;
    std::vector<u32> calls;
    manager.RegisterGuestAction(191, 2, [&] { calls.push_back(2); });
    manager.RegisterGuestAction(191, 1, [&] { calls.push_back(1); });
    manager.RegisterHostAction(191, 1, [&] { calls.push_back(3); });
    manager.IncrementGuest(191);
    CHECK(calls == std::vector<u32>{1});
    CHECK_FALSE(manager.IsReadyGuest(191, 2));
    manager.IncrementGuest(191);
    CHECK(calls == std::vector<u32>{1, 2});
    manager.IncrementHost(191);
    CHECK(calls == std::vector<u32>{1, 2, 3});
    CHECK(manager.IsReadyGuest(191, 2));
    CHECK(manager.IsReadyHost(191, 1));
    manager.WaitGuest(191, 2);
    manager.WaitHost(191, 1);
}
