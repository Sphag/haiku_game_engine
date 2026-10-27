// Dynamic ray-traced GI on real GPUs (US-044). Each case builds a floor and a wall as bottom-level and top-level structures, runs DynamicGi on
// DX12 and on Vulkan with the debug layers on, and reads the probe history back. Any validation error fails the check. The cases check the
// update budget (only the planned probes change), a moving sun (the diffuse bounce follows it, and no value is infinite or NaN), a reset
// that discards the old light, and the API's refusals before a scene and a plan.
#include "render_rt_fixture.hpp"

#include "engine/gi/dynamic_grid.hpp"
#include "engine/gi/probes.hpp"
#include "engine/render/dynamic_gi.hpp"
#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using engine::gi::DynamicGridDesc;
using engine::gi::DynamicLighting;
using engine::gi::Sh9;
using engine::render::DynamicGi;
using engine::render::RtScene;
using engine::render::kDynamicGiProbeStride;
using engine::render::make_top_level_instance;
using engine::render::tests::kRtBoxMesh;
using engine::render::tests::RtMeshData;
using engine::render::tests::RtSceneRecords;
using engine::render::tests::rt_box_mesh;
using engine::render::tests::rt_instance;
using engine::render::tests::rt_triangles_of;
using engine::rhi::AccelerationStructureHandle;
using engine::rhi::Backend;
using engine::rhi::BottomLevelDesc;
using engine::rhi::BufferDesc;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::BuildMode;
using engine::rhi::CommandList;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::FenceHandle;
using engine::rhi::Feature;
using engine::rhi::IndexFormat;
using engine::rhi::QueueType;
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TopLevelDesc;
using engine::rhi::TopLevelInstance;

namespace {

// The grid is 8 x 4 x 8 probes at 1 m. Its window is cells [-4, 4) x [0, 4) x [-4, 4) for this eye, so every probe sits above the floor and
// clear of the wall at x = 4.
// 8 x 4 x 8 probes. A budget of all of them traces the whole grid in one frame.
constexpr std::uint32_t kGridProbes = 8u * 4u * 8u;
const glm::vec3 kEye(0.0f, 2.5f, 0.0f);
// The bounce is sampled at a point above the floor, with the normal pointing down: the floor fills that hemisphere, so the sun on the
// floor shows directly in the irradiance.
const glm::vec3 kSamplePoint(0.0f, 1.0f, 0.0f);
const glm::vec3 kSampleNormal(0.0f, -1.0f, 0.0f);

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

DeviceDesc debug_device(Backend backend) {
    DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    return desc;
}

std::vector<std::uint8_t> load_shader(Backend backend, const std::string& name) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/" + name + (backend == Backend::Dx12 ? ".dxil" : ".spv");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("cannot open shader " << path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

ShaderBinary dynamic_shader(Backend backend) {
    return ShaderBinary{ShaderStage::Compute, "CSDynamicGi", load_shader(backend, "dynamic_gi")};
}

void require_ray_tracing(Device& device) {
    if (!device.capabilities().enabled(Feature::RayTracing)) {
        FAIL("ray tracing is not enabled on this adapter: " + std::string(engine::rhi::to_string(device.capabilities().state(Feature::RayTracing))));
    }
}

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

// A device buffer filled from `bytes` through a staging buffer, in ShaderRead as a structure build reads it. The staging buffers are kept
// until the scene is destroyed, so the GPU never reads freed upload memory.
BufferHandle make_device_buffer(Device& device, CommandList& list, const void* bytes, std::uint64_t size, BufferUsage usage, const char* name,
                                std::vector<BufferHandle>& staging) {
    const BufferHandle upload = take(device.create_buffer(buffer_desc(size, BufferUsage::CopySource | BufferUsage::CpuWrite, "gi staging")));
    require_ok(device.upload_buffer(upload, 0, size, bytes));
    staging.push_back(upload);
    const BufferHandle buffer = take(device.create_buffer(buffer_desc(size, usage | BufferUsage::CopyDestination, name)));
    require_ok(list.transition(buffer, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list.copy_buffer(upload, 0, buffer, 0, size));
    require_ok(list.transition(buffer, ResourceState::CopyDestination, ResourceState::ShaderRead));
    return buffer;
}

// The floor and the wall: both boxes, so one bottom-level structure serves both instances.
RtSceneRecords gi_records() {
    RtSceneRecords scene;
    const std::vector<engine::render::RtTriangleRecord> triangles = rt_triangles_of(rt_box_mesh());
    const auto add = [&scene, &triangles](const engine::render::RtInstanceRecord& instance) {
        engine::render::RtInstanceRecord record = instance;
        record.triangle_base = static_cast<std::uint32_t>(scene.triangles.size());
        record.triangle_count = static_cast<std::uint32_t>(triangles.size());
        scene.instances.push_back(record);
        scene.mesh.push_back(kRtBoxMesh);
        scene.triangles.insert(scene.triangles.end(), triangles.begin(), triangles.end());
    };
    add(rt_instance({0.0f, -0.05f, 0.0f}, {20.0f, 0.05f, 20.0f}, {0.8f, 0.8f, 0.8f}, 1.0f));  // floor
    add(rt_instance({4.0f, 1.5f, 0.0f}, {0.25f, 1.5f, 6.0f}, {0.8f, 0.8f, 0.8f}, 1.0f));      // wall at x = 4
    return scene;
}

// The scene on one device: the box structure, the top level, and the records the pass binds.
struct Scene {
    std::unique_ptr<Device> device;
    FenceHandle fence;
    std::uint64_t value = 0;
    AccelerationStructureHandle box_blas;
    AccelerationStructureHandle tlas;
    RtSceneRecords records;
    RtScene bound;
    std::vector<BufferHandle> staging;
};

Scene make_scene(Backend backend) {
    Scene scene;
    scene.device = take(engine::rhi::create_device(debug_device(backend)));
    require_ray_tracing(*scene.device);
    scene.fence = take(scene.device->create_fence(0));
    scene.records = gi_records();

    // The box's bottom-level structure, opaque, built in one list that the fence waits on.
    const RtMeshData box = rt_box_mesh();
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    std::vector<float> positions;
    for (const glm::vec3& p : box.positions) {
        positions.push_back(p.x);
        positions.push_back(p.y);
        positions.push_back(p.z);
    }
    const BufferHandle vertices = make_device_buffer(*scene.device, *list, positions.data(), positions.size() * sizeof(float),
                                                     BufferUsage::Vertex | BufferUsage::ShaderResource, "gi box vertices", scene.staging);
    const BufferHandle indices = make_device_buffer(*scene.device, *list, box.indices.data(), box.indices.size() * sizeof(std::uint32_t),
                                                    BufferUsage::Index | BufferUsage::ShaderResource, "gi box indices", scene.staging);
    BottomLevelDesc desc;
    desc.vertex_buffer = vertices;
    desc.vertex_stride = 3 * sizeof(float);
    desc.vertex_count = static_cast<std::uint32_t>(box.positions.size());
    desc.index_buffer = indices;
    desc.index_format = IndexFormat::Uint32;
    desc.index_count = static_cast<std::uint32_t>(box.indices.size());
    desc.opaque = true;
    desc.debug_name = "gi box";
    scene.box_blas = take(scene.device->create_bottom_level(desc));
    require_ok(list->build_bottom_level(scene.box_blas, BuildMode::Build));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(scene.device->submit(QueueType::Graphics, lists, scene.fence, ++scene.value));
    require_ok(scene.device->wait_fence(scene.fence, scene.value));

    TopLevelDesc top;
    top.max_instances = static_cast<std::uint32_t>(scene.records.instances.size());
    top.allow_refit = false;
    top.debug_name = "gi scene";
    scene.tlas = take(scene.device->create_top_level(top));

    scene.bound.top_level = scene.tlas;
    scene.bound.instances = scene.records.instances;
    scene.bound.triangles = scene.records.triangles;
    return scene;
}

std::vector<TopLevelInstance> top_level_of(const Scene& scene) {
    std::vector<TopLevelInstance> instances;
    for (std::size_t i = 0; i < scene.records.instances.size(); ++i) {
        instances.push_back(make_top_level_instance(scene.records.instances[i], static_cast<std::uint32_t>(i), scene.box_blas));
    }
    return instances;
}

DynamicLighting lighting(const glm::vec3& sun_direction) {
    DynamicLighting light;
    light.sun_direction = glm::normalize(sun_direction);
    light.sun_radiance = glm::vec3(1.0f);
    light.sky_radiance = glm::vec3(0.2f);
    return light;
}

std::unique_ptr<DynamicGi> make_pass(Scene& scene, Backend backend, std::uint32_t budget) {
    DynamicGridDesc desc;
    desc.dims = glm::uvec3(8u, 4u, 8u);
    desc.spacing = 1.0f;
    desc.rays_per_probe = 32;
    desc.update_budget = budget;
    desc.max_history = 4;
    desc.lighting_history = 2;
    auto pass = take(DynamicGi::create(*scene.device, desc, dynamic_shader(backend)));
    require_ok(pass->set_scene(scene.bound));
    static_cast<void>(pass->grid().follow(kEye));
    return pass;
}

// One frame: writes the top level, plans and uploads the pass's frame, builds the top level, traces, and waits for the GPU.
void run_frame(Scene& scene, DynamicGi& pass) {
    require_ok(scene.device->write_top_level(scene.tlas, top_level_of(scene)));
    require_ok(pass.update_frame());
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    require_ok(list->build_top_level(scene.tlas, BuildMode::Build));
    require_ok(pass.record(*list));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(scene.device->submit(QueueType::Graphics, lists, scene.fence, ++scene.value));
    require_ok(scene.device->wait_fence(scene.fence, scene.value));
}

// Reads the history back, one Sh9 per slot in slot order. The history is in ShaderRead between frames, and goes back to it.
std::vector<Sh9> read_history(Scene& scene, DynamicGi& pass) {
    const std::uint32_t slots = pass.slot_count();
    const std::uint64_t bytes = static_cast<std::uint64_t>(slots) * kDynamicGiProbeStride;
    const BufferHandle readback = take(scene.device->create_buffer(buffer_desc(bytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "gi readback")));
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(pass.history(), ResourceState::ShaderRead, ResourceState::CopySource));
    require_ok(list->copy_buffer(pass.history(), 0, readback, 0, bytes));
    require_ok(list->transition(pass.history(), ResourceState::CopySource, ResourceState::ShaderRead));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(scene.device->submit(QueueType::Graphics, lists, scene.fence, ++scene.value));
    require_ok(scene.device->wait_fence(scene.fence, scene.value));

    std::vector<std::uint8_t> raw(bytes);
    require_ok(scene.device->read_buffer(readback, 0, bytes, raw.data()));
    require_ok(scene.device->destroy(readback));

    std::vector<Sh9> history(slots);
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        for (std::size_t i = 0; i < 9; ++i) {
            float value[3];
            std::memcpy(value, raw.data() + static_cast<std::size_t>(slot) * kDynamicGiProbeStride + i * 16, sizeof(value));
            history[slot][i] = glm::vec3(value[0], value[1], value[2]);
        }
    }
    return history;
}

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// The irradiance at a point, read from the history through the grid's volume and the baked sampler.
glm::vec3 sample_at(Scene& scene, DynamicGi& pass, const glm::vec3& point, const glm::vec3& normal) {
    const std::vector<Sh9> history = read_history(scene, pass);
    for (const Sh9& sh : history) {
        for (const glm::vec3& coefficient : sh) {
            REQUIRE(finite(coefficient));
            REQUIRE(glm::all(glm::lessThan(glm::abs(coefficient), glm::vec3(1.0e3f))));
        }
    }
    const engine::gi::GiVolume volume = pass.grid().to_volume(history);
    const engine::gi::IrradianceSample sample = engine::gi::sample_irradiance(volume, point, normal, Sh9{});
    REQUIRE(sample.coverage > 0.99f);
    REQUIRE(finite(sample.irradiance));
    return sample.irradiance;
}

// Only the planned probes are traced, so only their history changes from zero.
void check_budget(Backend backend) {
    Scene scene = make_scene(backend);
    auto pass = make_pass(scene, backend, 64);
    static_cast<void>(pass->grid().set_lighting(lighting(glm::vec3(0.0f, 1.0f, 0.0f))));
    run_frame(scene, *pass);

    const std::vector<engine::gi::ProbeUpdate>& planned = pass->planned();
    REQUIRE(planned.size() == 64);
    std::vector<bool> traced(pass->slot_count(), false);
    for (const engine::gi::ProbeUpdate& update : planned) {
        REQUIRE_FALSE(traced[update.slot]);
        traced[update.slot] = true;
    }

    const std::vector<Sh9> history = read_history(scene, *pass);
    for (std::uint32_t slot = 0; slot < pass->slot_count(); ++slot) {
        bool nonzero = false;
        for (const glm::vec3& coefficient : history[slot]) {
            REQUIRE(finite(coefficient));
            nonzero = nonzero || glm::any(glm::notEqual(coefficient, glm::vec3(0.0f)));
        }
        CHECK(nonzero == traced[slot]);
    }
}

// A moving sun changes the bounce the probes receive from the floor. The change converges to a trace made fresh under the new light, and a
// reset discards the old light in one frame.
void check_moving_light(Backend backend) {
    Scene scene = make_scene(backend);
    const glm::vec3 sun_up(0.0f, 1.0f, 0.0f);
    const glm::vec3 sun_down(0.0f, -1.0f, 0.0f);

    auto pass = make_pass(scene, backend, kGridProbes);
    static_cast<void>(pass->grid().set_lighting(lighting(sun_up)));
    run_frame(scene, *pass);
    const glm::vec3 before = sample_at(scene, *pass, kSamplePoint, kSampleNormal);

    // A stationary light is a fixed point of the blend: more frames average identical traces and change nothing.
    for (int frame = 0; frame < 3; ++frame) {
        run_frame(scene, *pass);
    }
    CHECK(glm::length(sample_at(scene, *pass, kSamplePoint, kSampleNormal) - before) < 1.0e-3f);

    // The sun moves below the horizon: the floor no longer receives direct light, so its bounce drops.
    static_cast<void>(pass->grid().set_lighting(lighting(sun_down)));
    for (int frame = 0; frame < 24; ++frame) {
        run_frame(scene, *pass);
    }
    const glm::vec3 moved = sample_at(scene, *pass, kSamplePoint, kSampleNormal);

    auto fresh = make_pass(scene, backend, kGridProbes);
    static_cast<void>(fresh->grid().set_lighting(lighting(sun_down)));
    run_frame(scene, *fresh);
    const glm::vec3 reference = sample_at(scene, *fresh, kSamplePoint, kSampleNormal);

    CHECK(glm::length(before - reference) > 0.2f);
    CHECK(glm::length(moved - reference) < 0.03f);

    // A teleport resets the grid's samples: one frame then overwrites every probe with the new trace, with no blend of the old one.
    pass->grid().reset();
    run_frame(scene, *pass);
    CHECK(glm::length(sample_at(scene, *pass, kSamplePoint, kSampleNormal) - reference) < 1.0e-3f);
}

// The pass refuses to record before a scene and before a plan.
void check_api_order(Backend backend) {
    Scene scene = make_scene(backend);
    DynamicGridDesc desc;
    desc.dims = glm::uvec3(8u, 4u, 8u);
    desc.update_budget = 32;
    auto pass = take(DynamicGi::create(*scene.device, desc, dynamic_shader(backend)));
    CHECK_FALSE(pass->update_frame().ok());

    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    CHECK_FALSE(pass->record(*list).ok());
    require_ok(pass->set_scene(scene.bound));
    CHECK_FALSE(pass->record(*list).ok());
    require_ok(pass->update_frame());
    require_ok(list->close());
}

}  // namespace

TEST_CASE("DX12 dynamic GI: the budget traces only the planned probes", "[gpu][dx12]") {
    check_budget(Backend::Dx12);
}

TEST_CASE("Vulkan dynamic GI: the budget traces only the planned probes", "[gpu][vulkan]") {
    check_budget(Backend::Vulkan);
}

TEST_CASE("DX12 dynamic GI: a moving sun changes the diffuse bounce, with no NaN, and a reset discards the old light", "[gpu][dx12]") {
    check_moving_light(Backend::Dx12);
}

TEST_CASE("Vulkan dynamic GI: a moving sun changes the diffuse bounce, with no NaN, and a reset discards the old light", "[gpu][vulkan]") {
    check_moving_light(Backend::Vulkan);
}

TEST_CASE("DX12 dynamic GI: the API refuses a record before its scene and its plan", "[gpu][dx12]") {
    check_api_order(Backend::Dx12);
}

TEST_CASE("Vulkan dynamic GI: the API refuses a record before its scene and its plan", "[gpu][vulkan]") {
    check_api_order(Backend::Vulkan);
}
