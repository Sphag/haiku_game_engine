// Native-profile GI and reflections on real GPUs (US-040). The scene renderer lights a room and an open floor from baked probes, and a mirror
// reflects a wall through the screen-space reflection pass. Each case runs DX12 and Vulkan with the debug layers on, and any validation error
// fails the check. The cases cover: the probe grid against the environment alone (the room is darker, the open floor is not), the coverage
// debug view (green inside the grid, red outside), the reflection of an on-screen wall (on, and off as the reference), and the rejection of
// volumes that do not match the format or the grid, and of screen reflections without the post pass.
#include "asset_converter/importer.hpp"
#include "engine/assets/asset_registry.hpp"
#include "engine/assets/content_hash.hpp"
#include "engine/assets/cooked_container.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/gi/cooked_volume.hpp"
#include "engine/gi/probes.hpp"
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/post_processor.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"
#include "engine/scene/scene_graph.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using engine::gi::GiVolume;
using engine::render::Camera;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::LightingView;
using engine::render::MaterialParams;
using engine::render::PostProcessSettings;
using engine::render::RenderableRef;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::PostShaders;
using engine::rhi::Backend;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;

namespace {

// The room is a 6 x 6 m box with walls 2.5 m high and a doorway in the front wall (z = 3), on a slab whose top is at y = 0. The camera looks
// down -Z from (0, 4, 14), so the floor inside the room is seen through the doorway. The open floor at x = 6 is outside the room.
const glm::vec3 kEye{0.0f, 4.0f, 14.0f};
constexpr std::uint32_t kSize = 64;
// The bake grid: probes at x and z every 2 m from -8 to 8, and at y = -1, 1, 3. The slab's top sits between the layers at -1 and 1, so the
// floor is covered on every corner. The probes at y = -1 are below the slab and see its underside.
const glm::vec3 kGridOrigin{-8.0f, -1.0f, -8.0f};
constexpr float kGridSpacing = 2.0f;
const glm::uvec3 kGridDims{9u, 3u, 9u};

// The pixel that shows a world point, for the camera above (vertical field 60 degrees, aspect 1, looking down -Z). Row 0 is the top row.
std::pair<std::uint32_t, std::uint32_t> project(const glm::vec3& point) {
    const float depth = kEye.z - point.z;
    const float half_height = depth * std::tan(0.5f * 1.0471975511965976f);
    const float x = (point.x - kEye.x) / half_height;
    const float y = (point.y - kEye.y) / half_height;
    const auto column = static_cast<std::uint32_t>(std::floor((x * 0.5f + 0.5f) * static_cast<float>(kSize)));
    const auto row = static_cast<std::uint32_t>(std::floor((1.0f - (y * 0.5f + 0.5f)) * static_cast<float>(kSize)));
    return {column, row};
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

PostShaders post_shaders(Backend backend) {
    PostShaders shaders;
    shaders.taa = ShaderBinary{ShaderStage::Compute, "CSTaa", load_shader(backend, "post_taa")};
    shaders.bloom_horizontal = ShaderBinary{ShaderStage::Compute, "CSBloomHorizontal", load_shader(backend, "post_bloom_horizontal")};
    shaders.bloom_vertical = ShaderBinary{ShaderStage::Compute, "CSBloomVertical", load_shader(backend, "post_bloom_vertical")};
    shaders.tonemap = ShaderBinary{ShaderStage::Compute, "CSTonemap", load_shader(backend, "post_tonemap")};
    return shaders;
}

SceneShaders scene_shaders(Backend backend) {
    const auto load = [backend](const std::string& name) { return load_shader(backend, name); };
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load("scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load("scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load("scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load("scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load("scene_shadow_pixel")};
    shaders.post = post_shaders(backend);
    shaders.reflections = ShaderBinary{ShaderStage::Compute, "CSReflections", load("scene_reflections")};
    return shaders;
}

// The post pass with TAA off, so each frame shows only itself: the probe and reflection comparisons would otherwise blend into the history.
PostProcessSettings test_settings() {
    PostProcessSettings settings;
    settings.enabled = true;
    settings.taa = false;
    settings.ev100 = 0.0f;
    settings.bloom.threshold = 1.0f;
    settings.bloom.knee = 0.5f;
    settings.bloom.intensity = 0.0f;
    return settings;
}

// A scene of axis-aligned boxes. Each box is the unit box scaled to its half extents, so one mesh serves every box.
struct Box {
    glm::vec3 center{0.0f};
    glm::vec3 half{1.0f};
    glm::vec4 color{1.0f};
    std::uint32_t material = 0;
    bool occluder = true;  // part of the bake's geometry
};

struct BoxScene {
    engine::scene::SceneGraph graph;
    std::vector<RenderableRef> renderables;
    std::vector<Box> boxes;
    Camera camera;
};

BoxScene build_scene(const std::vector<Box>& boxes) {
    BoxScene scene;
    scene.boxes = boxes;
    scene.camera.position = glm::dvec3(kEye);
    scene.camera.fov_y_radians = 1.0471975511965976;
    scene.camera.aspect = 1.0;
    scene.camera.near_plane = 0.1;
    scene.camera.far_plane = 1000.0;
    for (const Box& box : boxes) {
        engine::scene::Transform transform;
        transform.position = glm::dvec3(box.center);
        transform.scale = glm::dvec3(box.half);
        RenderableRef renderable;
        renderable.node = scene.graph.create(transform).value();
        renderable.mesh = 0;
        renderable.color = box.color;
        renderable.material = box.material;
        scene.renderables.push_back(renderable);
    }
    return scene;
}

// The room and the slab, as the bake and the renderer both see them. Walls are 2.5 m high; the roof sits on them at y = 2.5 to 2.7.
std::vector<Box> room_boxes() {
    const glm::vec4 white(0.6f, 0.6f, 0.6f, 1.0f);
    std::vector<Box> boxes;
    boxes.push_back(Box{glm::vec3(0.0f, -0.05f, 0.0f), glm::vec3(40.0f, 0.05f, 40.0f), white});            // slab, top at y = 0
    boxes.push_back(Box{glm::vec3(-3.1f, 1.25f, 0.0f), glm::vec3(0.1f, 1.25f, 3.0f), white});            // left wall
    boxes.push_back(Box{glm::vec3(3.1f, 1.25f, 0.0f), glm::vec3(0.1f, 1.25f, 3.0f), white});             // right wall
    boxes.push_back(Box{glm::vec3(0.0f, 1.25f, -3.1f), glm::vec3(3.0f, 1.25f, 0.1f), white});            // back wall
    boxes.push_back(Box{glm::vec3(-2.0f, 1.25f, 3.1f), glm::vec3(1.0f, 1.25f, 0.1f), white});            // front, left of the door
    boxes.push_back(Box{glm::vec3(2.0f, 1.25f, 3.1f), glm::vec3(1.0f, 1.25f, 0.1f), white});             // front, right of the door
    boxes.push_back(Box{glm::vec3(0.0f, 2.6f, 0.0f), glm::vec3(3.2f, 0.1f, 3.2f), white});               // roof
    return boxes;
}

std::string vec3_json(const glm::vec3& value) {
    std::ostringstream text;
    text << '[' << value.x << ", " << value.y << ", " << value.z << ']';
    return text.str();
}

// Bakes the room through the converter's gi_probes importer, so the volume the renderer reads is the payload the converter writes.
GiVolume bake_room(const std::vector<Box>& boxes) {
    // A uniform sky of radiance one, the same as the renderer's default environment, so an unshadowed probe is exactly the environment.
    std::ostringstream source;
    source << "{\"grid\": {\"origin\": " << vec3_json(kGridOrigin) << ", \"spacing\": " << kGridSpacing << ", \"dims\": [" << kGridDims.x << ", "
           << kGridDims.y << ", " << kGridDims.z << "]},\n\"sky\": {\"zenith\": [1, 1, 1], \"horizon\": [1, 1, 1], \"ground\": [1, 1, 1], "
           << "\"sun_radiance\": [0, 0, 0]},\n\"samples\": 512,\n\"occluders\": [";
    bool first = true;
    for (const Box& box : boxes) {
        if (!box.occluder) {
            continue;
        }
        source << (first ? "" : ",") << "\n{\"min\": " << vec3_json(box.center - box.half) << ", \"max\": " << vec3_json(box.center + box.half)
               << ", \"albedo\": " << vec3_json(glm::vec3(box.color)) << "}";
        first = false;
    }
    source << "]}";

    const engine::converter::ImporterRegistry converters = engine::converter::ImporterRegistry::builtin();
    const engine::converter::Importer* importer = converters.find("gi_probes");
    REQUIRE(importer != nullptr);
    const auto imported = importer->import(source.str(), "{}");
    REQUIRE(imported.ok());

    // The converter writes "<output>/<guid>.cooked", and the runtime loads it through the registry. The renderer gets that file's volume.
    const engine::core::Guid guid = engine::core::Guid::generate();
    const std::filesystem::path output = std::filesystem::temp_directory_path() / ("engine_gi_gpu_" + guid.to_string());
    std::filesystem::create_directories(output);
    {
        std::ofstream file(output / (guid.to_string() + ".cooked"), std::ios::binary);
        file << engine::assets::write_cooked_container(imported.value().payload);
    }
    const auto source_path = engine::core::Path::from_utf8("gi/room.json");
    const auto output_path = engine::core::Path::from_utf8(output.generic_string());
    REQUIRE(source_path.ok());
    REQUIRE(output_path.ok());
    engine::assets::AssetRegistry registry;
    REQUIRE(registry.add(engine::assets::AssetRecord{guid, source_path.value(), "gi_probes", "{}", engine::assets::ContentHash{}, {}}).ok());

    auto volume = engine::gi::load_gi_volume(registry, output_path.value(), source_path.value());
    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
    REQUIRE(volume.ok());
    return std::move(volume.value());
}

struct Harness {
    std::unique_ptr<Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::uint32_t box_mesh = 0;
};

Harness make_renderer(Backend backend, bool reflections) {
    Harness harness;
    harness.device = take(engine::rhi::create_device(debug_device(backend)));
    SceneRendererDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.post = test_settings();
    desc.screen_reflections = reflections;
    desc.reflections.max_steps = 256;
    desc.reflections.max_distance = 100.0f;
    desc.reflections.thickness = 0.5f;
    harness.renderer = take(SceneRenderer::create(*harness.device, desc, scene_shaders(backend)));
    harness.box_mesh = take(harness.renderer->upload_mesh(engine::render::make_box_mesh(glm::vec3(1.0f))));
    return harness;
}

// Renders the scene and returns a copy of the displayed image.
SceneImage render_scene(SceneRenderer& renderer, BoxScene& scene) {
    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer.bounds(), scene.renderables);
    require_ok(renderer.render(snapshot));
    return renderer.image();
}

int luminance(const SceneImage& image, std::uint32_t x, std::uint32_t y) {
    const auto& px = image.color[image.index(x, y)];
    return static_cast<int>(px[0]) + static_cast<int>(px[1]) + static_cast<int>(px[2]);
}

}  // namespace

namespace {

// The probe grid lights the room less than the open floor, and the environment lights both without it. The room's floor is seen through the
// doorway, so its probes see the roof and walls. The coverage view is green where every corner of the probe cell is valid, and red outside.
void run_gi_case(Backend backend) {
    Harness harness = make_renderer(backend, false);
    SceneRenderer& renderer = *harness.renderer;
    const std::vector<Box> boxes = room_boxes();
    BoxScene scene = build_scene(boxes);

    // Without probes the environment lights everything the same, so the room and the open floor are equally bright.
    const SceneImage without = render_scene(renderer, scene);
    const auto [room_x, room_y] = project(glm::vec3(0.0f, 0.0f, 0.0f));
    const auto [open_x, open_y] = project(glm::vec3(6.0f, 0.0f, 0.0f));
    const int room_off = luminance(without, room_x, room_y);
    const int open_off = luminance(without, open_x, open_y);
    CHECK(std::abs(room_off - open_off) < 12);

    const GiVolume volume = bake_room(boxes);
    require_ok(renderer.set_gi_volume(volume));
    CHECK(renderer.has_gi_volume());
    const SceneImage with = render_scene(renderer, scene);
    const int room_on = luminance(with, room_x, room_y);
    const int open_on = luminance(with, open_x, open_y);
    INFO("room off " << room_off << " on " << room_on << ", open off " << open_off << " on " << open_on);
    // The roof and walls take sky light from the room's probes, and the open floor keeps the environment's light.
    CHECK(room_on + 25 < room_off);
    CHECK(open_on > room_on + 25);
    // The open floor is darkened less than the room: its probes see more sky.
    CHECK(open_off - open_on < room_off - room_on);

    // The coverage view: the room's floor is covered on every corner, and a floor point beyond the grid is not covered at all.
    renderer.set_lighting_view(LightingView::ProbeCoverage);
    const SceneImage coverage = render_scene(renderer, scene);
    const auto& inside = coverage.color[coverage.index(room_x, room_y)];
    CHECK(inside[1] > inside[0] + 100);
    const auto [outside_x, outside_y] = project(glm::vec3(7.5f, 0.0f, -9.0f));
    const auto& outside = coverage.color[coverage.index(outside_x, outside_y)];
    CHECK(outside[0] > outside[1] + 100);
    renderer.set_lighting_view(LightingView::Lit);

    require_ok(renderer.clear_gi_volume());
    CHECK_FALSE(renderer.has_gi_volume());
    const SceneImage cleared = render_scene(renderer, scene);
    CHECK(luminance(cleared, room_x, room_y) == room_off);
    require_ok(harness.device->check_debug_messages());
    require_ok(renderer.release());
    require_ok(harness.device->check_debug_messages());
}

// A mirror slab reflects a red wall that is on screen. With the reflection pass the mirror shows the wall; without it, the mirror shows the
// environment, which is white. A pixel whose reflection is not traced keeps the environment in both frames.
void run_reflection_case(Backend backend) {
    Harness on = make_renderer(backend, true);
    Harness off = make_renderer(backend, false);
    std::vector<Box> boxes;
    engine::render::MaterialParams mirror;
    mirror.metallic = 1.0f;
    mirror.roughness = 0.05f;
    const std::uint32_t mirror_material = take(on.renderer->create_material(mirror));
    CHECK(take(off.renderer->create_material(mirror)) == mirror_material);
    boxes.push_back(Box{glm::vec3(0.0f, -0.05f, 0.0f), glm::vec3(40.0f, 0.05f, 40.0f), glm::vec4(1.0f), mirror_material, true});
    boxes.push_back(Box{glm::vec3(0.0f, 5.0f, -12.0f), glm::vec3(20.0f, 5.0f, 0.5f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), 0, true});
    BoxScene scene = build_scene(boxes);

    const SceneImage reference = render_scene(*off.renderer, scene);
    const SceneImage traced = render_scene(*on.renderer, scene);
    // (0, 0, 0) on the mirror reflects the wall at z = -12, which is on screen at about 3 m high.
    const auto [mirror_x, mirror_y] = project(glm::vec3(0.0f, 0.0f, 0.0f));
    const auto& before = reference.color[reference.index(mirror_x, mirror_y)];
    const auto& after = traced.color[traced.index(mirror_x, mirror_y)];
    INFO("reference " << int(before[0]) << "," << int(before[1]) << "," << int(before[2]) << " traced " << int(after[0]) << ","
                      << int(after[1]) << "," << int(after[2]));
    CHECK(std::abs(int(before[0]) - int(before[1])) < 12);
    CHECK(int(after[0]) > int(after[1]) + 60);
    // The background is the same in both frames: no geometry, so no reflection.
    CHECK(reference.color[reference.index(0, 0)] == traced.color[traced.index(0, 0)]);
    require_ok(on.device->check_debug_messages());

    // A resize rebuilds the targets, and the reflection set must follow them.
    require_ok(on.renderer->resize(kSize / 2, kSize / 2));
    const SceneImage resized = render_scene(*on.renderer, scene);
    CHECK(resized.width == kSize / 2);
    require_ok(on.device->check_debug_messages());
    require_ok(on.renderer->release());
    require_ok(off.renderer->release());
    require_ok(on.device->check_debug_messages());
}

void run_api_case(Backend backend) {
    Harness harness = make_renderer(backend, false);
    GiVolume volume = bake_room(room_boxes());
    GiVolume wrong_version = volume;
    wrong_version.format_version = volume.format_version + 1;
    CHECK_FALSE(harness.renderer->set_gi_volume(wrong_version).ok());
    GiVolume short_grid = volume;
    short_grid.probes.pop_back();
    CHECK_FALSE(harness.renderer->set_gi_volume(short_grid).ok());
    CHECK_FALSE(harness.renderer->has_gi_volume());
    require_ok(harness.renderer->set_gi_volume(volume));
    CHECK(harness.renderer->has_gi_volume());
    require_ok(harness.renderer->release());

    // Screen reflections need the post pass, because the reflection pass works on HDR color.
    SceneRendererDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.screen_reflections = true;
    auto rejected = engine::render::SceneRenderer::create(*harness.device, desc, scene_shaders(backend));
    CHECK_FALSE(rejected.ok());
    require_ok(harness.device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 GI: the probe grid darkens the room and leaves the open floor lit, and the coverage view shows it", "[gpu][dx12]") {
    run_gi_case(Backend::Dx12);
}

TEST_CASE("Vulkan GI: the probe grid darkens the room and leaves the open floor lit, and the coverage view shows it", "[gpu][vulkan]") {
    run_gi_case(Backend::Vulkan);
}

TEST_CASE("DX12 screen reflections: a mirror shows an on-screen wall, and the environment is kept where nothing is traced", "[gpu][dx12]") {
    run_reflection_case(Backend::Dx12);
}

TEST_CASE("Vulkan screen reflections: a mirror shows an on-screen wall, and the environment is kept where nothing is traced", "[gpu][vulkan]") {
    run_reflection_case(Backend::Vulkan);
}

TEST_CASE("DX12 GI and reflections: volumes that do not match are rejected, and reflections need the post pass", "[gpu][dx12]") {
    run_api_case(Backend::Dx12);
}

TEST_CASE("Vulkan GI and reflections: volumes that do not match are rejected, and reflections need the post pass", "[gpu][vulkan]") {
    run_api_case(Backend::Vulkan);
}
