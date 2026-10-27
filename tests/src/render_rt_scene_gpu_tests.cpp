// Ray-traced shadows and reflections through the scene renderer on real GPUs (US-043). The reference scene of render_rt_fixture.hpp is built as
// renderer objects: the box and the fence quad are meshes, the fence is an alpha-mask material, and the mirror is metallic. Each case runs DX12
// and Vulkan with the debug layers on, and any validation error fails the check. The cases cover: the sun's shadow of a moving occluder, with the
// pass on and off (the off run is the reference, since no shadow map is cast), and a vertical mirror that shows a pillar outside the view, with
// the pass on and off. The pillar is kept in the snapshot by keep_offscreen; the scene draw does not draw it. The last case runs the full
// profile's dynamic GI (US-044): the scene's diffuse comes from the traced probe grid, and it responds when the sun goes off.
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/pbr.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"
#include "engine/scene/scene_graph.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using engine::render::Camera;
using engine::render::EquirectImage;
using engine::render::ImageRgba8;
using engine::render::Light;
using engine::render::LightType;
using engine::render::MaterialParams;
using engine::render::MaterialTextures;
using engine::render::MeshData;
using engine::render::RayTracedSettings;
using engine::render::RenderableRef;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::rhi::Backend;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;

namespace {

constexpr std::uint32_t kSize = 64;

// The camera of the reference scene: at (0, 1.5, 6), pitched down toward the scene, with a 60 degree vertical field and a square image.
Camera reference_camera() {
    Camera camera;
    camera.position = glm::dvec3(0.0, 1.5, 6.0);
    camera.orientation = glm::angleAxis(-std::atan(1.0 / 8.0), glm::dvec3(1.0, 0.0, 0.0));
    camera.fov_y_radians = 1.0471975511965976;
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;
    return camera;
}

// The pixel that shows a world point, through the renderer's own view matrix. Row 0 is the top row. The column and row may lie outside the image.
std::pair<std::int32_t, std::int32_t> project(const Camera& camera, const glm::vec3& point) {
    const glm::mat4 view_projection = engine::render::camera_view_projection(camera);
    const glm::vec3 relative = glm::vec3(glm::dvec3(point) - camera.position);
    const glm::vec4 clip = view_projection * glm::vec4(relative, 1.0f);
    const float x = clip.x / clip.w;
    const float y = clip.y / clip.w;
    const auto column = static_cast<std::int32_t>(std::floor((x * 0.5f + 0.5f) * static_cast<float>(kSize)));
    const auto row = static_cast<std::int32_t>(std::floor((1.0f - (y * 0.5f + 0.5f)) * static_cast<float>(kSize)));
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

SceneShaders scene_shaders(Backend backend) {
    const auto load = [backend](const std::string& name) { return load_shader(backend, name); };
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load("scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load("scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load("scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load("scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load("scene_shadow_pixel")};
    shaders.ray_traced = ShaderBinary{ShaderStage::Compute, "CSRayTracedEffects", load("rt_effects")};
    shaders.dynamic_gi = ShaderBinary{ShaderStage::Compute, "CSDynamicGi", load("dynamic_gi")};
    // The denoiser and the fog (US-045), and the post pass that the fog needs for its HDR color.
    shaders.denoise = ShaderBinary{ShaderStage::Compute, "CSTemporalDenoise", load("denoise_temporal")};
    shaders.fog_inject = ShaderBinary{ShaderStage::Compute, "CSFogInject", load("volumetric_fog_inject")};
    shaders.fog_integrate = ShaderBinary{ShaderStage::Compute, "CSFogIntegrate", load("volumetric_fog_integrate")};
    shaders.fog_apply = ShaderBinary{ShaderStage::Compute, "CSFogApply", load("volumetric_fog_apply")};
    shaders.post.taa = ShaderBinary{ShaderStage::Compute, "CSTaa", load("post_taa")};
    shaders.post.bloom_horizontal = ShaderBinary{ShaderStage::Compute, "CSBloomHorizontal", load("post_bloom_horizontal")};
    shaders.post.bloom_vertical = ShaderBinary{ShaderStage::Compute, "CSBloomVertical", load("post_bloom_vertical")};
    shaders.post.tonemap = ShaderBinary{ShaderStage::Compute, "CSTonemap", load("post_tonemap")};
    return shaders;
}

// The sun: from above and behind the fence (z < 0), so its shadows fall toward the camera, on the floor it can see. The light's direction is
// the way it travels. No shadow map is cast, so the only shadows are the traced ones.
Light sun() {
    Light light;
    light.id = 1;
    light.type = LightType::Directional;
    light.direction = glm::normalize(glm::vec3(0.0f, -1.0f, 0.3f));
    light.color = glm::vec3(1.0f);
    light.intensity = 1.0f;
    light.casts_shadow = false;
    return light;
}

// One object of the reference scene. Mesh 0 is the unit box, mesh 1 the fence quad.
struct Prop {
    glm::vec3 center{0.0f};
    glm::vec3 half{1.0f};
    glm::vec4 color{1.0f};
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
};

struct Harness {
    std::unique_ptr<Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::uint32_t fence_material = 0;
    std::uint32_t mirror_material = 0;
    std::uint32_t rough_material = 0;
    engine::scene::SceneGraph graph;
    std::vector<RenderableRef> renderables;
};

MeshData fence_quad() {
    MeshData mesh;
    mesh.positions = {{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}};
    mesh.uvs = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// The reference scene: a floor, the pillar off screen, the moving occluder, the masked fence, the rough box, and the mirror. The occluder's x is
// the only change between frames. The environment is dim (0.2), so the sun's contribution is the difference a shadow shows.
std::vector<Prop> reference_props(const Harness& harness, float occluder_x) {
    return {
        Prop{glm::vec3(0.0f, -0.05f, 0.0f), glm::vec3(20.0f, 0.05f, 20.0f), glm::vec4(0.55f, 0.55f, 0.55f, 1.0f), 0, 0},
        Prop{glm::vec3(-6.0f, 2.0f, 3.0f), glm::vec3(0.5f, 2.0f, 0.5f), glm::vec4(0.9f, 0.1f, 0.1f, 1.0f), 0, 0},
        Prop{glm::vec3(occluder_x, 1.0f, 1.0f), glm::vec3(0.5f, 1.0f, 0.5f), glm::vec4(0.8f, 0.8f, 0.9f, 1.0f), 0, 0},
        Prop{glm::vec3(1.5f, 1.5f, -3.0f), glm::vec3(1.5f, 1.5f, 1.0f), glm::vec4(0.2f, 0.6f, 0.9f, 1.0f), 1, harness.fence_material},
        Prop{glm::vec3(0.0f, 0.5f, 2.0f), glm::vec3(0.5f, 0.5f, 0.5f), glm::vec4(0.2f, 0.8f, 0.2f, 1.0f), 0, harness.rough_material},
        Prop{glm::vec3(-2.25f, 2.0f, -3.0f), glm::vec3(1.75f, 2.0f, 0.05f), glm::vec4(0.95f, 0.95f, 0.95f, 1.0f), 0, harness.mirror_material},
    };
}

// Builds the renderer with its meshes and materials. The fence's material is alpha-masked by a 4 x 4 checkerboard; the mirror is metallic and
// smooth; the rough box is above the reflection cutoff. The props' material indices are set by the caller (see reference_props).
// The fog needs the post pass (its HDR color), so volumetric_fog turns it on.
Harness make_renderer(Backend backend, bool ray_traced, bool dynamic_gi = false, bool denoise = false, bool volumetric_fog = false) {
    Harness harness;
    harness.device = take(engine::rhi::create_device(debug_device(backend)));
    SceneRendererDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.ray_traced.enabled = ray_traced;
    desc.ray_traced.roughness_cutoff = 0.4f;
    desc.ray_traced.reflection_distance = 100.0f;
    desc.ray_traced.shadow_distance = 1000.0f;
    desc.ray_traced.dynamic_gi = dynamic_gi;
    desc.ray_traced.denoise = denoise;
    desc.ray_traced.volumetric_fog = volumetric_fog;
    desc.ray_traced.fog.density = 0.0f;
    desc.post.enabled = volumetric_fog;
    harness.renderer = take(SceneRenderer::create(*harness.device, desc, scene_shaders(backend)));

    EquirectImage environment;
    environment.width = 16;
    environment.height = 8;
    environment.texels.assign(static_cast<std::size_t>(environment.width) * environment.height, glm::vec3(0.2f));
    require_ok(harness.renderer->set_environment(environment));

    // The box is mesh 0 and the fence quad mesh 1, as reference_props names them.
    take(harness.renderer->upload_mesh(engine::render::make_box_mesh(glm::vec3(1.0f))));
    take(harness.renderer->upload_mesh(fence_quad()));

    ImageRgba8 checker;
    checker.width = 4;
    checker.height = 4;
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint8_t alpha = (x + y) % 2 == 0 ? 255 : 0;
            checker.pixels.insert(checker.pixels.end(), {255, 255, 255, alpha});
        }
    }
    MaterialTextures fence_textures;
    fence_textures.base_color = take(harness.renderer->upload_texture(checker));
    MaterialParams fence;
    fence.blend = engine::render::BlendMode::AlphaMask;
    fence.alpha_cutoff = 0.5f;
    fence.roughness = 0.5f;
    harness.fence_material = take(harness.renderer->create_material(fence, fence_textures));

    MaterialParams mirror;
    mirror.metallic = 1.0f;
    mirror.roughness = 0.05f;
    harness.mirror_material = take(harness.renderer->create_material(mirror));

    MaterialParams rough;
    rough.roughness = 0.9f;
    harness.rough_material = take(harness.renderer->create_material(rough));
    return harness;
}

// Places the props in the scene graph. Each frame adds its nodes; only the renderables are read, so the nodes of earlier frames are inert.
void place(Harness& harness, const std::vector<Prop>& props) {
    harness.renderables.clear();
    for (const Prop& prop : props) {
        engine::scene::Transform transform;
        transform.position = glm::dvec3(prop.center);
        transform.scale = glm::dvec3(prop.half);
        RenderableRef renderable;
        renderable.node = harness.graph.create(transform).value();
        renderable.mesh = prop.mesh;
        renderable.color = prop.color;
        renderable.material = prop.material;
        harness.renderables.push_back(renderable);
    }
}

// Renders one frame of the reference scene and returns the displayed image. Off-screen objects stay in the snapshot, so a mirror can show them.
SceneImage render_reference(Harness& harness, const std::vector<Prop>& props, const Camera& camera, float sun_intensity = 1.0f) {
    place(harness, props);
    Light light = sun();
    light.intensity = sun_intensity;
    const std::vector<Light> lights{light};
    const engine::render::FrameSnapshot snapshot = engine::render::extract_frame(harness.graph, camera, harness.renderer->bounds(),
                                                                                  harness.renderables, lights, {}, true);
    require_ok(harness.renderer->render(snapshot));
    return harness.renderer->image();
}

// The sum of the red, green, and blue values over the whole image: the light the frame holds.
std::int64_t total_light(const SceneImage& image) {
    std::int64_t total = 0;
    for (const auto& px : image.color) {
        total += static_cast<std::int64_t>(px[0]) + px[1] + px[2];
    }
    return total;
}

int luminance(const SceneImage& image, std::int32_t x, std::int32_t y) {
    const auto& px = image.color[image.index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))];
    return static_cast<int>(px[0]) + static_cast<int>(px[1]) + static_cast<int>(px[2]);
}

// Luminance at a world point, through the renderer's projection.
int luminance_at(const SceneImage& image, const Camera& camera, const glm::vec3& point) {
    const auto [x, y] = project(camera, point);
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < static_cast<std::int32_t>(kSize));
    REQUIRE(y < static_cast<std::int32_t>(kSize));
    return luminance(image, x, y);
}

// Counts the pixels around a world point that a red pillar's reflection would color: red clearly above green and blue.
int reddish_pixels_near(const SceneImage& image, const Camera& camera, const glm::vec3& point, std::int32_t radius) {
    const auto [cx, cy] = project(camera, point);
    int count = 0;
    for (std::int32_t y = cy - radius; y <= cy + radius; ++y) {
        for (std::int32_t x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(kSize) || y >= static_cast<std::int32_t>(kSize)) {
                continue;
            }
            const auto& px = image.color[image.index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))];
            if (px[0] > px[1] + 40 && px[0] > px[2] + 40) {
                ++count;
            }
        }
    }
    return count;
}

void run_shadow_case(Backend backend) {
    const Camera camera = reference_camera();
    // Two floor points in front of the occluder's shadow (the shadow lies toward the camera, from z = 0.5 to 2.1 under the box). Each is visible
    // from the camera, and only one is shadowed in a given frame.
    const glm::vec3 left_floor(-1.2f, 0.0f, 1.8f);
    const glm::vec3 right_floor(1.2f, 0.0f, 1.8f);

    Harness traced = make_renderer(backend, true);
    const SceneImage traced_left = render_reference(traced, reference_props(traced, -1.2f), camera);
    const int traced_left_shadowed = luminance_at(traced_left, camera, left_floor);
    const int traced_left_clear = luminance_at(traced_left, camera, right_floor);
    CHECK(traced.renderer->ray_traced_instances() == 6);

    // The occluder moves to the right: the shadow moves with it, and the left floor is lit again.
    const SceneImage traced_right = render_reference(traced, reference_props(traced, 1.2f), camera);
    const int traced_left_clear_moved = luminance_at(traced_right, camera, left_floor);
    const int traced_right_shadowed = luminance_at(traced_right, camera, right_floor);
    INFO("traced: left shadowed " << traced_left_shadowed << ", right clear " << traced_left_clear << ", left after move " << traced_left_clear_moved
                                  << ", right shadowed " << traced_right_shadowed);
    CHECK(traced_left_shadowed + 40 < traced_left_clear);
    CHECK(traced_right_shadowed + 40 < traced_left_clear_moved);
    require_ok(traced.device->check_debug_messages());

    // The reference: the same scene with the pass off. No shadow map is cast, so neither floor point is darkened.
    Harness raster = make_renderer(backend, false);
    const SceneImage raster_image = render_reference(raster, reference_props(raster, -1.2f), camera);
    const int raster_left = luminance_at(raster_image, camera, left_floor);
    const int raster_right = luminance_at(raster_image, camera, right_floor);
    INFO("raster: left " << raster_left << ", right " << raster_right << ", traced shadowed " << traced_left_shadowed);
    CHECK(std::abs(raster_left - raster_right) < 12);
    CHECK(raster_left > traced_left_shadowed + 40);
    CHECK_FALSE(raster.renderer->ray_traced_active());
    require_ok(raster.device->check_debug_messages());
}

void run_reflection_case(Backend backend) {
    const Camera camera = reference_camera();
    // The pillar is outside the view: its projection lies beyond the image's left edge.
    const auto [pillar_column, pillar_row] = project(camera, glm::vec3(-6.0f, 2.0f, 3.0f));
    (void)pillar_row;
    CHECK(pillar_column < 0);
    // The pillar's image in the mirror (the mirror plane is z = -3) is where its reflected ray meets the mirror, on screen.
    const glm::vec3 image_point(-3.6f, 1.8f, -3.0f);

    Harness traced = make_renderer(backend, true);
    const SceneImage on = render_reference(traced, reference_props(traced, 3.0f), camera);
    const int traced_red = reddish_pixels_near(on, camera, image_point, 3);
    INFO("reddish pixels with the pass on " << traced_red);
    CHECK(traced_red > 0);
    CHECK(traced.renderer->ray_traced_instances() == 6);
    require_ok(traced.device->check_debug_messages());

    Harness raster = make_renderer(backend, false);
    const SceneImage off = render_reference(raster, reference_props(raster, 3.0f), camera);
    const int raster_red = reddish_pixels_near(off, camera, image_point, 3);
    INFO("reddish pixels with the pass off " << raster_red);
    CHECK(raster_red == 0);
    require_ok(raster.device->check_debug_messages());
}

// Whether the last frame ran a pass of this name, and where in the order the graph ran it (-1 when it did not).
int pass_index(const SceneRenderer& renderer, const std::string& name) {
    const auto& passes = renderer.frame_passes();
    for (std::size_t i = 0; i < passes.size(); ++i) {
        if (passes[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The denoiser and the fog run as passes of their own, between the ray-traced pass and the scene, or after the scene. The fog changes the
// frame, and a zero density leaves it as it was; a camera cut then discards the history, and the frame stays stable.
void run_denoise_fog_case(Backend backend) {
    const Camera camera = reference_camera();

    Harness plain = make_renderer(backend, true);
    render_reference(plain, reference_props(plain, -1.2f), camera);
    CHECK(pass_index(*plain.renderer, "denoise shadow") == -1);
    CHECK(pass_index(*plain.renderer, "fog apply") == -1);
    require_ok(plain.device->check_debug_messages());

    Harness effects = make_renderer(backend, true, false, true, true);
    RayTracedSettings settings;
    settings.enabled = true;
    settings.roughness_cutoff = 0.4f;
    settings.reflection_distance = 100.0f;
    settings.shadow_distance = 1000.0f;
    settings.denoise = true;
    settings.volumetric_fog = true;
    settings.fog.density = 0.0f;
    require_ok(effects.renderer->set_ray_traced_settings(settings));

    SceneImage clear;
    for (int frame = 0; frame < 4; ++frame) {
        clear = render_reference(effects, reference_props(effects, -1.2f), camera);
    }
    CHECK(effects.renderer->ray_traced_instances() == 6);
    const int traced = pass_index(*effects.renderer, "ray-traced effects");
    const int denoise_shadow = pass_index(*effects.renderer, "denoise shadow");
    const int denoise_reflection = pass_index(*effects.renderer, "denoise reflection");
    const int inject = pass_index(*effects.renderer, "fog inject");
    const int integrate = pass_index(*effects.renderer, "fog integrate");
    const int scene = pass_index(*effects.renderer, "scene");
    const int apply = pass_index(*effects.renderer, "fog apply");
    INFO("passes: traced " << traced << ", denoise " << denoise_shadow << "/" << denoise_reflection << ", inject " << inject << ", integrate "
                           << integrate << ", scene " << scene << ", apply " << apply);
    CHECK(traced >= 0);
    CHECK(denoise_shadow > traced);
    CHECK(denoise_reflection > denoise_shadow);
    CHECK(inject > denoise_reflection);
    CHECK(integrate > inject);
    CHECK(scene > integrate);
    CHECK(apply > scene);
    require_ok(effects.device->check_debug_messages());

    // The fog at a real density changes the frame. The fog adds scattered light and takes out the light behind it, so the total moves either
    // way; the check is that it moves.
    settings.fog.density = 0.1f;
    require_ok(effects.renderer->set_ray_traced_settings(settings));
    SceneImage fogged;
    for (int frame = 0; frame < 6; ++frame) {
        fogged = render_reference(effects, reference_props(effects, -1.2f), camera);
    }
    const std::int64_t clear_light = total_light(clear);
    const std::int64_t fogged_light = total_light(fogged);
    INFO("total light: fog density 0 " << clear_light << ", fog density 0.1 " << fogged_light);
    CHECK(std::abs(static_cast<double>(fogged_light - clear_light)) > 0.03 * static_cast<double>(clear_light));
    require_ok(effects.device->check_debug_messages());

    // A camera cut: the camera jumps, and the next frame restarts the histories. The frame still renders and the passes still run.
    Camera moved = camera;
    moved.position += glm::dvec3(30.0, 0.0, 0.0);
    render_reference(effects, reference_props(effects, -1.2f), moved);
    CHECK(pass_index(*effects.renderer, "fog apply") >= 0);
    require_ok(effects.device->check_debug_messages());
}

void run_dynamic_case(Backend backend) {
    const Camera camera = reference_camera();
    Harness dynamic = make_renderer(backend, true, true);
    // The grid updates one sixteenth of its probes per frame, so 16 frames trace every probe once. The history then holds real traces.
    constexpr int kFillFrames = 16;
    for (int frame = 0; frame < kFillFrames; ++frame) {
        render_reference(dynamic, reference_props(dynamic, -1.2f), camera, 1.0f);
    }
    const SceneImage lit = render_reference(dynamic, reference_props(dynamic, -1.2f), camera, 1.0f);
    CHECK(dynamic.renderer->ray_traced_instances() == 6);

    // The sun goes off. The lighting change cuts every probe to a few samples, so the bounce follows the light within a few frames, and the
    // direct sun leaves the image. The frame must lose most of its light, and it must not go to black: the environment still lights it.
    SceneImage dark;
    for (int frame = 0; frame < 8; ++frame) {
        dark = render_reference(dynamic, reference_props(dynamic, -1.2f), camera, 0.0f);
    }
    const std::int64_t lit_light = total_light(lit);
    const std::int64_t dark_light = total_light(dark);
    INFO("dynamic GI total light: sun on " << lit_light << ", sun off " << dark_light);
    CHECK(static_cast<double>(lit_light) > 1.2 * static_cast<double>(dark_light));
    CHECK(dark_light > 0);
    require_ok(dynamic.device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 ray-traced scene: the sun's shadow follows a moving occluder, and the pass off shows none", "[gpu][dx12]") {
    run_shadow_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced scene: the sun's shadow follows a moving occluder, and the pass off shows none", "[gpu][vulkan]") {
    run_shadow_case(Backend::Vulkan);
}

TEST_CASE("DX12 ray-traced scene: a vertical mirror shows the off-screen pillar, and the pass off shows the environment", "[gpu][dx12]") {
    run_reflection_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced scene: a vertical mirror shows the off-screen pillar, and the pass off shows the environment", "[gpu][vulkan]") {
    run_reflection_case(Backend::Vulkan);
}

TEST_CASE("DX12 ray-traced scene: the denoiser and the fog run as passes of their own, and the fog changes the frame", "[gpu][dx12]") {
    run_denoise_fog_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced scene: the denoiser and the fog run as passes of their own, and the fog changes the frame", "[gpu][vulkan]") {
    run_denoise_fog_case(Backend::Vulkan);
}

TEST_CASE("DX12 ray-traced scene: the full profile's dynamic GI traces the grid, and it follows the sun", "[gpu][dx12]") {
    run_dynamic_case(Backend::Dx12);
}

TEST_CASE("Vulkan ray-traced scene: the full profile's dynamic GI traces the grid, and it follows the sun", "[gpu][vulkan]") {
    run_dynamic_case(Backend::Vulkan);
}
