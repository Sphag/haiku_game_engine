// Render graph compilation and execution on the CPU reference device (US-032). Compilation needs no device, so these run on every
// operating system. Execution uses the Validation backend, which enforces the RHI contract without a GPU.

#include "render_graph_fixture.hpp"

#include "engine/render/render_graph.hpp"
#include "engine/rhi/device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using engine::render::Access;
using engine::render::CompileResult;
using engine::render::CompiledGraph;
using engine::render::CompiledPass;
using engine::render::execute;
using engine::render::ExecutionReport;
using engine::render::GraphError;
using engine::render::GraphErrorKind;
using engine::render::PassContext;
using engine::render::PassKind;
using engine::render::RenderGraph;
using engine::render::ResourceAccess;
using engine::render::ResourceId;
using engine::rhi::Backend;
using engine::rhi::BufferDesc;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ResourceState;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;

namespace {

// Handles for compile-only tests. The graph never calls the device at compile time, so a valid-looking handle is enough.
TextureHandle fake_texture(std::uint32_t index) {
    return TextureHandle{index, 1, 1};
}

TextureDesc color_texture(TextureUsage usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource) {
    TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.format = engine::rhi::Format::R8G8B8A8Unorm;
    desc.usage = usage;
    return desc;
}

BufferDesc storage_buffer() {
    BufferDesc desc;
    desc.size_bytes = 256;
    desc.usage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource;
    return desc;
}

// Returns the first error of the given kind, or fails the test when there is none.
const GraphError& error_of(const CompileResult& result, GraphErrorKind kind) {
    const GraphError* found = nullptr;
    for (const GraphError& error : result.errors) {
        if (error.kind == kind) {
            found = &error;
            break;
        }
    }
    REQUIRE(found != nullptr);
    return *found;
}

bool names_include(const GraphError& error, const std::string& name) {
    return std::find(error.passes.begin(), error.passes.end(), name) != error.passes.end();
}

const CompiledGraph& graph_of(const CompileResult& result) {
    if (!result.ok()) {
        FAIL("compile failed: " << result.errors.front().message);
    }
    return *result.graph;
}

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

std::unique_ptr<Device> validation_device() {
    DeviceDesc desc;
    desc.backend = Backend::Validation;
    return take(engine::rhi::create_device(desc));
}

}  // namespace

TEST_CASE("Passes run in data order, whatever their declaration order", "[unit]") {
    RenderGraph graph;
    const ResourceId color = graph.create_texture("color", color_texture());
    graph.add_pass("consume", PassKind::Graphics, {{color, Access::ShaderRead}});
    graph.add_pass("produce", PassKind::Graphics, {{color, Access::ColorWrite}});
    const CompileResult result = graph.compile();
    REQUIRE(result.ok());

    const CompiledGraph& compiled = graph_of(result);
    REQUIRE(compiled.passes.size() == 2);
    CHECK(compiled.passes[0].name == "produce");
    CHECK(compiled.passes[1].name == "consume");
    REQUIRE(compiled.edges.size() == 1);
    CHECK(compiled.edges[0].from == 1);  // declaration index of "produce"
    CHECK(compiled.edges[0].to == 0);    // declaration index of "consume"
}

TEST_CASE("A cycle is rejected, and the error names every pass on it", "[unit]") {
    RenderGraph graph;
    const ResourceId x = graph.create_texture("x", color_texture());
    const ResourceId y = graph.create_texture("y", color_texture());
    graph.add_pass("alpha", PassKind::Graphics, {{x, Access::ShaderRead}, {y, Access::ColorWrite}});
    graph.add_pass("beta", PassKind::Graphics, {{y, Access::ShaderRead}, {x, Access::ColorWrite}});
    const CompileResult result = graph.compile();

    REQUIRE_FALSE(result.ok());
    CHECK_FALSE(result.graph.has_value());
    const GraphError& error = error_of(result, GraphErrorKind::Cycle);
    CHECK(names_include(error, "alpha"));
    CHECK(names_include(error, "beta"));
    CHECK(error.message == "cycle: 'alpha' -> 'beta' -> 'alpha'");
}

TEST_CASE("A transient resource that is read before any pass writes it is rejected, and the error names the reader", "[unit]") {
    RenderGraph graph;
    const ResourceId color = graph.create_texture("color", color_texture());
    graph.add_pass("reader", PassKind::Graphics, {{color, Access::ShaderRead}});
    const CompileResult result = graph.compile();

    REQUIRE_FALSE(result.ok());
    const GraphError& error = error_of(result, GraphErrorKind::ReadBeforeWrite);
    CHECK(names_include(error, "reader"));
    CHECK(error.resource == "color");
}

TEST_CASE("An imported resource may be read without a writer, since it already has content", "[unit]") {
    RenderGraph graph;
    const ResourceId imported = graph.import_texture("history", fake_texture(7), color_texture(TextureUsage::ShaderResource),
                                                     ResourceState::ShaderRead);
    graph.add_pass("sample", PassKind::Graphics, {{imported, Access::ShaderRead}});
    CHECK(graph.compile().ok());
}

TEST_CASE("Incompatible usage is rejected, and the error names the pass and the resource", "[unit]") {
    SECTION("a compute pass cannot draw into a render target") {
        RenderGraph graph;
        const ResourceId color = graph.create_texture("color", color_texture());
        graph.add_pass("shade", PassKind::Compute, {{color, Access::ColorWrite}});
        const CompileResult result = graph.compile();
        REQUIRE_FALSE(result.ok());
        const GraphError& error = error_of(result, GraphErrorKind::IncompatibleUsage);
        CHECK(names_include(error, "shade"));
        CHECK(error.resource == "color");
    }
    SECTION("a texture without the render target flag cannot be written as one") {
        RenderGraph graph;
        const ResourceId color = graph.create_texture("color", color_texture(TextureUsage::ShaderResource));
        graph.add_pass("draw", PassKind::Graphics, {{color, Access::ColorWrite}});
        const CompileResult result = graph.compile();
        REQUIRE_FALSE(result.ok());
        const GraphError& error = error_of(result, GraphErrorKind::IncompatibleUsage);
        CHECK(names_include(error, "draw"));
        CHECK(error.message.find("usage flags") != std::string::npos);
    }
    SECTION("a buffer cannot be a render target") {
        RenderGraph graph;
        const ResourceId buffer = graph.create_buffer("scratch", storage_buffer());
        graph.add_pass("draw", PassKind::Graphics, {{buffer, Access::ColorWrite}});
        const CompileResult result = graph.compile();
        REQUIRE_FALSE(result.ok());
        CHECK(names_include(error_of(result, GraphErrorKind::IncompatibleUsage), "draw"));
    }
    SECTION("a pass cannot declare the same resource twice") {
        RenderGraph graph;
        const ResourceId buffer = graph.create_buffer("scratch", storage_buffer());
        graph.add_pass("loop", PassKind::Compute, {{buffer, Access::StorageWrite}, {buffer, Access::ShaderRead}});
        const CompileResult result = graph.compile();
        REQUIRE_FALSE(result.ok());
        CHECK(names_include(error_of(result, GraphErrorKind::IncompatibleUsage), "loop"));
    }
}

TEST_CASE("Two passes that write the same resource are rejected, and the error names both", "[unit]") {
    RenderGraph graph;
    const ResourceId color = graph.create_texture("color", color_texture());
    graph.add_pass("first", PassKind::Graphics, {{color, Access::ColorWrite}});
    graph.add_pass("second", PassKind::Graphics, {{color, Access::ColorWrite}});
    const CompileResult result = graph.compile();

    REQUIRE_FALSE(result.ok());
    const GraphError& error = error_of(result, GraphErrorKind::MultipleWriters);
    CHECK(names_include(error, "first"));
    CHECK(names_include(error, "second"));
}

TEST_CASE("Duplicate pass and resource names are rejected", "[unit]") {
    RenderGraph graph;
    const ResourceId color = graph.create_texture("color", color_texture());
    graph.create_texture("color", color_texture());
    graph.add_pass("pass", PassKind::Graphics, {{color, Access::ColorWrite}});
    graph.add_pass("pass", PassKind::Graphics, {});
    const CompileResult result = graph.compile();

    REQUIRE_FALSE(result.ok());
    CHECK(names_include(error_of(result, GraphErrorKind::DuplicateName), "pass"));
}

TEST_CASE("An access to a resource from another graph is rejected", "[unit]") {
    RenderGraph other;
    const ResourceId foreign = other.create_texture("foreign", color_texture());
    other.create_texture("foreign_2", color_texture());
    RenderGraph graph;
    graph.add_pass("stray", PassKind::Graphics, {{foreign, Access::ColorWrite}});
    const CompileResult result = graph.compile();

    REQUIRE_FALSE(result.ok());
    CHECK(names_include(error_of(result, GraphErrorKind::InvalidResource), "stray"));
}

TEST_CASE("Barriers move each resource from the state its last user left it in", "[unit]") {
    RenderGraph graph;
    const ResourceId color = graph.create_texture("color", color_texture());
    graph.add_pass("draw", PassKind::Graphics, {{color, Access::ColorWrite}});
    graph.add_pass("sample", PassKind::Graphics, {{color, Access::ShaderRead}});
    const CompileResult result = graph.compile();
    const CompiledGraph& compiled = graph_of(result);

    REQUIRE(compiled.passes.size() == 2);
    REQUIRE(compiled.passes[0].barriers.size() == 1);
    CHECK(compiled.passes[0].barriers[0].before == ResourceState::Common);
    CHECK(compiled.passes[0].barriers[0].after == ResourceState::RenderTarget);
    REQUIRE(compiled.passes[1].barriers.size() == 1);
    CHECK(compiled.passes[1].barriers[0].before == ResourceState::RenderTarget);
    CHECK(compiled.passes[1].barriers[0].after == ResourceState::ShaderRead);
    CHECK(compiled.passes[0].barriers[0].slot == compiled.passes[1].barriers[0].slot);
}

TEST_CASE("A pass needs no barrier for a resource already in its state", "[unit]") {
    RenderGraph graph;
    const ResourceId buffer = graph.create_buffer("scratch", storage_buffer());
    graph.add_pass("write", PassKind::Compute, {{buffer, Access::StorageWrite}});
    graph.add_pass("read_first", PassKind::Compute, {{buffer, Access::ShaderRead}});
    graph.add_pass("read_second", PassKind::Compute, {{buffer, Access::ShaderRead}});
    const CompileResult result = graph.compile();
    const CompiledGraph& compiled = graph_of(result);

    REQUIRE(compiled.passes.size() == 3);
    CHECK(compiled.passes[0].barriers.size() == 1);
    CHECK(compiled.passes[1].barriers.size() == 1);
    CHECK(compiled.passes[2].barriers.empty());
}

TEST_CASE("Transient resources with one layout and disjoint lifetimes share an allocation", "[unit]") {
    RenderGraph graph;
    const ResourceId first = graph.create_texture("first", color_texture());
    const ResourceId second = graph.create_texture("second", color_texture());
    const ResourceId third = graph.create_texture("third", color_texture());
    graph.add_pass("write_first", PassKind::Graphics, {{first, Access::ColorWrite}});
    graph.add_pass("read_first", PassKind::Graphics, {{first, Access::ShaderRead}});
    graph.add_pass("write_second", PassKind::Graphics, {{second, Access::ColorWrite}});
    graph.add_pass("read_both", PassKind::Graphics, {{second, Access::ShaderRead}, {third, Access::ColorWrite}});
    const CompileResult result = graph.compile();
    const CompiledGraph& compiled = graph_of(result);

    CHECK(compiled.lifetimes[first.index].slot == compiled.lifetimes[second.index].slot);
    CHECK(compiled.lifetimes[third.index].slot != compiled.lifetimes[second.index].slot);

    // The reused slot starts from the state its previous user left it in, so the barrier names ShaderRead as its "before" state.
    const CompiledPass& write_second = compiled.passes[2];
    REQUIRE(write_second.name == "write_second");
    REQUIRE(write_second.barriers.size() == 1);
    CHECK(write_second.barriers[0].before == ResourceState::ShaderRead);
    CHECK(write_second.barriers[0].after == ResourceState::RenderTarget);
}

TEST_CASE("Imported resources keep their own allocation and end in their final state through an exit barrier", "[unit]") {
    RenderGraph graph;
    const ResourceId target = graph.import_texture("target", fake_texture(3), color_texture(), ResourceState::Common,
                                                   ResourceState::ShaderRead);
    graph.add_pass("draw", PassKind::Graphics, {{target, Access::ColorWrite}});
    const CompileResult result = graph.compile();
    const CompiledGraph& compiled = graph_of(result);

    REQUIRE(compiled.exit_barriers.size() == 1);
    CHECK(compiled.exit_barriers[0].before == ResourceState::RenderTarget);
    CHECK(compiled.exit_barriers[0].after == ResourceState::ShaderRead);
    CHECK(compiled.slots[compiled.lifetimes[target.index].slot].imported);
}

TEST_CASE("The graph export names passes, accesses, barriers, edges, and lifetimes", "[unit]") {
    RenderGraph graph;
    const ResourceId color = graph.create_texture("color \"main\"", color_texture());
    graph.add_pass("draw", PassKind::Graphics, {{color, Access::ColorWrite}});
    graph.add_pass("sample", PassKind::Graphics, {{color, Access::ShaderRead}});
    const CompileResult result = graph.compile();
    const std::string json = engine::render::export_json(graph_of(result));

    CHECK(json.find("\"schema\": \"engine.render_graph\"") != std::string::npos);
    CHECK(json.find("\"name\": \"draw\"") != std::string::npos);
    CHECK(json.find("\"access\": \"ColorWrite\"") != std::string::npos);
    CHECK(json.find("\"before\": \"RenderTarget\", \"after\": \"ShaderRead\"") != std::string::npos);
    CHECK(json.find("\"from\": \"draw\", \"to\": \"sample\"") != std::string::npos);
    CHECK(json.find("\"first_pass\": 0") != std::string::npos);
    CHECK(json.find("color \\\"main\\\"") != std::string::npos);
    CHECK(json.find("\"timings\"") == std::string::npos);
}

TEST_CASE("The readback chain executes on the reference device and reuses a transient allocation", "[unit]") {
    auto device = validation_device();
    const BufferHandle readback = take(device->create_buffer(engine::render::tests::chain_readback_desc()));
    const RenderGraph graph = engine::render::tests::build_chain_graph(readback);
    const CompileResult result = graph.compile();
    const CompiledGraph& compiled = graph_of(result);

    const auto before = take(device->resource_stats());
    const ExecutionReport report = take(execute(*device, compiled));

    CHECK(report.backend == Backend::Validation);
    REQUIRE(report.passes.size() == 4);
    CHECK(report.passes[0].name == "clear");
    CHECK(report.passes[3].name == "clear_scratch");
    CHECK(report.barriers > 0);
    CHECK(report.cpu_ms >= 0.0);
    CHECK(report.transient_allocations == 2);  // one texture for target and scratch, one staging buffer
    CHECK(report.aliased_resources == 1);

    // Every transient resource is released when execute() returns, so the device holds what it held before.
    const auto after = take(device->resource_stats());
    CHECK(after.textures == before.textures);
    CHECK(after.buffers == before.buffers);
    require_ok(device->check_debug_messages());
    require_ok(device->destroy(readback));
}

TEST_CASE("A failing pass stops the graph, names itself, and leaves no transient resource behind", "[unit]") {
    auto device = validation_device();
    const auto before = take(device->resource_stats());

    RenderGraph graph;
    const ResourceId color = graph.create_texture("color", color_texture());
    graph.add_pass("draw", PassKind::Graphics, {{color, Access::ColorWrite}},
                   [](PassContext&) { return engine::rhi::fail(engine::rhi::RhiErrorCode::InvalidState, "recording refused"); });
    graph.add_pass("after", PassKind::Graphics, {{color, Access::ShaderRead}});
    const CompileResult result = graph.compile();
    const CompiledGraph& compiled = graph_of(result);

    const RhiResult<ExecutionReport> failed = execute(*device, compiled);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message.find("pass 'draw'") != std::string::npos);
    CHECK(failed.error().message.find("recording refused") != std::string::npos);

    const auto after = take(device->resource_stats());
    CHECK(after.textures == before.textures);
    CHECK(after.buffers == before.buffers);
}
