// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/control/channel_state.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace {
using Maxwell3D = Tegra::Engines::Maxwell3D;

class DrawRecorder final : public VideoCore::RasterizerInterface {
public:
    explicit DrawRecorder(Maxwell3D& engine_) : engine{engine_} {}

    struct DrawCall {
        u32 base_instance;
        u32 instances;
        u32 first;
        u32 vertices;
    };
    std::vector<DrawCall> calls;

    void Draw(bool, u32 instances) override {
        const auto& state = engine.draw_manager.draw_state;
        calls.push_back({state.base_instance, instances, state.vertex_buffer.first,
                         state.vertex_buffer.count});
    }
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32, u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override { return false; }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr, u64) override { return {}; }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override { return false; }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        throw std::logic_error{"DMA is not part of the draw fixture"};
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

private:
    Maxwell3D& engine;
};

struct DrawFixture {
    Core::System system;
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager device_manager{device_memory};
    Tegra::MemoryManager memory{system, device_manager, 36};
    Maxwell3D engine{memory};
    DrawRecorder rasterizer{engine};

    DrawFixture() { engine.BindRasterizer(&rasterizer); }

    void Write(u32 method, u32 value) { engine.CallMethod(system, method, value, true); }
};
} // Anonymous namespace

TEST_CASE("Immediate array instances are not replayed as deferred draws", "[video_core][draw_instances]") {
    const auto fixture = std::make_unique<DrawFixture>();
    auto& engine = fixture->engine;
    Maxwell3D::Regs::VertexArray command{};
    command.topology.Assign(Maxwell3D::Regs::PrimitiveTopology::Triangles);
    command.start.Assign(7);
    command.count.Assign(3);
    fixture->Write(MAXWELL3D_REG_INDEX(vertex_array_instance_first), std::bit_cast<u32>(command));
    REQUIRE(fixture->rasterizer.calls.size() == 1);
    CHECK(fixture->rasterizer.calls[0].base_instance == 0);
    CHECK_FALSE(engine.draw_manager.HasDeferredDraw());

    // A state write and macro-end flush must not replay an already submitted draw.
    fixture->Write(MAXWELL3D_REG_INDEX(global_base_instance_index), 11);
    engine.draw_manager.DrawDeferred(engine);
    REQUIRE(fixture->rasterizer.calls.size() == 1);
    fixture->Write(MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent), std::bit_cast<u32>(command));
    engine.draw_manager.DrawDeferred(engine);
    REQUIRE(fixture->rasterizer.calls.size() == 2);
    CHECK(fixture->rasterizer.calls[1].base_instance == 1);
    for (const auto& call : fixture->rasterizer.calls) {
        CHECK(call.instances == 1);
        CHECK(call.first == 7);
        CHECK(call.vertices == 3);
    }
}

TEST_CASE("Legacy begin and end instances still flush once before state changes", "[video_core][draw_instances]") {
    const auto fixture = std::make_unique<DrawFixture>();
    auto& engine = fixture->engine;
    auto& manager = engine.draw_manager;
    engine.regs.draw.instance_id.Assign(Maxwell3D::Regs::Draw::InstanceId::First);
    engine.regs.draw.topology.Assign(Maxwell3D::Regs::PrimitiveTopology::Triangles);
    manager.DrawBegin(engine);
    manager.DrawEnd(engine);
    engine.regs.draw.instance_id.Assign(Maxwell3D::Regs::Draw::InstanceId::Subsequent);
    manager.DrawBegin(engine);
    manager.DrawEnd(engine);
    REQUIRE(manager.HasDeferredDraw());
    fixture->Write(MAXWELL3D_REG_INDEX(global_base_instance_index), 19);
    REQUIRE(fixture->rasterizer.calls.size() == 2);
    CHECK(fixture->rasterizer.calls[0].instances == 1);
    CHECK(fixture->rasterizer.calls[1].instances == 2);
    CHECK(fixture->rasterizer.calls[1].base_instance == 0);
    CHECK_FALSE(manager.HasDeferredDraw());
    manager.DrawDeferred(engine);
    CHECK(fixture->rasterizer.calls.size() == 2);
}

TEST_CASE("Array instance identity survives deferred batch resets", "[video_core][draw_instances]") {
    const auto fixture = std::make_unique<DrawFixture>();
    auto& engine = fixture->engine;
    auto& manager = engine.draw_manager;
    engine.regs.draw.instance_id.Assign(Maxwell3D::Regs::Draw::InstanceId::First);
    engine.regs.draw.topology.Assign(Maxwell3D::Regs::PrimitiveTopology::Triangles);
    manager.DrawBegin(engine);
    manager.DrawEnd(engine);
    engine.regs.draw.instance_id.Assign(Maxwell3D::Regs::Draw::InstanceId::Subsequent);
    manager.DrawBegin(engine);
    manager.DrawEnd(engine);
    REQUIRE(manager.HasDeferredDraw());
    manager.DrawArrayInstanced(engine, Maxwell3D::Regs::PrimitiveTopology::Triangles, 7, 9, true);
    REQUIRE(fixture->rasterizer.calls.size() == 3);
    CHECK(fixture->rasterizer.calls[1].instances == 2);
    CHECK(fixture->rasterizer.calls[2].instances == 1);
    CHECK(fixture->rasterizer.calls[2].base_instance == 2);
    CHECK_FALSE(manager.HasDeferredDraw());
    CHECK(manager.draw_state.draw_element_count == 9);
    manager.DrawArrayInstanced(engine, Maxwell3D::Regs::PrimitiveTopology::Triangles, 0, 3, false);
    CHECK(fixture->rasterizer.calls.back().base_instance == 0);
    CHECK(manager.draw_state.draw_element_count == 3);
}

TEST_CASE("Vulkan shutdown invalidation detaches channel dirty flags", "[video_core][shutdown]") {
    const auto fixture = std::make_unique<DrawFixture>();
    Tegra::Control::ChannelState channel{1};
    channel.payload.emplace(fixture->system, fixture->memory, channel);
    Vulkan::StateTracker tracker;
    tracker.ChangeChannel(channel);
    auto& flags = channel.payload->maxwell_3d.dirty.flags;
    flags.reset();
    tracker.UnbindChannel();
    tracker.InvalidateCommandBufferState();
    CHECK(flags.none());
    channel.payload.reset();
    tracker.InvalidateCommandBufferState();
    tracker.InvalidateState();
}

TEST_CASE("Invalid GPU descriptor addresses do not index host page tables", "[video_core][gpu_bounds]") {
    const auto fixture = std::make_unique<DrawFixture>();
    auto& memory = fixture->memory;
    constexpr u64 end = 1ULL << 36;
    for (const u64 address : {end, end + 0x1234, 0x1956fad0000ULL,
                             std::numeric_limits<u64>::max() - 7}) {
        std::array<u8, 32> data;
        data.fill(0xcc);
        memory.ReadBlockUnsafe(address, data.data(), data.size());
        CHECK(std::ranges::all_of(data, [](u8 value) { return value == 0; }));
        CHECK_FALSE(memory.IsGranularRange(address, data.size()));
        CHECK_FALSE(memory.IsContinuousRange(address, data.size()));
        CHECK_FALSE(memory.IsFullyMappedRange(address, data.size()));
        CHECK_FALSE(memory.GpuToCpuAddress(address));
        data.fill(0xcc);
        memory.ReadBlockUnsafe(address, data.data(), 0);
        CHECK(data.front() == 0xcc);
    }
}

TEST_CASE("GPU range queries reject a tail beyond the address space", "[video_core][gpu_bounds]") {
    const auto fixture = std::make_unique<DrawFixture>();
    auto& memory = fixture->memory;
    memory.BindRasterizer(&fixture->rasterizer);
    constexpr u64 end = 1ULL << 36;
    memory.MapSparse(end - 0x1000, 0x1000, false);
    CHECK(memory.IsFullyMappedRange(end - 32, 32));
    CHECK_FALSE(memory.IsFullyMappedRange(end - 16, 32));
    CHECK_FALSE(memory.IsContinuousRange(end - 16, 32));
    std::array<u8, 32> data;
    data.fill(0xcc);
    memory.ReadBlockUnsafe(end - 16, data.data(), data.size());
    CHECK(std::ranges::all_of(data, [](u8 value) { return value == 0; }));
    CHECK(memory.IsFullyMappedRange(end - 32, 32));
}
