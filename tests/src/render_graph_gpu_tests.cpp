// Render graph lifetimes on real GPUs (US-032). The readback chain runs on DX12 and on Vulkan with the debug layers on. Each run checks the
// reference clear color, the transient allocation count, the live-resource baseline after repeated executions, and the timings. Any
// validation error fails the test.

#include "render_graph_fixture.hpp"

#include "engine/render/render_graph.hpp"
#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using engine::render::CompiledGraph;
using engine::render::execute;
using engine::render::ExecutionReport;
using engine::rhi::Backend;
using engine::rhi::BufferHandle;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::Feature;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;

namespace {

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

void require_ok(const RhiStatus& status) {
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

bool near_channel(std::uint8_t actual, float expected) {
    const auto wanted = static_cast<int>(expected * 255.0f + 0.5f);
    const int difference = static_cast<int>(actual) - wanted;
    return difference >= -1 && difference <= 1;
}

DeviceDesc debug_device(Backend backend) {
    DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    return desc;
}

void run_readback_chain(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const BufferHandle readback = take(device->create_buffer(engine::render::tests::chain_readback_desc()));
    const CompiledGraph graph = [&readback] {
        auto result = engine::render::tests::build_chain_graph(readback).compile();
        if (!result.ok()) {
            FAIL(result.errors.front().message);
        }
        return std::move(*result.graph);
    }();
    const auto baseline = take(device->resource_stats());

    // Repeated executions must not grow the live resources: transient memory is created and released each time.
    for (int run = 0; run < 3; ++run) {
        INFO("run " << run);
        const ExecutionReport report = take(execute(*device, graph));
        REQUIRE(report.passes.size() == 4);
        CHECK(report.transient_allocations == 2);
        CHECK(report.aliased_resources == 1);

        const auto stats = take(device->resource_stats());
        CHECK(stats.textures == baseline.textures);
        CHECK(stats.buffers == baseline.buffers);

        // The timings are present exactly when the device has timestamp queries.
        const bool timed = device->capabilities().enabled(Feature::TimestampQueries);
        CHECK(report.gpu_ms.has_value() == timed);
        for (const auto& pass : report.passes) {
            CHECK(pass.gpu_ms.has_value() == timed);
            CHECK(pass.cpu_ms >= 0.0);
        }
        if (timed) {
            CHECK(*report.gpu_ms >= 0.0);
        }
    }

    // The download pass copied the cleared target into the readback buffer, so the reference color is there.
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(engine::render::tests::kChainWidth, engine::rhi::Format::R8G8B8A8Unorm);
    std::vector<std::uint8_t> bytes(engine::render::tests::chain_staging_size());
    require_ok(device->read_buffer(readback, 0, bytes.size(), bytes.data()));
    for (std::uint32_t y : {0u, 31u, 63u}) {
        for (std::uint32_t x : {0u, 31u, 63u}) {
            const std::size_t offset = y * pitch + x * 4;
            INFO("texel " << x << "," << y);
            for (std::size_t channel = 0; channel < 4; ++channel) {
                CHECK(near_channel(bytes[offset + channel], engine::render::tests::kChainClear[channel]));
            }
        }
    }

    require_ok(device->check_debug_messages());
    require_ok(device->destroy(readback));
}

}  // namespace

TEST_CASE("DX12 runs a render graph that reuses transient memory, with stable live resources", "[gpu][dx12]") {
    run_readback_chain(Backend::Dx12);
}

TEST_CASE("Vulkan runs a render graph that reuses transient memory, with stable live resources", "[gpu][vulkan]") {
    run_readback_chain(Backend::Vulkan);
}
