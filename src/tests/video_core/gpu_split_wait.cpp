// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "video_core/gpu_split.h"

namespace VideoCommon {
struct GpuSplitWaitTest {
    static void Run(bool drain) {
        using namespace std::chrono_literals;
        GpuSplit split;
        constexpr u64 rounds = 4096;
        std::atomic<bool> done{}, rescued{};
        std::atomic<u32> slept{};
        u64 payload{};
        u32 errors{};
        std::jthread consumer{[&] {
            for (u64 round = 1; round <= rounds && !done.load(); ++round) {
                while (split.published.load(std::memory_order_acquire) < round && !done.load())
                    std::this_thread::yield();
                // Force a real sleeping waiter every sixteen batches; other batches
                // exercise progress racing the front's initial look and flag store.
                if (round % 16 == 0) {
                    while (!split.front_waiting.load(std::memory_order_acquire) && !done.load())
                        std::this_thread::yield();
                    ++slept;
                }
                if (done.load()) break;
                payload = round ^ 0x12345678;
                split.StoreConsumed(round);
            }
        }};
        // A lost wakeup must fail the test instead of hanging the regression suite.
        std::jthread watchdog{[&] {
            u64 previous{};
            auto progress = std::chrono::steady_clock::now();
            while (!done.load()) {
                std::this_thread::sleep_for(10ms);
                const u64 current = split.consumed.load();
                if (current != previous) {
                    previous = current;
                    progress = std::chrono::steady_clock::now();
                } else if (std::chrono::steady_clock::now() - progress > 5s) {
                    rescued.store(true);
                    done.store(true);
                    split.consumed.store(split.published.load());
                    split.consumed.notify_one();
                    break;
                }
            }
        }};
        for (u64 round = 1; round <= rounds; ++round) {
            split.write_position = round;
            if (drain) split.Drain();
            else split.WaitForSpace(GpuSplit::RingWords);
            if (rescued.load()) break;
            if (payload != (round ^ 0x12345678) || split.consumed_cache != round) ++errors;
        }
        done.store(true);
        consumer.join();
        watchdog.join();
        REQUIRE_FALSE(rescued.load());
        REQUIRE(errors == 0);
        REQUIRE(slept.load() == rounds / 16);
        REQUIRE_FALSE(split.front_waiting.load());
        if (drain) REQUIRE(split.publish_every == 8);
    }
};
} // namespace VideoCommon

TEST_CASE("GPU split drain wakes after back-thread progress", "[video_core][gpu_split]") {
    VideoCommon::GpuSplitWaitTest::Run(true);
}

TEST_CASE("GPU split capacity waits preserve ordered results", "[video_core][gpu_split]") {
    VideoCommon::GpuSplitWaitTest::Run(false);
}
