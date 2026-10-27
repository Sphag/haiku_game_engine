// Froxel volumetric fog on real GPUs (US-045). Each case runs VolumetricFog on DX12 and on Vulkan with the debug layers on, over a box
// occluder in a top-level structure. The cases compare the injected and integrated grids with volumetric_fog's CPU reference, whose
// visibility is the same box tested analytically. They run three frames: a first frame, a moving sun that the previous grid reprojects
// into, and a camera cut that must discard the history. The API's refusals are checked too. Any validation error fails the check.
#include "engine/render/volumetric_fog.hpp"
#include "engine/render/rt_effects.hpp"
#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"
#include "render_rt_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using engine::render::FogFrame;
using engine::render::FogLight;
using engine::render::FogVolume;
using engine::render::FogVisibility;
using engine::render::VolumetricFog;
using engine::render::integrate_reference;
using engine::render::inject_reference;
using engine::render::kFogFroxelBytes;
using engine::render::make_top_level_instance;
using engine::render::RtInstanceRecord;
using engine::render::tests::rt_box_mesh;
using engine::render::tests::rt_instance;
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

constexpr std::uint32_t kWidth = 8;
constexpr std::uint32_t kHeight = 6;
constexpr std::uint32_t kDepth = 8;
// The occluder: a box in front of the camera, above and to one side of the view's centre, so the sun's shadow falls across the view.
const glm::vec3 kBoxCenter(0.0f, 0.5f, -10.0f);
const glm::vec3 kBoxHalf(3.0f, 3.0f, 1.0f);

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

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

void require_ray_tracing(Device& device) {
    if (!device.capabilities().enabled(Feature::RayTracing)) {
        FAIL("ray tracing is not enabled on this adapter: " + std::string(engine::rhi::to_string(device.capabilities().state(Feature::RayTracing))));
    }
}

// The device, the box's structures, and the staging buffers, which stay alive until the case ends.
struct Scene {
    std::unique_ptr<Device> device;
    FenceHandle fence;
    std::uint64_t value = 0;
    AccelerationStructureHandle box_blas;
    AccelerationStructureHandle tlas;
    RtInstanceRecord box;
    std::vector<BufferHandle> staging;
};

BufferHandle make_device_buffer(Scene& scene, CommandList& list, const void* bytes, std::uint64_t size, BufferUsage usage, const char* name) {
    const BufferHandle upload = take(scene.device->create_buffer(buffer_desc(size, BufferUsage::CopySource | BufferUsage::CpuWrite, "fog staging")));
    require_ok(scene.device->upload_buffer(upload, 0, size, bytes));
    scene.staging.push_back(upload);
    const BufferHandle buffer = take(scene.device->create_buffer(buffer_desc(size, usage | BufferUsage::CopyDestination, name)));
    require_ok(list.transition(buffer, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list.copy_buffer(upload, 0, buffer, 0, size));
    require_ok(list.transition(buffer, ResourceState::CopyDestination, ResourceState::ShaderRead));
    return buffer;
}

Scene make_scene(Backend backend) {
    Scene scene;
    scene.device = take(engine::rhi::create_device(debug_device(backend)));
    require_ray_tracing(*scene.device);
    scene.fence = take(scene.device->create_fence(0));
    scene.box = rt_instance(kBoxCenter, kBoxHalf, glm::vec3(0.8f), 1.0f);

    const engine::render::tests::RtMeshData box = rt_box_mesh();
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    std::vector<float> positions;
    for (const glm::vec3& p : box.positions) {
        positions.push_back(p.x);
        positions.push_back(p.y);
        positions.push_back(p.z);
    }
    const BufferHandle vertices = make_device_buffer(scene, *list, positions.data(), positions.size() * sizeof(float),
                                                     BufferUsage::Vertex | BufferUsage::ShaderResource, "fog box vertices");
    const BufferHandle indices = make_device_buffer(scene, *list, box.indices.data(), box.indices.size() * sizeof(std::uint32_t),
                                                    BufferUsage::Index | BufferUsage::ShaderResource, "fog box indices");
    BottomLevelDesc desc;
    desc.vertex_buffer = vertices;
    desc.vertex_stride = 3 * sizeof(float);
    desc.vertex_count = static_cast<std::uint32_t>(box.positions.size());
    desc.index_buffer = indices;
    desc.index_format = IndexFormat::Uint32;
    desc.index_count = static_cast<std::uint32_t>(box.indices.size());
    desc.opaque = true;
    desc.debug_name = "fog box";
    scene.box_blas = take(scene.device->create_bottom_level(desc));
    require_ok(list->build_bottom_level(scene.box_blas, BuildMode::Build));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(scene.device->submit(QueueType::Graphics, lists, scene.fence, ++scene.value));
    require_ok(scene.device->wait_fence(scene.fence, scene.value));

    TopLevelDesc top;
    top.max_instances = 1;
    top.allow_refit = false;
    top.debug_name = "fog scene";
    scene.tlas = take(scene.device->create_top_level(top));
    return scene;
}

// The camera at the origin looking down -Z, with a 90 degree vertical field of view on a square image.
FogFrame camera_frame() {
    FogFrame frame;
    frame.forward = glm::vec3(0.0f, 0.0f, -1.0f);
    frame.right = glm::vec3(1.0f, 0.0f, 0.0f);
    frame.up = glm::vec3(0.0f, 1.0f, 0.0f);
    frame.previous_forward = frame.forward;
    frame.previous_right = frame.right;
    frame.previous_up = frame.up;
    frame.near_plane = 1.0f;
    frame.far_plane = 40.0f;
    frame.density = 0.05f;
    frame.albedo = glm::vec3(1.0f);
    frame.anisotropy = 0.3f;
    frame.sun_direction = glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f));
    frame.sun_radiance = glm::vec3(1.0f);
    frame.shadows = true;
    frame.history_weight = 0.9f;
    frame.max_age = 15;
    FogLight light;
    light.position = glm::vec3(2.0f, 1.0f, -8.0f);
    light.radiance = glm::vec3(4.0f);
    light.range = 20.0f;
    light.shadowed = true;
    frame.lights.push_back(light);
    return frame;
}

// The box as a visibility: 0 where the segment from `from` to `to` enters it, 1 elsewhere (the slab test over the segment).
float box_visibility(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 low = kBoxCenter - kBoxHalf;
    const glm::vec3 high = kBoxCenter + kBoxHalf;
    const glm::vec3 direction = to - from;
    float enter = 0.0f;
    float exit = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(direction[axis]) < 1.0e-12f) {
            if (from[axis] < low[axis] || from[axis] > high[axis]) {
                return 1.0f;
            }
            continue;
        }
        const float t0 = (low[axis] - from[axis]) / direction[axis];
        const float t1 = (high[axis] - from[axis]) / direction[axis];
        enter = std::max(enter, std::min(t0, t1));
        exit = std::min(exit, std::max(t0, t1));
        if (enter > exit) {
            return 1.0f;
        }
    }
    return 0.0f;
}

std::vector<TopLevelInstance> top_level_of(const Scene& scene) {
    return {make_top_level_instance(scene.box, 0, scene.box_blas)};
}

// One frame: writes the top level, uploads the frame's parameters, then builds the top level and records inject and integrate in one
// list. The grid is read back after the GPU is done.
void run_frame(Scene& scene, VolumetricFog& fog, const FogFrame& frame) {
    require_ok(scene.device->write_top_level(scene.tlas, top_level_of(scene)));
    require_ok(fog.update_frame(frame));
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    require_ok(list->build_top_level(scene.tlas, BuildMode::Build));
    require_ok(fog.inject(*list));
    require_ok(fog.integrate(*list));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(scene.device->submit(QueueType::Graphics, lists, scene.fence, ++scene.value));
    require_ok(scene.device->wait_fence(scene.fence, scene.value));
}

// The integrated grid, read back in froxel order (16 bytes each).
FogVolume read_integrated(Scene& scene, VolumetricFog& fog) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(kWidth) * kHeight * kDepth * kFogFroxelBytes;
    const BufferHandle readback = take(scene.device->create_buffer(buffer_desc(bytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "fog readback")));
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(fog.integrated(), ResourceState::ShaderRead, ResourceState::CopySource));
    require_ok(list->copy_buffer(fog.integrated(), 0, readback, 0, bytes));
    require_ok(list->transition(fog.integrated(), ResourceState::CopySource, ResourceState::ShaderRead));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(scene.device->submit(QueueType::Graphics, lists, scene.fence, ++scene.value));
    require_ok(scene.device->wait_fence(scene.fence, scene.value));

    std::vector<std::uint8_t> raw(bytes);
    require_ok(scene.device->read_buffer(readback, 0, bytes, raw.data()));
    require_ok(scene.device->destroy(readback));

    FogVolume volume;
    volume.width = kWidth;
    volume.height = kHeight;
    volume.depth = kDepth;
    volume.cells.resize(static_cast<std::size_t>(kWidth) * kHeight * kDepth);
    std::memcpy(volume.cells.data(), raw.data(), raw.size());
    return volume;
}

// The GPU grid against the CPU reference: a froxel matches when each channel is within 2e-3 plus 1% of the larger value. A mismatch must
// be rare: the two differ only where the visibility ray and the analytic box disagree on a grazing segment.
void compare_grids(const FogVolume& gpu, const FogVolume& expected, const char* label) {
    std::size_t mismatches = 0;
    std::size_t non_finite = 0;
    for (std::size_t i = 0; i < gpu.cells.size(); ++i) {
        const glm::vec4 a = gpu.cells[i];
        const glm::vec4 b = expected.cells[i];
        if (!std::isfinite(a.r) || !std::isfinite(a.g) || !std::isfinite(a.b) || !std::isfinite(a.a)) {
            ++non_finite;
        }
        for (int channel = 0; channel < 4; ++channel) {
            const float tolerance = 2.0e-3f + 1.0e-2f * std::max(std::fabs(a[channel]), std::fabs(b[channel]));
            if (std::fabs(a[channel] - b[channel]) > tolerance) {
                ++mismatches;
                break;
            }
        }
    }
    INFO(label);
    CHECK(non_finite == 0);
    CHECK(mismatches <= gpu.cells.size() / 50);
}

void check_frames(Backend backend) {
    Scene scene = make_scene(backend);
    auto fog = take(VolumetricFog::create(*scene.device, kWidth, kHeight, kDepth, ShaderBinary{ShaderStage::Compute, "CSFogInject", load_shader(backend, "volumetric_fog_inject")},
                                          ShaderBinary{ShaderStage::Compute, "CSFogIntegrate", load_shader(backend, "volumetric_fog_integrate")},
                                          ShaderBinary{ShaderStage::Compute, "CSFogApply", load_shader(backend, "volumetric_fog_apply")}));
    require_ok(fog->set_scene(scene.tlas));

    // The box's shadow must change the grid, or the comparison below would not test the shadow rays.
    const FogFrame first = camera_frame();
    const FogVolume shadowed = integrate_reference(first, inject_reference(first, FogVolume{}, kWidth, kHeight, kDepth, box_visibility));
    const FogVolume open = integrate_reference(
        first, inject_reference(first, FogVolume{}, kWidth, kHeight, kDepth, [](const glm::vec3&, const glm::vec3&) { return 1.0f; }));
    float largest = 0.0f;
    for (std::size_t i = 0; i < shadowed.cells.size(); ++i) {
        largest = std::max(largest, std::fabs(shadowed.cells[i].r - open.cells[i].r));
    }
    CHECK(largest > 1.0e-3f);

    // Frame 1: no history. The grid is lit by the sun and the light, with the box's shadow.
    run_frame(scene, *fog, first);
    FogVolume injected = inject_reference(first, FogVolume{}, kWidth, kHeight, kDepth, box_visibility);
    compare_grids(read_integrated(scene, *fog), integrate_reference(first, injected), "frame 1");

    // Frame 2: the sun moves, the camera stays. The previous injected grid is reprojected and blended.
    FogFrame second = first;
    second.sun_direction = glm::normalize(glm::vec3(0.1f, 1.0f, -1.0f));
    second.previous_eye = first.eye;
    run_frame(scene, *fog, second);
    injected = inject_reference(second, injected, kWidth, kHeight, kDepth, box_visibility);
    compare_grids(read_integrated(scene, *fog), integrate_reference(second, injected), "frame 2 (moving sun)");

    // Frame 3: a camera cut. The history is discarded, so the grid matches a fresh inject.
    FogFrame cut = second;
    cut.reset = true;
    run_frame(scene, *fog, cut);
    const FogVolume fresh = inject_reference(cut, FogVolume{}, kWidth, kHeight, kDepth, box_visibility);
    compare_grids(read_integrated(scene, *fog), integrate_reference(cut, fresh), "frame 3 (camera cut)");

    require_ok(scene.device->check_debug_messages());
    require_ok(fog->release());
}

void check_api_order(Backend backend) {
    Scene scene = make_scene(backend);
    auto fog = take(VolumetricFog::create(*scene.device, kWidth, kHeight, kDepth, ShaderBinary{ShaderStage::Compute, "CSFogInject", load_shader(backend, "volumetric_fog_inject")},
                                          ShaderBinary{ShaderStage::Compute, "CSFogIntegrate", load_shader(backend, "volumetric_fog_integrate")},
                                          ShaderBinary{ShaderStage::Compute, "CSFogApply", load_shader(backend, "volumetric_fog_apply")}));
    auto list = take(scene.device->create_command_list(QueueType::Graphics));
    CHECK_FALSE(fog->inject(*list).ok());
    CHECK_FALSE(fog->integrate(*list).ok());
    CHECK_FALSE(fog->set_scene(AccelerationStructureHandle{}).ok());
    require_ok(fog->set_scene(scene.tlas));
    CHECK_FALSE(fog->inject(*list).ok());
    FogFrame bad = camera_frame();
    bad.history_weight = 1.0f;
    CHECK_FALSE(fog->update_frame(bad).ok());
    require_ok(fog->update_frame(camera_frame()));
    CHECK_FALSE(fog->integrate(*list).ok());
    require_ok(list->close());
    require_ok(fog->release());
}

}  // namespace

TEST_CASE("DX12 volumetric fog matches its CPU reference across a first frame, a moving sun, and a camera cut", "[gpu][dx12]") {
    check_frames(Backend::Dx12);
}

TEST_CASE("Vulkan volumetric fog matches its CPU reference across a first frame, a moving sun, and a camera cut", "[gpu][vulkan]") {
    check_frames(Backend::Vulkan);
}

TEST_CASE("DX12 volumetric fog refuses a record before its scene, its frame, and its inject", "[gpu][dx12]") {
    check_api_order(Backend::Dx12);
}

TEST_CASE("Vulkan volumetric fog refuses a record before its scene, its frame, and its inject", "[gpu][vulkan]") {
    check_api_order(Backend::Vulkan);
}
