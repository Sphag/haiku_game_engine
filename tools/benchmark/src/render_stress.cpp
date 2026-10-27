// render_stress (US-076). The scene comes from the seeded generator, so it has the table's counts and the same digests as
// engine_workload. The camera follows the manifest's route, and the frame index sets the camera position, so every run draws the same
// frame at the same index. Every frame records its counters; the start frame and the last frame are kept for the report.

#include "render_stress.hpp"

#include "asset_converter/mesh_cooking.hpp"
#include "asset_converter/model_format.hpp"

#include "engine/benchmark/measure.hpp"
#include "engine/benchmark/workload.hpp"
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/meshlet.hpp"
#include "engine/render/render_graph.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/scene/scene_graph.hpp"
#include "engine/scene/transform.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace engine::bench::render {
namespace {

namespace converter = engine::converter;
namespace rhi = engine::rhi;

using engine::render::Camera;
using engine::render::DrawPath;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::GeometryPath;
using engine::render::Light;
using engine::render::LightType;
using engine::render::MeshData;
using engine::render::MeshletData;
using engine::render::MeshletCluster;
using engine::render::RenderableRef;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::rhi::Backend;
using engine::rhi::Feature;

// The camera clock: frame n is at n / kRouteRateHz seconds along the route.
constexpr std::uint32_t kRouteRateHz = 60;

// Draw limits of the renderer, set for this scene. The defaults would send 200 meshes past the indirect group limit (64 groups, and
// the rest draw directly), and the meshlet pools cannot hold 200 cooked meshes.
constexpr std::uint32_t kMaxItems = 1u << 15;
constexpr std::uint32_t kMaxDrawGroups = 256;
constexpr std::uint32_t kMeshletVertices = 1u << 21;
constexpr std::uint32_t kMeshletTriangleBytes = 1u << 23;
constexpr std::uint32_t kMeshletMeshVertices = 1u << 20;

// Compiled shader names (tools/benchmark/CMakeLists.txt, outputs bench_<name>.dxil or .spv).
std::vector<std::uint8_t> read_binary(const std::string& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    ok = static_cast<bool>(file);
    if (!ok) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Loads one entry. Returns an empty binary when the file is missing, which the caller reports.
rhi::ShaderBinary load_entry(const RunSettings& settings, rhi::ShaderStage stage, const char* name, const char* entry, bool& ok) {
    const std::string extension = settings.backend == "dx12" ? ".dxil" : ".spv";
    const std::string path = settings.shader_dir + "/bench_" + name + extension;
    rhi::ShaderBinary binary{stage, entry, read_binary(path, ok)};
    if (!ok) {
        return {};
    }
    return binary;
}

// The scene shaders. A missing file fails the run with its name, so a build without the toolchain never measures a partial shader set.
core::Result<SceneShaders, std::string> load_shaders(const RunSettings& settings) {
    bool ok = true;
    SceneShaders shaders;
    shaders.vertex = load_entry(settings, rhi::ShaderStage::Vertex, "scene_vertex", "VSMain", ok);
    shaders.pixel = load_entry(settings, rhi::ShaderStage::Pixel, "scene_pixel", "PSMain", ok);
    shaders.pixel_blend = load_entry(settings, rhi::ShaderStage::Pixel, "scene_pixel_blend", "PSBlend", ok);
    shaders.shadow_vertex = load_entry(settings, rhi::ShaderStage::Vertex, "scene_shadow_vertex", "VSShadow", ok);
    shaders.shadow_pixel = load_entry(settings, rhi::ShaderStage::Pixel, "scene_shadow_pixel", "PSShadow", ok);
    shaders.indirect_vertex = load_entry(settings, rhi::ShaderStage::Vertex, "scene_indirect_vertex", "VSIndirect", ok);
    shaders.cull = load_entry(settings, rhi::ShaderStage::Compute, "scene_cull", "CSCull", ok);
    shaders.meshlet_task = load_entry(settings, rhi::ShaderStage::Amplification, "scene_meshlet_task", "ASMeshlet", ok);
    shaders.meshlet_mesh = load_entry(settings, rhi::ShaderStage::Mesh, "scene_meshlet_mesh", "MSMeshlet", ok);
    shaders.reflections = load_entry(settings, rhi::ShaderStage::Compute, "scene_reflections", "CSReflections", ok);
    shaders.post.taa = load_entry(settings, rhi::ShaderStage::Compute, "post_taa", "CSTaa", ok);
    shaders.post.bloom_horizontal = load_entry(settings, rhi::ShaderStage::Compute, "post_bloom_horizontal", "CSBloomHorizontal", ok);
    shaders.post.bloom_vertical = load_entry(settings, rhi::ShaderStage::Compute, "post_bloom_vertical", "CSBloomVertical", ok);
    shaders.post.tonemap = load_entry(settings, rhi::ShaderStage::Compute, "post_tonemap", "CSTonemap", ok);
    if (!ok) {
        return core::failure(std::string("the bench shaders are missing from ") + settings.shader_dir +
                             " (build engine_bench with the shader toolchain)");
    }
    return core::success(shaders);
}

// Cooks a generated mesh the way the asset converter does, and splits the result into the renderer's two inputs. The indexed and the
// meshlet paths both read this one cooked mesh, so the two paths draw the same triangles.
struct CookedMesh {
    MeshData mesh;
    MeshletData meshlets;
};

core::Result<CookedMesh, std::string> cook(const RenderMesh& source) {
    converter::ModelMesh model;
    model.positions.reserve(source.positions.size());
    for (const auto& p : source.positions) {
        model.positions.emplace_back(p[0], p[1], p[2]);
    }
    std::vector<std::uint32_t> triangles = source.indices;
    const auto cooked = converter::cook_mesh(model, std::move(triangles));
    if (!cooked.ok()) {
        return core::failure(std::string("cook_mesh failed: ") + cooked.error());
    }
    if (model.lods.empty()) {
        return core::failure(std::string("cook_mesh produced no LOD 0"));
    }
    const converter::MeshLod& lod = model.lods.front();
    CookedMesh out;
    out.mesh.positions = model.positions;
    out.mesh.normals = model.normals;
    out.mesh.uvs = model.uvs;
    out.mesh.indices = lod.indices;
    for (const converter::Meshlet& meshlet : lod.meshlets) {
        MeshletCluster cluster;
        cluster.vertex_offset = meshlet.vertex_offset;
        cluster.vertex_count = meshlet.vertex_count;
        cluster.triangle_offset = meshlet.triangle_offset;
        cluster.triangle_count = meshlet.triangle_count;
        cluster.center = meshlet.center;
        cluster.radius = meshlet.radius;
        cluster.cone_axis = meshlet.cone_axis;
        cluster.cone_cutoff = meshlet.cone_cutoff;
        out.meshlets.meshlets.push_back(cluster);
    }
    out.meshlets.vertices = lod.meshlet_vertices;
    out.meshlets.triangles = lod.meshlet_triangles;
    return core::success(std::move(out));
}

// The camera of frame `frame`: it starts at the route's start, looks along the heading, and moves at the route's speed. The height
// is above the terrain under the camera, so the view follows the same ground the instances stand on.
Camera camera_at(const CameraRoute& route, std::uint64_t frame, std::uint32_t width, std::uint32_t height) {
    const double t = static_cast<double>(frame) / kRouteRateHz;
    const double heading = glm::radians(route.heading_deg);
    const double x = route.start_x_m + std::sin(heading) * route.speed_m_per_s * t;
    const double z = route.start_z_m - std::cos(heading) * route.speed_m_per_s * t;
    const auto ground_mm = terrain_height_mm(generator_world().world_seed, static_cast<std::int64_t>(std::llround(x * 1000.0)),
                                             static_cast<std::int64_t>(std::llround(z * 1000.0)));
    Camera camera;
    camera.position = glm::dvec3(x, ground_mm / 1000.0 + route.height_m, z);
    // Identity looks down -Z. Yaw by -heading turns it toward (sin h, 0, -cos h); pitch then tilts it about the local X axis.
    camera.orientation = glm::angleAxis(glm::radians(-route.heading_deg), glm::dvec3(0.0, 1.0, 0.0)) *
                         glm::angleAxis(glm::radians(route.pitch_deg), glm::dvec3(1.0, 0.0, 0.0));
    camera.fov_y_radians = glm::radians(60.0);
    camera.aspect = static_cast<double>(width) / static_cast<double>(height);
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;  // the manifest's object draw distance
    return camera;
}

// Writes the image as a binary PPM (P6). The image is the displayed 8-bit output, top row first.
core::Result<core::Unit, std::string> write_ppm(const std::filesystem::path& path, const SceneImage& image) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return core::failure("cannot write " + path.string());
    }
    out << "P6\n" << image.width << " " << image.height << "\n255\n";
    for (const auto& pixel : image.color) {
        out.put(static_cast<char>(pixel[0]));
        out.put(static_cast<char>(pixel[1]));
        out.put(static_cast<char>(pixel[2]));
    }
    if (!out) {
        return core::failure("cannot write " + path.string());
    }
    return core::success();
}

// What one frame submitted and what the GPU kept of it.
struct FrameRecord {
    std::uint64_t items = 0;               // visible items the CPU extracted
    std::uint64_t shadow_only = 0;         // items that only cast shadows into the view
    std::uint64_t triangles = 0;           // triangles of the visible items (the CPU's count of what the paths submit)
    std::uint32_t indirect_candidates = 0;
    std::uint32_t indirect_visible = 0;    // GPU-kept, from indirect_stats()
    std::uint32_t indirect_drawn = 0;
    std::uint32_t indirect_overflow = 0;
    std::uint32_t meshlet_candidates = 0;  // (item, meshlet) pairs the task stage started with
    std::uint32_t meshlet_visible = 0;     // pairs the task stage kept
    bool indirect_active = false;
    bool meshlets_active = false;
    std::uint32_t lights_dropped = 0;
    double gpu_ms = 0.0;
    bool gpu_present = false;
};

// Everything a repeat needs to be built. Nothing here outlives the device it was made for.
struct Built {
    std::unique_ptr<SceneRenderer> renderer;
    engine::scene::SceneGraph graph;
    std::vector<RenderableRef> renderables;
    std::vector<Light> lights;
    std::vector<std::uint64_t> mesh_triangles;  // per uploaded mesh, cooked
    std::uint32_t mesh_count = 0;
};

// Creates the lights, the instances, and the meshes. `meshes` are the cooked meshes in upload order.
core::Result<core::Unit, std::string> populate(Built& built, const RenderContent& content, const std::vector<CookedMesh>& meshes,
                                               Path path) {
    for (const CookedMesh& cooked : meshes) {
        auto uploaded = built.renderer->upload_mesh(cooked.mesh);
        if (!uploaded.ok()) {
            return core::failure("upload_mesh: " + uploaded.error().message);
        }
        // Without mesh shaders the renderer resolves the meshlet path to its fallback (the indirect-indexed draws, FR-06), and attach_meshlets
        // would be refused. The meshlets are then not attached, and the run records the fallback.
        if (path == Path::Meshlets && !cooked.meshlets.meshlets.empty() && built.renderer->geometry_path() == GeometryPath::Meshlets) {
            const auto attached = built.renderer->attach_meshlets(uploaded.value(), cooked.meshlets);
            if (!attached.ok()) {
                return core::failure("attach_meshlets: " + attached.error().message);
            }
        }
        built.mesh_triangles.push_back(cooked.mesh.indices.size() / 3);
    }
    built.mesh_count = static_cast<std::uint32_t>(meshes.size());

    for (const RenderInstance& instance : content.instances) {
        engine::scene::Transform transform;
        transform.position = glm::dvec3(instance.position[0], instance.position[1], instance.position[2]);
        transform.rotation = glm::angleAxis(glm::radians(instance.yaw_deg), glm::dvec3(0.0, 1.0, 0.0));
        transform.scale = glm::dvec3(instance.scale);
        const auto node = built.graph.create(transform);
        if (!node.ok()) {
            return core::failure(std::string("scene graph refused an instance node"));
        }
        RenderableRef renderable;
        renderable.node = node.value();
        renderable.mesh = instance.mesh;
        renderable.color = glm::vec4(1.0f);
        built.renderables.push_back(renderable);
    }

    for (const RenderLight& local : content.lights) {
        Light light;
        light.id = local.id;
        light.type = LightType::Point;
        light.position = glm::dvec3(local.position[0], local.position[1], local.position[2]);
        light.color = glm::vec3(local.color[0], local.color[1], local.color[2]);
        light.intensity = static_cast<float>(local.intensity);
        light.range = static_cast<float>(local.radius_m);
        light.casts_shadow = local.shadowed;
        if (const auto problem = engine::render::validate_light(light)) {
            return core::failure("local light " + std::to_string(local.id) + ": " + *problem);
        }
        built.lights.push_back(light);
    }
    std::uint32_t sun_id = static_cast<std::uint32_t>(content.lights.size()) + 1;
    for (const RenderSun& sun : content.suns) {
        // The sun's direction is the way its light travels: from the sun down through the scene.
        const double azimuth = glm::radians(sun.azimuth_deg);
        const double elevation = glm::radians(sun.elevation_deg);
        const glm::vec3 towards_sun(static_cast<float>(std::cos(elevation) * std::sin(azimuth)), static_cast<float>(std::sin(elevation)),
                                    static_cast<float>(std::cos(elevation) * std::cos(azimuth)));
        Light light;
        light.id = sun_id++;
        light.type = LightType::Directional;
        light.direction = -glm::normalize(towards_sun);
        light.color = glm::vec3(1.0f);
        light.intensity = 3.0f;
        light.casts_shadow = true;
        if (const auto problem = engine::render::validate_light(light)) {
            return core::failure("sun: " + *problem);
        }
        built.lights.push_back(light);
    }
    return core::success();
}

// The features this run used, from the device and the renderer. A feature that is off is reported off, with the reason, never enabled.
void record_features(const rhi::CapabilityReport& caps, const SceneRenderer& renderer, const RunSettings& settings, const FrameRecord& last,
                     bool meshlet_requested, RunRecord& run) {
    if (settings.disable_mesh_shaders) {
        run.features["mesh_shaders"] = "disabled: forced off by --disable-mesh-shaders";
    } else if (!caps.enabled(Feature::MeshShaders)) {
        run.features["mesh_shaders"] = "unavailable on this adapter";
    } else {
        run.features["mesh_shaders"] = "enabled";
    }
    if (meshlet_requested) {
        run.features["meshlet_path"] = renderer.geometry_path() == GeometryPath::Meshlets ? "active" : "fell back to indirect-indexed draws (mesh shaders unavailable)";
    }
    run.features["indirect_draws"] = last.indirect_active ? "active" : "inactive (no indirect groups drawn on the last frame)";
    run.features["temporal_aa"] = renderer.post_enabled() ? "enabled" : "disabled";
    run.features["screen_space_reflections"] = "enabled";
    run.features["ray_traced_shadows"] = renderer.ray_traced_active() ? "enabled" : "disabled (native profile)";
    run.features["volumetric_fog"] = "not wired into render_stress";
    run.features["baked_probe_gi"] = "not wired into render_stress";
    run.features["skinned_characters"] = "not spawned by render_stress yet";
    run.features["gpu_particles"] = "not spawned by render_stress yet";
}

}  // namespace

std::string_view to_string(Path path) noexcept {
    switch (path) {
        case Path::Indexed: return "indexed";
        case Path::Indirect: return "indirect";
        case Path::Meshlets: return "meshlets";
    }
    return "unknown";
}

std::optional<Path> parse_path(std::string_view text) noexcept {
    if (text == "indexed") return Path::Indexed;
    if (text == "indirect") return Path::Indirect;
    if (text == "meshlets") return Path::Meshlets;
    return std::nullopt;
}

void run_render_stress(const SceneDef& scene, const std::string& profile_name, const ProfileDef& profile, const RunSettings& settings,
                       RunRecord& run) {
    run.path = std::string(to_string(settings.path));
    if (profile_name == "full_1080") {
        // The full profile needs FSR 2 upscaling and the ray-traced effects. FSR 2 is not built (its shader tables are missing), so the
        // profile cannot be measured, and it is reported as unsupported rather than relabelled as native.
        run.status = RunStatus::Unsupported;
        run.reason = "full_1080 needs FSR 2 upscaling, which this build does not include (ENGINE_FSR2_AVAILABLE is off)";
        return;
    }
    if (profile_name != "native_1080" || profile.output_width == 0) {
        run.status = RunStatus::Incomplete;
        run.reason = "render_stress has no rendering for profile " + profile_name;
        return;
    }
    if (!scene.camera.has_value()) {
        run.status = RunStatus::Incomplete;
        run.reason = "render_stress has no camera route in the manifest";
        return;
    }
    const auto start_floor = scene.counts.find("min_submitted_triangles_indexed_start_frame");
    const std::uint64_t min_start_triangles = start_floor == scene.counts.end() ? 0 : start_floor->second;

    // Content first, so a count mismatch is reported before any device work.
    const auto generated = generate_render_content(scene);
    if (!generated.ok()) {
        run.status = RunStatus::Incomplete;
        run.reason = "workload: " + generated.error();
        return;
    }
    const RenderContent& content = generated.value();
    std::vector<CookedMesh> meshes;
    for (const RenderMesh& source : content.meshes) {
        auto cooked = cook(source);
        if (!cooked.ok()) {
            run.status = RunStatus::Incomplete;
            run.reason = cooked.error();
            return;
        }
        meshes.push_back(std::move(cooked).value());
    }

    const auto shaders = load_shaders(settings);
    if (!shaders.ok()) {
        run.status = RunStatus::Incomplete;
        run.reason = shaders.error();
        return;
    }

    if (!settings.screenshot_dir.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(settings.screenshot_dir, ignored);
    }

    rhi::DeviceDesc device_desc;
    device_desc.backend = settings.backend == "dx12" ? Backend::Dx12 : Backend::Vulkan;
    device_desc.debug_layers = false;  // performance runs have no validation overhead
    device_desc.allow_software_adapter = false;
    if (settings.disable_mesh_shaders) {
        device_desc.disabled_features = Feature::MeshShaders;
    }
    auto device = rhi::create_device(device_desc);
    if (!device.ok()) {
        run.status = RunStatus::Unsupported;
        run.reason = device.error().message;
        return;
    }
    const rhi::CapabilityReport caps = device.value()->capabilities();

    SceneRendererDesc desc;
    desc.width = profile.output_width;
    desc.height = profile.output_height;
    desc.draw_path = settings.path == Path::Indexed ? DrawPath::Indexed : DrawPath::Indirect;
    desc.geometry = settings.path == Path::Meshlets ? GeometryPath::Meshlets : GeometryPath::Conventional;
    desc.max_items = kMaxItems;
    desc.max_draw_groups = kMaxDrawGroups;
    desc.meshlets.vertices = kMeshletVertices;
    desc.meshlets.triangle_bytes = kMeshletTriangleBytes;
    desc.meshlets.mesh_vertices = kMeshletMeshVertices;
    desc.post.enabled = true;  // the native profile's TAA runs in the post pass
    desc.post.taa = true;
    desc.screen_reflections = true;

    Built built;
    auto renderer = SceneRenderer::create(*device.value(), desc, shaders.value());
    if (!renderer.ok()) {
        run.status = RunStatus::Incomplete;
        run.reason = "scene renderer: " + renderer.error().message;
        return;
    }
    built.renderer = std::move(renderer).value();
    const auto populated = populate(built, content, meshes, settings.path);
    if (!populated.ok()) {
        run.status = RunStatus::Incomplete;
        run.reason = populated.error();
        if (built.renderer) {
            static_cast<void>(built.renderer->release());
        }
        return;
    }

    // The frame. A failed render ends the measurement: the frame is not counted, and the run is incomplete.
    std::string failure_text;
    std::vector<FrameRecord> records;
    std::vector<double> gpu_ms;
    std::uint64_t frame_index = 0;
    bool gpu_all_present = true;
    const std::filesystem::path shots = settings.screenshot_dir;
    const std::function<void()> frame = [&]() {
        if (!failure_text.empty()) {
            return;
        }
        const std::uint64_t index = frame_index++;
        const Camera camera = camera_at(*scene.camera, index, profile.output_width, profile.output_height);
        const FrameSnapshot snapshot = extract_frame(built.graph, camera, built.renderer->bounds(), built.renderables, built.lights);
        FrameRecord record;
        for (const engine::render::SnapshotItem& item : snapshot.items()) {
            if (item.pick_id == 0) {
                ++record.shadow_only;
                continue;
            }
            ++record.items;
            record.triangles += built.mesh_triangles.at(item.mesh);
        }
        // A game never reads the frame back, so the timed frames skip the copy. The start frame keeps it only for its screenshot.
        built.renderer->set_readback(index == 0 && !shots.empty());
        const auto status = built.renderer->render(snapshot);
        if (!status.ok()) {
            failure_text = "render frame " + std::to_string(index) + ": " + status.error().message;
            return;
        }
        const auto& indirect = built.renderer->indirect_stats();
        const auto& meshlet = built.renderer->meshlet_stats();
        record.indirect_active = indirect.active;
        record.indirect_candidates = indirect.candidates;
        record.indirect_visible = indirect.visible;
        record.indirect_drawn = indirect.drawn;
        record.indirect_overflow = indirect.overflow;
        record.meshlets_active = meshlet.active;
        record.meshlet_candidates = meshlet.candidates;
        record.meshlet_visible = meshlet.visible;
        record.lights_dropped = built.renderer->lighting_stats().dropped_lights;
        double total = 0.0;
        bool present = true;
        for (const auto& pass : built.renderer->frame_passes()) {
            if (pass.gpu_ms.has_value()) {
                total += *pass.gpu_ms;
            } else {
                present = false;
            }
        }
        record.gpu_present = present && !built.renderer->frame_passes().empty();
        record.gpu_ms = total;
        gpu_all_present = gpu_all_present && record.gpu_present;
        gpu_ms.push_back(total);
        records.push_back(record);
        if (index == 0 && !shots.empty()) {
            const auto written = write_ppm(shots / (settings.name + "-start.ppm"), built.renderer->image());
            if (!written.ok()) {
                failure_text = written.error();
            }
        }
    };

    std::uint64_t frames_started = 0;
    const std::vector<double> cpu_ms = run_frames(settings.warmup_s, settings.duration_s, frame, frames_started);
    run.frames_started = frames_started;
    if (!failure_text.empty()) {
        run.status = RunStatus::Incomplete;
        run.reason = failure_text;
        static_cast<void>(built.renderer->release());
        return;
    }
    if (cpu_ms.empty() || records.empty()) {
        run.status = RunStatus::Incomplete;
        run.reason = "no frames were measured";
        static_cast<void>(built.renderer->release());
        return;
    }

    // Measured frames are the contiguous tail of the frames run_frames timed (its last frame has no interval).
    const std::size_t first = records.size() - 1 - cpu_ms.size();
    run.frames.cpu_ms = cpu_ms;
    run.frames.gpu_present = gpu_all_present;
    if (gpu_all_present) {
        run.frames.gpu_ms.assign(gpu_ms.begin() + static_cast<std::ptrdiff_t>(first), gpu_ms.end() - 1);
    }
    run.cpu = summarize(run.frames.cpu_ms);
    if (gpu_all_present) {
        run.gpu = summarize(run.frames.gpu_ms);
        std::vector<double> frame_ms;
        for (std::size_t i = 0; i < run.frames.cpu_ms.size(); ++i) {
            frame_ms.push_back(std::max(run.frames.cpu_ms[i], run.frames.gpu_ms[i]));
        }
        run.frame = summarize(frame_ms);
    }

    const FrameRecord& start = records.front();
    const FrameRecord& last = records.back();
    run.counters["frames_started"] = frames_started;
    run.counters["frames_rendered"] = records.size();
    run.counters["measured_frames"] = cpu_ms.size();
    run.counters["start_items_drawn"] = start.items;
    run.counters["start_shadow_only_items"] = start.shadow_only;
    run.counters["start_triangles_in_view"] = start.triangles;
    run.counters["last_items_drawn"] = last.items;
    run.counters["last_triangles_in_view"] = last.triangles;
    run.counters["start_indirect_candidates"] = start.indirect_candidates;
    run.counters["start_indirect_visible"] = start.indirect_visible;
    run.counters["start_indirect_drawn"] = start.indirect_drawn;
    run.counters["start_indirect_overflow"] = start.indirect_overflow;
    run.counters["start_meshlet_candidates"] = start.meshlet_candidates;
    run.counters["start_meshlet_visible"] = start.meshlet_visible;
    run.counters["last_indirect_visible"] = last.indirect_visible;
    run.counters["last_meshlet_visible"] = last.meshlet_visible;
    run.counters["lights"] = built.lights.size();
    run.counters["lights_dropped_last_frame"] = last.lights_dropped;
    run.counters["renderables"] = built.renderables.size();
    run.counters["meshes"] = built.mesh_count;
    std::uint64_t measured_items = 0;
    for (std::size_t i = first; i < records.size(); ++i) {
        measured_items += records[i].items;
    }
    run.counters["mean_items_drawn"] = measured_items / cpu_ms.size();

    // The last frame's image is still in the renderer's target; the start image was written by frame 0.
    if (!shots.empty()) {
        const auto end_shot = write_ppm(shots / (settings.name + "-end.ppm"), built.renderer->image());
        if (!end_shot.ok()) {
            run.checks.push_back({"end screenshot", false, end_shot.error()});
        }
    }

    // The indexed reference path must submit the start-frame triangle floor (manifest: min_submitted_triangles_indexed_start_frame).
    if (settings.path == Path::Indexed) {
        const bool passed = start.triangles >= min_start_triangles;
        run.checks.push_back({"start frame submits at least " + std::to_string(min_start_triangles) + " triangles (indexed reference path)",
                              passed, std::to_string(start.triangles) + " triangles in view on frame 0"});
    }

    record_features(caps, *built.renderer, settings, last, settings.path == Path::Meshlets, run);

    // The frame-time verdict. Residency is not measured yet, so the contract cannot pass; a timing threshold that is exceeded still fails.
    engine::bench::RepeatEvidence evidence;
    evidence.frames = run.frames;
    const ContractResult contract = evaluate_contract(evidence);
    run.checks.insert(run.checks.end(), contract.checks.begin(), contract.checks.end());
    std::vector<std::string> open_items = {"skinned characters", "GPU particles", "volumetric fog", "baked probe GI"};
    std::string reason = "render_stress does not yet draw";
    for (std::size_t i = 0; i < open_items.size(); ++i) {
        reason += (i == 0 ? " " : (i + 1 == open_items.size() ? " and " : ", ")) + open_items[i];
    }
    reason += "; GPU residency is not measured";
    run.reason = reason;
    if (contract.verdict == Verdict::Failed) {
        run.status = RunStatus::Failed;
        run.reason = "a frame-time threshold was exceeded: " + contract.reason;
    } else {
        run.status = RunStatus::Incomplete;
    }
    static_cast<void>(built.renderer->release());
}

}  // namespace engine::bench::render
