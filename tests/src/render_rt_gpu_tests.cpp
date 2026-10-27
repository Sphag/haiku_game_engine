// Ray-traced shadows and reflections on real GPUs (US-043). Each case builds the reference scene (render_rt_fixture.hpp) as bottom-level
// structures, a top-level structure, and an alpha atlas, then runs RtEffects on DX12 and on Vulkan with the debug layers on. Any
// validation error fails the check. The output of each frame is compared with the CPU reference (trace_reference) pixel by pixel, and the
// cases check what the reference scene is for: a reflection of an off-screen pillar, a shadow that moves with the occluder, the
// alpha-masked holes in the fence, the roughness cutoff, the reflection distance limit, and the difference between RT on and off.
#include "render_rt_fixture.hpp"

#include "engine/render/rt_effects.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using engine::render::RtEffects;
using engine::render::RtFrame;
using engine::render::RtMaskImage;
using engine::render::RtReference;
using engine::render::RtScene;
using engine::render::make_top_level_instance;
using engine::render::trace_reference;
using engine::render::tests::kRtPillar;
using engine::render::tests::kRtQuadMesh;
using engine::render::tests::kRtRoughBox;
using engine::render::tests::kRtSize;
using engine::render::tests::rt_box_mesh;
using engine::render::tests::rt_quad_mesh;
using engine::render::tests::rt_scene;
using engine::render::tests::rt_view;
using engine::render::tests::RtMeshData;
using engine::render::tests::RtSceneRecords;
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
using engine::rhi::Format;
using engine::rhi::IndexFormat;
using engine::rhi::QueueType;
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;
using engine::rhi::TopLevelDesc;
using engine::rhi::TopLevelInstance;

namespace {

// The reference's shadow and reflection values can differ at a triangle edge, where float rounding picks another triangle. A few pixels
// of that kind are allowed; anything more is a real disagreement.
constexpr std::size_t kMaxEdgeMismatches = 16;
constexpr float kColorTolerance = 0.02f;

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

ShaderBinary trace_shader(Backend backend) {
    return ShaderBinary{ShaderStage::Compute, "CSRayTracedEffects", load_shader(backend, "rt_effects")};
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

// A device buffer filled from `bytes` through a staging buffer, in ShaderRead as a structure build reads it. The staging buffer goes into
// `staging` and is destroyed once the upload's fence has been waited on.
BufferHandle make_device_buffer(Device& device, CommandList& list, const void* bytes, std::uint64_t size, BufferUsage usage, const char* name,
                                std::vector<BufferHandle>& staging) {
    const BufferHandle upload = take(device.create_buffer(buffer_desc(size, BufferUsage::CopySource | BufferUsage::CpuWrite, "rt staging")));
    require_ok(device.upload_buffer(upload, 0, size, bytes));
    staging.push_back(upload);
    const BufferHandle buffer = take(device.create_buffer(buffer_desc(size, usage | BufferUsage::CopyDestination, name)));
    require_ok(list.transition(buffer, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list.copy_buffer(upload, 0, buffer, 0, size));
    require_ok(list.transition(buffer, ResourceState::CopyDestination, ResourceState::ShaderRead));
    return buffer;
}

// The mask atlas: RGBA8 with the checkerboard's alpha in .a. The rows are at the copy pitch, as copy_buffer_to_texture reads them.
TextureHandle make_mask_atlas(Device& device, CommandList& list, const RtMaskImage& mask, std::vector<BufferHandle>& staging) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(mask.width, Format::R8G8B8A8Unorm);
    std::vector<std::uint8_t> bytes(pitch * mask.height, 0);
    for (std::uint32_t y = 0; y < mask.height; ++y) {
        for (std::uint32_t x = 0; x < mask.width; ++x) {
            bytes[y * pitch + x * 4 + 3] = static_cast<std::uint8_t>(mask.alpha[static_cast<std::size_t>(y) * mask.width + x] >= 0.5f ? 255 : 0);
        }
    }
    const BufferHandle upload = take(device.create_buffer(buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "atlas staging")));
    require_ok(device.upload_buffer(upload, 0, bytes.size(), bytes.data()));
    staging.push_back(upload);

    TextureDesc desc;
    desc.width = mask.width;
    desc.height = mask.height;
    desc.format = Format::R8G8B8A8Unorm;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDestination;
    desc.debug_name = "rt mask atlas";
    const TextureHandle atlas = take(device.create_texture(desc));
    require_ok(list.transition(atlas, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list.copy_buffer_to_texture(upload, 0, atlas));
    require_ok(list.transition(atlas, ResourceState::CopyDestination, ResourceState::ShaderRead));
    return atlas;
}

// One frame's output, read back from the two targets.
struct Output {
    std::vector<float> shadow;
    std::vector<glm::vec4> reflection;
};

// The scene on one device: the bottom-level structures, the top-level structure, the atlas, and the pass.
struct Harness {
    std::unique_ptr<Device> device;
    FenceHandle fence;
    std::uint64_t value = 0;
    AccelerationStructureHandle box_blas;
    AccelerationStructureHandle quad_blas;
    AccelerationStructureHandle tlas;
    TextureHandle atlas;
    BufferHandle readback;
    std::uint64_t readback_pitch = 0;
    std::unique_ptr<RtEffects> effects;
    std::vector<BufferHandle> staging;
};

// Builds the bottom-level structures from the mesh data, in one list that the fence waits on.
void build_bottom_levels(Harness& harness, const RtMeshData& box, const RtMeshData& quad) {
    auto list = take(harness.device->create_command_list(QueueType::Graphics));
    const auto upload_mesh = [&](const RtMeshData& mesh, const char* name, bool opaque) {
        std::vector<float> positions;
        for (const glm::vec3& p : mesh.positions) {
            positions.push_back(p.x);
            positions.push_back(p.y);
            positions.push_back(p.z);
        }
        const BufferHandle vertices = make_device_buffer(*harness.device, *list, positions.data(), positions.size() * sizeof(float),
                                                         BufferUsage::Vertex | BufferUsage::ShaderResource, name, harness.staging);
        const BufferHandle indices = make_device_buffer(*harness.device, *list, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t),
                                                        BufferUsage::Index | BufferUsage::ShaderResource, name, harness.staging);
        BottomLevelDesc desc;
        desc.vertex_buffer = vertices;
        desc.vertex_stride = 3 * sizeof(float);
        desc.vertex_count = static_cast<std::uint32_t>(mesh.positions.size());
        desc.index_buffer = indices;
        desc.index_format = IndexFormat::Uint32;
        desc.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        desc.opaque = opaque;
        desc.debug_name = name;
        return take(harness.device->create_bottom_level(desc));
    };
    // The box is opaque; the fence quad is a non-opaque structure, so its candidates reach the alpha test.
    harness.box_blas = upload_mesh(box, "rt box", true);
    harness.quad_blas = upload_mesh(quad, "rt quad", false);
    require_ok(list->build_bottom_level(harness.box_blas, BuildMode::Build));
    require_ok(list->build_bottom_level(harness.quad_blas, BuildMode::Build));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(harness.device->submit(QueueType::Graphics, lists, harness.fence, ++harness.value));
    require_ok(harness.device->wait_fence(harness.fence, harness.value));
}

// The top-level structure for one set of records: the instances' transforms and the bottom-level structure of each mesh.
std::vector<TopLevelInstance> top_level_of(const RtSceneRecords& scene, const Harness& harness) {
    std::vector<TopLevelInstance> instances;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const AccelerationStructureHandle blas = scene.mesh[i] == kRtQuadMesh ? harness.quad_blas : harness.box_blas;
        instances.push_back(make_top_level_instance(scene.instances[i], static_cast<std::uint32_t>(i), blas));
    }
    return instances;
}

Harness make_harness(Backend backend, const RtSceneRecords& scene) {
    Harness harness;
    harness.device = take(engine::rhi::create_device(debug_device(backend)));
    require_ray_tracing(*harness.device);
    harness.fence = take(harness.device->create_fence(0));
    build_bottom_levels(harness, rt_box_mesh(), rt_quad_mesh());

    TopLevelDesc top;
    top.max_instances = static_cast<std::uint32_t>(scene.instances.size());
    top.allow_refit = false;
    top.debug_name = "rt scene";
    harness.tlas = take(harness.device->create_top_level(top));

    auto list = take(harness.device->create_command_list(QueueType::Graphics));
    harness.atlas = make_mask_atlas(*harness.device, *list, scene.mask, harness.staging);
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(harness.device->submit(QueueType::Graphics, lists, harness.fence, ++harness.value));
    require_ok(harness.device->wait_fence(harness.fence, harness.value));

    harness.effects = take(RtEffects::create(*harness.device, kRtSize, kRtSize, trace_shader(backend)));
    harness.readback_pitch = engine::rhi::copy_row_pitch(kRtSize, Format::R16G16B16A16Float);
    harness.readback = take(harness.device->create_buffer(
        buffer_desc(harness.readback_pitch * kRtSize * 2, BufferUsage::CopyDestination | BufferUsage::CpuRead, "rt readback")));
    return harness;
}

// Uploads the scene's tables for one mask setting, and the TLAS for one occluder position.
void bind_scene(Harness& harness, const RtSceneRecords& scene) {
    RtScene bound;
    bound.top_level = harness.tlas;
    bound.instances = scene.instances;
    bound.triangles = scene.triangles;
    bound.mask_atlas = harness.atlas;
    require_ok(harness.effects->set_scene(bound));
}

// Runs one frame: writes the TLAS for the scene, builds it, traces, and reads the two targets back.
Output render(Harness& harness, const RtSceneRecords& scene, const RtFrame& frame) {
    // The previous frame has finished (every render waits), so the instance memory is free to rewrite.
    require_ok(harness.device->write_top_level(harness.tlas, top_level_of(scene, harness)));
    require_ok(harness.effects->update_frame(frame));

    auto list = take(harness.device->create_command_list(QueueType::Graphics));
    require_ok(list->build_top_level(harness.tlas, BuildMode::Build));
    require_ok(harness.effects->record(*list));
    require_ok(list->transition(harness.effects->shadow(), ResourceState::ShaderRead, ResourceState::CopySource));
    require_ok(list->copy_texture_to_buffer(harness.effects->shadow(), harness.readback, 0));
    require_ok(list->transition(harness.effects->shadow(), ResourceState::CopySource, ResourceState::ShaderRead));
    require_ok(list->transition(harness.effects->reflection(), ResourceState::ShaderRead, ResourceState::CopySource));
    require_ok(list->copy_texture_to_buffer(harness.effects->reflection(), harness.readback, harness.readback_pitch * kRtSize));
    require_ok(list->transition(harness.effects->reflection(), ResourceState::CopySource, ResourceState::ShaderRead));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(harness.device->submit(QueueType::Graphics, lists, harness.fence, ++harness.value));
    require_ok(harness.device->wait_fence(harness.fence, harness.value));

    std::vector<std::uint8_t> bytes(harness.readback_pitch * kRtSize * 2);
    require_ok(harness.device->read_buffer(harness.readback, 0, bytes.size(), bytes.data()));
    Output output;
    output.shadow.resize(kRtSize * kRtSize);
    output.reflection.resize(kRtSize * kRtSize);
    for (std::uint32_t y = 0; y < kRtSize; ++y) {
        for (std::uint32_t x = 0; x < kRtSize; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * kRtSize + x;
            std::uint64_t shadow_texel = 0;
            std::memcpy(&shadow_texel, bytes.data() + y * harness.readback_pitch + x * 8, sizeof(shadow_texel));
            output.shadow[pixel] = glm::unpackHalf4x16(shadow_texel).x;
            std::uint64_t reflection_texel = 0;
            std::memcpy(&reflection_texel, bytes.data() + harness.readback_pitch * kRtSize + y * harness.readback_pitch + x * 8, sizeof(reflection_texel));
            output.reflection[pixel] = glm::unpackHalf4x16(reflection_texel);
        }
    }
    return output;
}

bool finite(const glm::vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

// Compares a frame with the reference: shadow lit or not, reflection hit or not, and the reflected color where both hit.
void compare_with_reference(const Output& gpu, const RtReference& reference, const char* label) {
    std::size_t shadow_mismatch = 0;
    std::size_t hit_mismatch = 0;
    std::size_t color_mismatch = 0;
    std::size_t invalid = 0;
    std::ostringstream samples;
    for (std::size_t i = 0; i < gpu.shadow.size(); ++i) {
        if (!std::isfinite(gpu.shadow[i]) || !finite(gpu.reflection[i]) || glm::any(glm::lessThan(gpu.reflection[i], glm::vec4(-1.0e-3f)))) {
            ++invalid;
        }
        if ((gpu.shadow[i] >= 0.5f) != (reference.shadow[i] >= 0.5f)) {
            ++shadow_mismatch;
        }
        const bool gpu_hit = gpu.reflection[i].a >= 0.5f;
        const bool reference_hit = reference.reflection[i].a >= 0.5f;
        if (gpu_hit != reference_hit) {
            ++hit_mismatch;
        } else if (reference_hit && glm::any(glm::greaterThan(glm::abs(glm::vec3(gpu.reflection[i]) - glm::vec3(reference.reflection[i])), glm::vec3(kColorTolerance)))) {
            if (color_mismatch < 4) {
                const glm::vec4 g = gpu.reflection[i];
                const glm::vec4 r = reference.reflection[i];
                samples << " (" << i % kRtSize << "," << i / kRtSize << ") gpu " << g.r << "," << g.g << "," << g.b << " ref " << r.r << "," << r.g << "," << r.b
                        << " primary " << reference.primary_instance[i];
            }
            ++color_mismatch;
        }
    }
    INFO(label << ": shadow " << shadow_mismatch << ", reflection hit " << hit_mismatch << ", reflection color " << color_mismatch << ", invalid " << invalid
               << ", first color mismatches:" << samples.str());
    CHECK(invalid == 0);
    CHECK(shadow_mismatch <= kMaxEdgeMismatches);
    CHECK(hit_mismatch <= kMaxEdgeMismatches);
    CHECK(color_mismatch <= kMaxEdgeMismatches);
}

template <typename Predicate>
std::size_t count_where(const Output& output, Predicate&& predicate) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < output.shadow.size(); ++i) {
        if (predicate(i)) {
            ++count;
        }
    }
    return count;
}

bool red_reflection(const Output& output, std::size_t i) {
    const glm::vec4& value = output.reflection[i];
    return value.a > 0.5f && value.r > 2.0f * value.g && value.r > 2.0f * value.b;
}

void run_scene_case(Backend backend) {
    const RtSceneRecords lit_scene = rt_scene(-1.5f);
    const RtSceneRecords moved_scene = rt_scene(1.5f);
    Harness harness = make_harness(backend, lit_scene);
    bind_scene(harness, lit_scene);

    // Frame A: the occluder on the left.
    const RtFrame frame = rt_view();
    const Output a = render(harness, lit_scene, frame);
    const RtReference reference_a = trace_reference(lit_scene.instances, lit_scene.triangles, lit_scene.mask, frame, kRtSize, kRtSize);
    compare_with_reference(a, reference_a, "frame A");
    require_ok(harness.device->check_debug_messages());

    // The reflected pillar is off screen, so no primary ray of the trace reaches it, and the reflection still shows it.
    CHECK(count_where(a, [&](std::size_t i) { return reference_a.primary_instance[i] == static_cast<std::int32_t>(kRtPillar); }) == 0);
    const std::size_t off_screen = count_where(a, [&](std::size_t i) { return red_reflection(a, i); });
    INFO("GPU pixels reflecting the off-screen pillar: " << off_screen);
    CHECK(off_screen >= 10);

    // The rough box is above the cutoff: none of its pixels is reflected.
    CHECK(count_where(a, [&](std::size_t i) { return reference_a.primary_instance[i] == static_cast<std::int32_t>(kRtRoughBox) && a.reflection[i].a > 0.5f; }) == 0);

    // Frame B: the occluder moved right, the TLAS rewritten and rebuilt. The shadow moves, and the GPU follows the reference.
    const Output b = render(harness, moved_scene, frame);
    const RtReference reference_b = trace_reference(moved_scene.instances, moved_scene.triangles, moved_scene.mask, frame, kRtSize, kRtSize);
    compare_with_reference(b, reference_b, "frame B");
    const std::size_t moved = count_where(a, [&](std::size_t i) { return (a.shadow[i] < 0.5f) != (b.shadow[i] < 0.5f); });
    INFO("GPU pixels whose shadow moved with the occluder: " << moved);
    CHECK(moved > 0);
    require_ok(harness.device->check_debug_messages());

    // RT off: the raster fallback lights every pixel and reflects nothing. RT on differs where the reference shadows or reflects.
    RtFrame off = frame;
    off.shadows = false;
    off.reflections = false;
    const Output none = render(harness, lit_scene, off);
    CHECK(count_where(none, [&](std::size_t i) { return none.shadow[i] != 1.0f; }) == 0);
    CHECK(count_where(none, [&](std::size_t i) { return none.reflection[i].a != 0.0f; }) == 0);
    CHECK(count_where(a, [&](std::size_t i) { return a.shadow[i] < 0.5f; }) > 0);
    CHECK(count_where(a, [&](std::size_t i) { return red_reflection(a, i); }) > 0);

    // A reflection distance of half a meter is too short for the pillar, so its reflection is lost.
    RtFrame short_reach = frame;
    short_reach.reflection_distance = 0.5f;
    const Output limited = render(harness, lit_scene, short_reach);
    CHECK(count_where(limited, [&](std::size_t i) { return red_reflection(limited, i); }) == 0);
    require_ok(harness.device->check_debug_messages());

    // The fence without its mask casts the same shadow as the masked fence where the mask is opaque, and differs from it where the holes are.
    const RtSceneRecords open_scene = rt_scene(-1.5f, false);
    bind_scene(harness, open_scene);
    const Output open = render(harness, open_scene, frame);
    const RtReference reference_open = trace_reference(open_scene.instances, open_scene.triangles, open_scene.mask, frame, kRtSize, kRtSize);
    compare_with_reference(open, reference_open, "fence without mask");
    const std::size_t holes = count_where(a, [&](std::size_t i) { return (a.shadow[i] < 0.5f) != (open.shadow[i] < 0.5f); });
    INFO("GPU shadow pixels that differ between the masked and unmasked fence: " << holes);
    CHECK(holes > 0);
    require_ok(harness.device->check_debug_messages());

    require_ok(harness.effects->release());
    require_ok(harness.device->check_debug_messages());
}

void run_api_case(Backend backend) {
    const RtSceneRecords scene = rt_scene(-1.5f);
    Harness harness = make_harness(backend, scene);

    // The pass refuses to record before it has a scene and a frame.
    auto list = take(harness.device->create_command_list(QueueType::Graphics));
    CHECK_FALSE(harness.effects->record(*list).ok());
    RtScene unbound;
    unbound.instances = scene.instances;
    unbound.triangles = scene.triangles;
    CHECK_FALSE(harness.effects->set_scene(unbound).ok());
    bind_scene(harness, scene);
    CHECK_FALSE(harness.effects->record(*list).ok());

    // A frame with a non-unit forward vector is rejected before any upload.
    RtFrame bad = rt_view();
    bad.forward = glm::vec3(0.0f, 0.0f, 2.0f);
    CHECK_FALSE(harness.effects->update_frame(bad).ok());
    require_ok(harness.device->check_debug_messages());
    require_ok(harness.effects->release());
    require_ok(harness.device->check_debug_messages());
}

void run_feature_case(Backend backend) {
    DeviceDesc desc = debug_device(backend);
    desc.disabled_features = Feature::RayTracing;
    auto device = take(engine::rhi::create_device(desc));
    CHECK(device->capabilities().state(Feature::RayTracing) == engine::rhi::FeatureState::DisabledForTest);
    const auto effects = RtEffects::create(*device, kRtSize, kRtSize, trace_shader(backend));
    CHECK_FALSE(effects.ok());
    require_ok(device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 ray-traced effects: the off-screen reflection, the moving shadow, the mask, and RT on against RT off", "[gpu][dx12]") {
    run_scene_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced effects: the off-screen reflection, the moving shadow, the mask, and RT on against RT off", "[gpu][vulkan]") {
    run_scene_case(Backend::Vulkan);
}

TEST_CASE("DX12 ray-traced effects: the API rejects a record before its scene and frame, and a bad frame", "[gpu][dx12]") {
    run_api_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced effects: the API rejects a record before its scene and frame, and a bad frame", "[gpu][vulkan]") {
    run_api_case(Backend::Vulkan);
}

TEST_CASE("DX12 ray-traced effects are refused without the RayTracing feature", "[gpu][dx12]") {
    run_feature_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced effects are refused without the RayTracing feature", "[gpu][vulkan]") {
    run_feature_case(Backend::Vulkan);
}
