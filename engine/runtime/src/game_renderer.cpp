#include "engine/runtime/game_renderer.hpp"

#include "asset_converter/model_format.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/assets/texture_format.hpp"
#include "engine/gameplay/animation_binding.hpp"
#include "engine/runtime/runtime_material.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/gameplay/components.hpp"
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/scene_renderer.hpp"
#include "engine/render/terrain_mesh.hpp"
#include "engine/world/terrain.hpp"
#include "engine/world/terrain_edit.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <variant>
#include <vector>

namespace engine::runtime {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kMaxPoolParticles = 96;  // a pool entity per particle; a larger max_particles is capped here

template <typename T>
T read(const scene::Scene& scene, const core::Guid& entity, const char* type, const char* field, T fallback) {
    const auto value = scene.field(entity, type, field);
    if (value && std::holds_alternative<T>(*value)) {
        return std::get<T>(*value);
    }
    return fallback;
}

bool has_component(const scene::Scene& scene, const core::Guid& entity, const char* type) {
    const auto types = scene.component_types(entity);
    return std::find(types.begin(), types.end(), type) != types.end();
}

std::string describe(const rhi::RhiError& error) {
    return error.message;
}

std::vector<std::uint8_t> load_shader(rhi::Backend backend, const std::string& dir, const std::string& name, std::string& problem) {
    const std::string file = dir + "/" + name + (backend == rhi::Backend::Dx12 ? ".dxil" : ".spv");
    auto path = core::Path::from_utf8(file);
    if (!path.ok()) {
        problem = "shader path is not valid UTF-8: " + file;
        return {};
    }
    auto bytes = core::read_file(path.value());
    if (!bytes.ok()) {
        problem = "cannot read the shader " + file;
        return {};
    }
    return std::vector<std::uint8_t>(bytes.value().begin(), bytes.value().end());
}

// One drawable piece of a cooked model: a mesh the renderer holds, and the renderer's material for it.
struct ModelPart {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
};

struct CachedModel {
    bool ok = false;
    bool skinned = false;
    std::vector<ModelPart> parts;
    animation::Skeleton skeleton;
    animation::ClipLibrary clips;
};

struct TerrainEntry {
    std::vector<std::uint32_t> tiles;  // renderer mesh indices
};

struct Particle {
    bool alive = false;
    float age = 0.0f;
    glm::dvec3 position{0.0};
    glm::vec3 velocity{0.0f};
};

struct Pool {
    core::Guid emitter;
    std::vector<core::Guid> entities;  // scene entities the particles are drawn through
    std::vector<Particle> particles;
    double carry = 0.0;
    std::uint32_t seed = 1;
};

glm::mat4 node_world(const std::vector<converter::ModelNode>& nodes, std::size_t index, std::vector<glm::mat4>& cache,
                     std::vector<bool>& done) {
    if (done[index]) {
        return cache[index];
    }
    const converter::ModelNode& node = nodes[index];
    cache[index] = node.parent < 0 ? node.local : node_world(nodes, static_cast<std::size_t>(node.parent), cache, done) * node.local;
    done[index] = true;
    return cache[index];
}

}  // namespace

struct GameRenderer::Impl {
    rhi::Device* device = nullptr;
    rhi::Backend backend = rhi::Backend::Dx12;
    AssetReader reader;
    std::unique_ptr<render::SceneRenderer> renderer;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t box_mesh = 0;
    std::uint32_t sphere_mesh = 0;
    bool skinned_shaders = false;
    bool terrain_shaders = false;
    rhi::BufferHandle staging;
    std::uint64_t staging_bytes = 0;
    rhi::FenceHandle fence;
    std::uint64_t fence_value = 0;
    std::array<std::uint8_t, 3> sky{115, 158, 217};  // the environment's sky_color in RGB 0..255
    std::vector<std::uint8_t> overlay;                // RGBA, straight alpha
    std::uint32_t overlay_width = 0;
    std::uint32_t overlay_height = 0;

    std::map<core::Guid, CachedModel> models;
    std::map<core::Guid, std::uint32_t> textures;                      // by texture asset: the renderer's texture id
    std::map<core::Guid, std::optional<std::uint32_t>> instances;      // by material asset: the renderer's material, or nothing when it failed
    std::map<core::Guid, TerrainEntry> terrains;  // by terrain entity
    std::map<core::Guid, std::string> animator_names;  // by entity that has an animator
    std::vector<Pool> pools;

    const CachedModel& model(const core::Guid& asset, std::vector<std::string>* problems);
    // The renderer material of a material asset, built once. A failure is reported once and the model's own materials are used instead.
    std::optional<std::uint32_t> instance_material(const core::Guid& asset, std::vector<std::string>* problems);
    void load_terrain(scene::Scene& scene, const core::Guid& entity, std::vector<std::string>& problems);
};

std::optional<std::uint32_t> GameRenderer::Impl::instance_material(const core::Guid& asset, std::vector<std::string>* problems) {
    const auto cached = instances.find(asset);
    if (cached != instances.end()) {
        return cached->second;
    }
    std::optional<std::uint32_t>& slot = instances[asset];
    const auto fail = [&](const std::string& message) {
        if (problems != nullptr) {
            problems->push_back("material " + asset.to_string() + ": " + message);
        }
        return std::optional<std::uint32_t>();
    };
    const std::optional<std::string> text = reader ? reader(asset) : std::nullopt;
    if (!text) {
        return fail("not in the packs");
    }
    auto recipe = resolve_material(*text, reader);
    if (!recipe.ok()) {
        return fail(recipe.error());
    }
    if (problems != nullptr) {
        for (const std::string& note : recipe.value().notes) {
            problems->push_back("material " + asset.to_string() + ": " + note);
        }
    }
    render::MaterialTextures bound;
    for (const auto& [slot_name, texture_asset] : recipe.value().textures) {
        std::optional<std::uint32_t> id;
        if (const auto uploaded = textures.find(texture_asset); uploaded != textures.end()) {
            id = uploaded->second;
        } else {
            const std::optional<std::string> payload = reader ? reader(texture_asset) : std::nullopt;
            if (!payload) {
                return fail("the texture " + texture_asset.to_string() + " is not in the packs");
            }
            auto image = decode_texture_payload(*payload);
            if (!image.ok()) {
                return fail(image.error());
            }
            auto created = renderer->upload_texture(image.value());
            if (!created.ok()) {
                return fail(describe(created.error()));
            }
            textures[texture_asset] = created.value();
            id = created.value();
        }
        if (slot_name == kSlotBaseColor) {
            bound.base_color = id;
        } else if (slot_name == kSlotNormal) {
            bound.normal = id;
        } else if (slot_name == kSlotMetallicRoughness) {
            bound.metallic_roughness = id;
        } else if (slot_name == kSlotEmissive) {
            bound.emissive = id;
        } else if (slot_name == kSlotOcclusion) {
            bound.occlusion = id;
        }
    }
    auto material = renderer->create_material(recipe.value().params, bound);
    if (!material.ok()) {
        return fail(describe(material.error()));
    }
    slot = material.value();
    return slot;
}

const CachedModel& GameRenderer::Impl::model(const core::Guid& asset, std::vector<std::string>* problems) {
    const auto found = models.find(asset);
    if (found != models.end()) {
        return found->second;
    }
    CachedModel& out = models[asset];
    const auto fail = [&](const std::string& message) -> const CachedModel& {
        if (problems != nullptr) {
            problems->push_back("model " + asset.to_string() + ": " + message + "; a primitive is drawn instead");
        }
        out.ok = false;
        return out;
    };
    if (!reader) {
        return fail("no asset reader");
    }
    const std::optional<std::string> bytes = reader(asset);
    if (!bytes) {
        return fail("not in the packs");
    }
    auto parsed = converter::read_model_payload(*bytes);
    if (!parsed.ok()) {
        return fail(parsed.error());
    }
    const converter::ModelPayload& payload = parsed.value();

    // The renderer's materials, one per payload material. A mesh without one uses the renderer's default (index 0).
    std::vector<std::uint32_t> materials;
    for (const converter::ModelMaterial& source : payload.materials) {
        render::MaterialParams params;
        params.base_color = source.base_color;
        params.metallic = source.metallic;
        params.roughness = source.roughness;
        params.emissive = source.emissive;
        params.alpha_cutoff = source.alpha_cutoff;
        params.two_sided = source.double_sided;
        params.blend = source.alpha_mode == converter::AlphaMode::Blend  ? render::BlendMode::AlphaBlend
                       : source.alpha_mode == converter::AlphaMode::Mask ? render::BlendMode::AlphaMask
                                                                          : render::BlendMode::Opaque;
        auto created = renderer->create_material(params);
        materials.push_back(created.ok() ? created.value() : 0u);
    }

    std::vector<glm::mat4> world(payload.nodes.size());
    std::vector<bool> done(payload.nodes.size(), false);
    for (std::size_t n = 0; n < payload.nodes.size(); ++n) {
        static_cast<void>(node_world(payload.nodes, n, world, done));
    }

    out.skinned = !payload.joints.empty() && skinned_shaders;
    const auto mesh_data = [&payload](std::size_t index, const glm::mat4* bake, bool with_skin) {
        const converter::ModelMesh& source = payload.meshes[index];
        render::MeshData mesh;
        mesh.positions = source.positions;
        mesh.normals = source.normals;
        mesh.uvs = source.uvs;
        mesh.indices = source.lods.front().indices;
        if (with_skin) {
            mesh.joints = source.joints;
            mesh.weights = source.weights;
        }
        if (bake != nullptr) {
            const glm::mat3 normal_matrix = glm::inverseTranspose(glm::mat3(*bake));
            for (glm::vec3& position : mesh.positions) {
                position = glm::vec3(*bake * glm::vec4(position, 1.0f));
            }
            for (glm::vec3& normal : mesh.normals) {
                normal = glm::normalize(normal_matrix * normal);
            }
        }
        return mesh;
    };
    const auto material_of = [&](std::int32_t index) {
        return index >= 0 && static_cast<std::size_t>(index) < materials.size() ? materials[static_cast<std::size_t>(index)] : 0u;
    };

    if (out.skinned) {
        // A skinned mesh is uploaded once, in the model's own space; the palette moves it.
        for (std::size_t m = 0; m < payload.meshes.size(); ++m) {
            if (payload.meshes[m].joints.empty()) {
                continue;
            }
            auto uploaded = renderer->upload_mesh(mesh_data(m, nullptr, true));
            if (!uploaded.ok()) {
                return fail(describe(uploaded.error()));
            }
            out.parts.push_back({uploaded.value(), material_of(payload.meshes[m].material)});
        }
        if (auto built = gameplay::animation_from_payload(payload)) {
            out.skeleton = std::move(built->skeleton);
            out.clips = std::move(built->clips);
        }
    } else {
        // A static mesh is drawn once per node that places it, with the node's world transform baked into the uploaded vertices, so the
        // entity's own transform is all the renderer needs.
        for (std::size_t n = 0; n < payload.nodes.size(); ++n) {
            for (const std::uint32_t m : payload.nodes[n].meshes) {
                auto uploaded = renderer->upload_mesh(mesh_data(m, &world[n], false));
                if (!uploaded.ok()) {
                    return fail(describe(uploaded.error()));
                }
                out.parts.push_back({uploaded.value(), material_of(payload.meshes[m].material)});
            }
        }
    }
    out.ok = !out.parts.empty();
    if (!out.ok) {
        return fail("it has no mesh to draw");
    }
    return out;
}

void GameRenderer::Impl::load_terrain(scene::Scene& scene, const core::Guid& entity, std::vector<std::string>& problems) {
    if (terrains.count(entity) != 0) {
        return;
    }
    if (!terrain_shaders) {
        problems.push_back("terrain " + entity.to_string() + ": the terrain shaders are not in the shaders folder");
        return;
    }
    const core::Guid asset = read<core::Guid>(scene, entity, gameplay::kTerrainId, "heightmap", core::Guid{});
    const std::optional<std::string> bytes = reader && !asset.is_nil() ? reader(asset) : std::nullopt;
    if (!bytes) {
        problems.push_back("terrain " + entity.to_string() + ": its height map is not in the packs");
        return;
    }
    auto parsed = world::parse_terrain(*bytes);
    if (!parsed.ok()) {
        problems.push_back("terrain " + entity.to_string() + ": " + parsed.error());
        return;
    }
    world::Terrain terrain = std::move(parsed).value();
    const float height_scale = read<float>(scene, entity, gameplay::kTerrainId, "height_scale", 1.0f);
    for (float& sample : terrain.heights) {
        sample *= height_scale;
    }
    const std::vector<std::uint32_t> lods(static_cast<std::size_t>(terrain.desc.tiles_x) * terrain.desc.tiles_z, 0u);
    auto tiles = world::build_terrain_meshes(terrain, lods);
    if (!tiles.ok()) {
        problems.push_back("terrain " + entity.to_string() + ": " + tiles.error());
        return;
    }
    TerrainEntry entry;
    for (const world::TerrainTileMesh& tile : tiles.value()) {
        auto render_tile = render::make_terrain_render_tile(tile);
        if (!render_tile.ok()) {
            problems.push_back("terrain " + entity.to_string() + ": " + render_tile.error());
            return;
        }
        auto uploaded = renderer->upload_terrain_mesh(render_tile.value().mesh, render_tile.value().splat);
        if (!uploaded.ok()) {
            problems.push_back("terrain " + entity.to_string() + ": " + describe(uploaded.error()));
            return;
        }
        entry.tiles.push_back(uploaded.value());
    }
    terrains[entity] = std::move(entry);
}

GameRenderer::GameRenderer() : impl_(std::make_unique<Impl>()) {}

GameRenderer::~GameRenderer() {
    if (impl_ != nullptr && impl_->device != nullptr) {
        static_cast<void>(impl_->device->wait_idle());
        impl_->renderer.reset();
        if (impl_->staging.valid()) {
            static_cast<void>(impl_->device->destroy(impl_->staging));
        }
        if (impl_->fence.valid()) {
            static_cast<void>(impl_->device->destroy(impl_->fence));
        }
    }
}

core::Result<std::unique_ptr<GameRenderer>, std::string> GameRenderer::create(rhi::Device& device, rhi::Backend backend,
                                                                              const std::string& shader_dir, std::uint32_t width,
                                                                              std::uint32_t height, AssetReader reader) {
    if (width == 0 || height == 0 || width % 64 != 0) {
        return core::failure(std::string("the render width must be a positive multiple of 64"));
    }
    std::string problem;
    render::SceneShaders shaders;
    const auto load = [&](rhi::ShaderStage stage, const char* entry, const char* name) {
        return rhi::ShaderBinary{stage, entry, load_shader(backend, shader_dir, name, problem)};
    };
    shaders.vertex = load(rhi::ShaderStage::Vertex, "VSMain", "scene_vertex");
    shaders.pixel = load(rhi::ShaderStage::Pixel, "PSMain", "scene_pixel");
    shaders.pixel_blend = load(rhi::ShaderStage::Pixel, "PSBlend", "scene_pixel_blend");
    shaders.shadow_vertex = load(rhi::ShaderStage::Vertex, "VSShadow", "scene_shadow_vertex");
    shaders.shadow_pixel = load(rhi::ShaderStage::Pixel, "PSShadow", "scene_shadow_pixel");
    if (!problem.empty()) {
        return core::failure(problem + " (the game needs its shaders folder beside the executable)");
    }
    // Skinning and terrain are optional shader sets: without them the renderer still draws everything else.
    std::string optional_problem;
    const auto optional = [&](rhi::ShaderStage stage, const char* entry, const char* name) {
        return rhi::ShaderBinary{stage, entry, load_shader(backend, shader_dir, name, optional_problem)};
    };
    shaders.skinned_vertex = optional(rhi::ShaderStage::Vertex, "VSSkinned", "scene_skinned_vertex");
    shaders.skinned_shadow_vertex = optional(rhi::ShaderStage::Vertex, "VSSkinnedShadow", "scene_skinned_shadow_vertex");
    const bool skinned_ok = optional_problem.empty();
    shaders.terrain_vertex = optional(rhi::ShaderStage::Vertex, "VSTerrain", "scene_terrain_vertex");
    shaders.terrain_shadow_vertex = optional(rhi::ShaderStage::Vertex, "VSTerrainShadow", "scene_terrain_shadow_vertex");
    const bool terrain_ok = optional_problem.empty();
    if (!skinned_ok) {
        shaders.skinned_vertex = {};
        shaders.skinned_shadow_vertex = {};
    }
    if (!terrain_ok) {
        shaders.terrain_vertex = {};
        shaders.terrain_shadow_vertex = {};
    }

    render::SceneRendererDesc desc;
    desc.width = width;
    desc.height = height;
    desc.draw_path = render::DrawPath::Indexed;
    desc.geometry = render::GeometryPath::Conventional;
    auto created = render::SceneRenderer::create(device, desc, shaders);
    if (!created.ok()) {
        return core::failure("cannot create the scene renderer: " + describe(created.error()));
    }

    std::unique_ptr<GameRenderer> out(new GameRenderer());
    Impl& impl = *out->impl_;
    impl.device = &device;
    impl.backend = backend;
    impl.reader = std::move(reader);
    impl.renderer = std::move(created).value();
    impl.width = width;
    impl.height = height;
    impl.skinned_shaders = skinned_ok;
    impl.terrain_shaders = terrain_ok;
    auto box = impl.renderer->upload_mesh(render::make_box_mesh(glm::vec3(0.5f)));
    auto sphere = impl.renderer->upload_mesh(render::make_sphere_mesh(0.5f, 16, 32));
    if (!box.ok() || !sphere.ok()) {
        return core::failure(std::string("cannot upload the primitive meshes"));
    }
    impl.box_mesh = box.value();
    impl.sphere_mesh = sphere.value();
    auto fence = device.create_fence(0);
    if (!fence.ok()) {
        return core::failure("cannot create the present fence: " + describe(fence.error()));
    }
    impl.fence = std::move(fence).value();
    return core::success(std::move(out));
}

const render::SceneImage& GameRenderer::image() const {
    return impl_->renderer->image();
}

std::vector<std::string> GameRenderer::bind_scene(scene::Scene& scene, const std::map<core::Guid, std::string>& animator_names,
                                                  bool particles) {
    std::vector<std::string> problems;
    impl_->animator_names = animator_names;
    for (const core::Guid& guid : scene.entity_guids()) {
        if (has_component(scene, guid, gameplay::kTerrainId)) {
            impl_->load_terrain(scene, guid, problems);
        }
        if (has_component(scene, guid, gameplay::kMeshRendererId)) {
            // Materials are built now, so a missing graph or texture is reported at start and not as a surprise on screen.
            const core::Guid material = read<core::Guid>(scene, guid, gameplay::kMeshRendererId, "material", core::Guid{});
            if (!material.is_nil()) {
                static_cast<void>(impl_->instance_material(material, &problems));
            }
        }
        if (particles && has_component(scene, guid, gameplay::kParticleEmitterId) &&
            std::none_of(impl_->pools.begin(), impl_->pools.end(), [&guid](const Pool& existing) { return existing.emitter == guid; })) {
            Pool pool;
            pool.emitter = guid;
            const std::uint32_t wanted = read<std::uint32_t>(scene, guid, gameplay::kParticleEmitterId, "max_particles", 64u);
            const std::uint32_t count = std::min(wanted, kMaxPoolParticles);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto added = scene.add_entity(scene::Transform{});
                if (!added.ok()) {
                    problems.push_back("particles " + guid.to_string() + ": " + added.error().message);
                    break;
                }
                pool.entities.push_back(added.value());
            }
            pool.particles.resize(pool.entities.size());
            pool.seed = 2166136261u ^ static_cast<std::uint32_t>(impl_->pools.size() + 1) * 16777619u;
            impl_->pools.push_back(std::move(pool));
        }
    }
    return problems;
}

void GameRenderer::reset_scene() {
    impl_->terrains.clear();
    impl_->animator_names.clear();
    impl_->pools.clear();
}

core::Result<core::Unit, std::string> GameRenderer::resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || width % 64 != 0) {
        return core::failure(std::string("the render width must be a positive multiple of 64"));
    }
    if (width == impl_->width && height == impl_->height) {
        return core::success();
    }
    if (auto resized = impl_->renderer->resize(width, height); !resized.ok()) {
        return core::failure("cannot resize the scene renderer: " + describe(resized.error()));
    }
    impl_->width = width;
    impl_->height = height;
    return core::success();
}

std::uint32_t GameRenderer::width() const noexcept {
    return impl_->width;
}

std::uint32_t GameRenderer::height() const noexcept {
    return impl_->height;
}

core::Result<RenderStats, std::string> GameRenderer::render(scene::Scene& scene, animation::AnimatorSet* animators, double dt,
                                                           const RenderView& view) {
    RenderStats stats;
    render::Camera camera;
    camera.aspect = static_cast<double>(impl_->width) / static_cast<double>(impl_->height);
    camera.position = glm::dvec3(0.0, 2.0, 8.0);

    std::vector<render::Light> lights;
    std::vector<render::RenderableRef> renderables;
    std::deque<render::SkinPose> poses;  // stays valid until extract_frame has copied the palettes
    std::uint32_t light_id = 1;

    // Particles first: they move their pool entities, which the loop below then draws like any other mesh.
    for (Pool& pool : impl_->pools) {
        const glm::dmat4* emitter_world = scene.world_matrix(pool.emitter);
        if (emitter_world == nullptr) {
            continue;
        }
        const glm::dvec3 origin((*emitter_world)[3]);
        const float rate = read<float>(scene, pool.emitter, gameplay::kParticleEmitterId, "rate", 50.0f);
        const float lifetime = std::max(read<float>(scene, pool.emitter, gameplay::kParticleEmitterId, "lifetime", 2.0f), 0.05f);
        const float speed = read<float>(scene, pool.emitter, gameplay::kParticleEmitterId, "speed", 2.0f);
        const float size = read<float>(scene, pool.emitter, gameplay::kParticleEmitterId, "size", 0.1f);
        const auto random01 = [&pool]() {
            pool.seed = pool.seed * 1664525u + 1013904223u;
            return static_cast<float>((pool.seed >> 8) & 0xFFFFFFu) / 16777215.0f;
        };
        pool.carry += static_cast<double>(rate) * dt;
        for (std::size_t i = 0; i < pool.particles.size(); ++i) {
            Particle& particle = pool.particles[i];
            if (particle.alive) {
                particle.age += static_cast<float>(dt);
                particle.velocity.y -= 0.3f * static_cast<float>(dt);
                particle.position += glm::dvec3(particle.velocity) * dt;
                if (particle.age >= lifetime) {
                    particle.alive = false;
                }
            }
            if (!particle.alive && pool.carry >= 1.0) {
                pool.carry -= 1.0;
                particle.alive = true;
                particle.age = 0.0f;
                particle.position = origin;
                const float angle = random01() * 6.2831853f;
                const float spread = random01() * 0.35f;
                particle.velocity = glm::vec3(std::cos(angle) * spread, 1.0f, std::sin(angle) * spread) * speed * (0.6f + 0.4f * random01());
            }
            scene::Transform pose;
            pose.position = particle.position;
            const double shrink = particle.alive ? std::max(0.2, 1.0 - static_cast<double>(particle.age / lifetime)) : 0.0;
            pose.scale = glm::dvec3(std::max(static_cast<double>(size) * shrink, 1.0e-4));
            static_cast<void>(scene.set_transform(pool.entities[i], pose));
        }
        pool.carry = std::min(pool.carry, 4.0);
    }

    for (const core::Guid& guid : scene.entity_guids()) {
        const glm::dmat4* world = scene.world_matrix(guid);
        const auto handle = scene.handle(guid);
        if (world == nullptr || !handle) {
            continue;
        }
        const glm::dvec3 position((*world)[3]);
        glm::dmat3 axes(*world);
        for (int column = 0; column < 3; ++column) {
            const double length = glm::length(axes[column]);
            if (length > 0.0) {
                axes[column] /= length;
            }
        }
        const glm::dquat rotation = glm::normalize(glm::quat_cast(axes));

        if (!view.camera && !stats.camera && has_component(scene, guid, gameplay::kCameraId) && read<bool>(scene, guid, gameplay::kCameraId, "primary", true)) {
            camera.position = position;
            camera.orientation = rotation;
            camera.fov_y_radians = read<float>(scene, guid, gameplay::kCameraId, "vertical_fov_degrees", 60.0f) * kPi / 180.0;
            camera.near_plane = read<float>(scene, guid, gameplay::kCameraId, "near_plane", 0.1f);
            camera.far_plane = read<float>(scene, guid, gameplay::kCameraId, "far_plane", 1000.0f);
            stats.camera = true;
        }

        if (has_component(scene, guid, gameplay::kEnvironmentId)) {
            const auto sky = read<std::array<float, 3>>(scene, guid, gameplay::kEnvironmentId, "sky_color", {0.45f, 0.62f, 0.85f});
            for (int channel = 0; channel < 3; ++channel) {
                impl_->sky[channel] = static_cast<std::uint8_t>(std::clamp(sky[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }

        if (has_component(scene, guid, gameplay::kLightId)) {
            render::Light light;
            light.id = light_id++;
            const auto kind = read<std::int32_t>(scene, guid, gameplay::kLightId, "kind", 0);
            light.type = kind == 1 ? render::LightType::Point : kind == 2 ? render::LightType::Spot : render::LightType::Directional;
            light.position = position;
            light.direction = glm::vec3(glm::normalize(rotation * glm::dvec3(0.0, 0.0, -1.0)));
            const auto color = read<std::array<float, 3>>(scene, guid, gameplay::kLightId, "color", {1.0f, 1.0f, 1.0f});
            light.color = glm::vec3(color[0], color[1], color[2]);
            light.intensity = read<float>(scene, guid, gameplay::kLightId, "intensity", 1.0f);
            light.range = read<float>(scene, guid, gameplay::kLightId, "range", 20.0f);
            const float cone = read<float>(scene, guid, gameplay::kLightId, "cone_angle_degrees", 45.0f) * static_cast<float>(kPi) / 180.0f;
            light.outer_angle = std::clamp(cone * 0.5f, 0.05f, 1.5f);
            light.inner_angle = light.outer_angle * 0.8f;
            light.casts_shadow = read<bool>(scene, guid, gameplay::kLightId, "cast_shadows", false);
            lights.push_back(light);
            ++stats.lights;
        }

        if (has_component(scene, guid, gameplay::kTerrainId)) {
            const auto entry = impl_->terrains.find(guid);
            if (entry != impl_->terrains.end()) {
                for (const std::uint32_t tile : entry->second.tiles) {
                    render::RenderableRef item;
                    item.node = *handle;
                    item.mesh = tile;
                    renderables.push_back(item);
                    ++stats.terrain_tiles;
                }
            }
        }

        if (has_component(scene, guid, gameplay::kMeshRendererId) && read<bool>(scene, guid, gameplay::kMeshRendererId, "visible", true)) {
            const auto tint_values = read<std::array<float, 4>>(scene, guid, gameplay::kMeshRendererId, "tint", {1.0f, 1.0f, 1.0f, 1.0f});
            const glm::vec4 tint(tint_values[0], tint_values[1], tint_values[2], tint_values[3]);
            const core::Guid asset = read<core::Guid>(scene, guid, gameplay::kMeshRendererId, "mesh", core::Guid{});
            const core::Guid material_asset = read<core::Guid>(scene, guid, gameplay::kMeshRendererId, "material", core::Guid{});
            const std::optional<std::uint32_t> override_material =
                material_asset.is_nil() ? std::nullopt : impl_->instance_material(material_asset, nullptr);
            const CachedModel* cached = asset.is_nil() ? nullptr : &impl_->model(asset, nullptr);
            if (cached != nullptr && cached->ok) {
                const render::SkinPose* skin = nullptr;
                if (cached->skinned) {
                    const auto name = impl_->animator_names.find(guid);
                    animation::StateMachine* machine =
                        animators != nullptr && name != impl_->animator_names.end() ? animators->find(name->second) : nullptr;
                    if (machine != nullptr) {
                        render::SkinPose pose;
                        pose.palette = animation::skinning_palette(cached->skeleton, machine->pose());
                        poses.push_back(std::move(pose));
                        skin = &poses.back();
                    }
                    ++stats.skinned;
                }
                for (const ModelPart& part : cached->parts) {
                    render::RenderableRef item;
                    item.node = *handle;
                    item.mesh = part.mesh;
                    item.color = tint;
                    item.material = override_material ? *override_material : part.material;
                    item.skin = skin;
                    renderables.push_back(item);
                }
            } else {
                render::RenderableRef item;
                item.node = *handle;
                const bool sphere = has_component(scene, guid, gameplay::kRigidBodyId) &&
                                    read<std::int32_t>(scene, guid, gameplay::kRigidBodyId, "shape", 0) == 1;
                item.mesh = sphere ? impl_->sphere_mesh : impl_->box_mesh;
                item.color = tint;
                item.material = override_material ? *override_material : 0u;
                renderables.push_back(item);
            }
        }
    }

    // The particle pool: a live particle is a small box tinted by its emitter.
    for (const Pool& pool : impl_->pools) {
        const auto color = read<std::array<float, 4>>(scene, pool.emitter, gameplay::kParticleEmitterId, "color", {1.0f, 0.8f, 0.4f, 1.0f});
        for (std::size_t i = 0; i < pool.particles.size(); ++i) {
            if (!pool.particles[i].alive) {
                continue;
            }
            const auto handle = scene.handle(pool.entities[i]);
            if (!handle) {
                continue;
            }
            render::RenderableRef item;
            item.node = *handle;
            item.mesh = impl_->box_mesh;
            item.color = glm::vec4(color[0], color[1], color[2], 1.0f);
            renderables.push_back(item);
            ++stats.particles;
        }
    }

    if (view.camera) {
        camera = *view.camera;
        camera.aspect = static_cast<double>(impl_->width) / static_cast<double>(impl_->height);
        stats.camera = true;
    }
    if (lights.empty() && view.default_light) {
        // A scene with no light is drawn with one from above and behind the viewer, so an object is not black.
        render::Light sun;
        sun.id = 1;
        sun.type = render::LightType::Directional;
        sun.direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));
        sun.intensity = 1.1f;
        lights.push_back(sun);
        stats.lights = 1;
    }
    if (!camera.is_valid()) {
        return core::failure(std::string("the primary camera is not valid (check its field of view and clip planes)"));
    }
    const render::FrameSnapshot snapshot =
        render::extract_frame(scene.graph(), camera, impl_->renderer->bounds(), renderables, lights);
    if (auto drawn = impl_->renderer->render(snapshot); !drawn.ok()) {
        return core::failure("the frame could not be drawn: " + describe(drawn.error()));
    }
    stats.items = snapshot.stats().drawn;
    for (const auto& entry : impl_->models) {
        stats.models += entry.second.ok ? 1u : 0u;
    }
    for (const auto& entry : impl_->instances) {
        stats.materials += entry.second ? 1u : 0u;
    }
    stats.textures = static_cast<std::uint32_t>(impl_->textures.size());
    return core::success(stats);
}

void GameRenderer::set_overlay(const ui::Surface& surface) {
    impl_->overlay = surface.pixels();
    impl_->overlay_width = surface.width();
    impl_->overlay_height = surface.height();
}

core::Result<core::Unit, std::string> GameRenderer::present(rhi::TextureHandle back_buffer, std::uint32_t buffer_width,
                                                           std::uint32_t buffer_height, const PresentOptions& options) {
    const render::SceneImage& image = impl_->renderer->image();
    const std::uint64_t pitch = rhi::copy_row_pitch(buffer_width, rhi::Format::B8G8R8A8Unorm);
    const std::uint64_t bytes = pitch * (buffer_height - 1) + static_cast<std::uint64_t>(buffer_width) * 4;
    if (!impl_->staging.valid() || impl_->staging_bytes != bytes) {
        if (impl_->staging.valid()) {
            static_cast<void>(impl_->device->destroy(impl_->staging));
        }
        auto staging = impl_->device->create_buffer(
            rhi::BufferDesc{bytes, rhi::BufferUsage::CopySource | rhi::BufferUsage::CpuWrite, "game present staging"});
        if (!staging.ok()) {
            return core::failure("cannot create the present buffer: " + describe(staging.error()));
        }
        impl_->staging = std::move(staging).value();
        impl_->staging_bytes = bytes;
    }

    // The image is RGBA8 and the back buffer BGRA8, so the red and blue channels swap on the way. Pixels outside the image stay black.
    std::vector<std::uint8_t> packed(bytes, 0);
    if (options.fill) {
        for (std::uint32_t y = 0; y < buffer_height; ++y) {
            std::uint8_t* row = packed.data() + y * pitch;
            for (std::uint32_t x = 0; x < buffer_width; ++x) {
                row[x * 4 + 0] = (*options.fill)[2];
                row[x * 4 + 1] = (*options.fill)[1];
                row[x * 4 + 2] = (*options.fill)[0];
                row[x * 4 + 3] = 255;
            }
        }
    }
    const std::uint32_t copy_w = options.x >= buffer_width ? 0 : std::min(image.width, buffer_width - options.x);
    const std::uint32_t copy_h = options.y >= buffer_height ? 0 : std::min(image.height, buffer_height - options.y);
    for (std::uint32_t y = 0; y < copy_h; ++y) {
        std::uint8_t* row = packed.data() + (y + options.y) * pitch + static_cast<std::size_t>(options.x) * 4;
        for (std::uint32_t x = 0; x < copy_w; ++x) {
            const std::size_t index = image.index(x, y);
            const auto& texel = image.color[index];
            // Nothing was drawn here (no pick id): the environment's sky shows instead of the cleared black.
            const bool sky = image.pick_ids[index] == 0;
            row[x * 4 + 0] = sky ? impl_->sky[2] : texel[2];
            row[x * 4 + 1] = sky ? impl_->sky[1] : texel[1];
            row[x * 4 + 2] = sky ? impl_->sky[0] : texel[0];
            row[x * 4 + 3] = 255;
        }
    }
    // The UI over the picture: source-over blend of the overlay's straight-alpha pixels.
    if (options.overlay && !impl_->overlay.empty()) {
        const std::uint32_t overlay_w = std::min(impl_->overlay_width, buffer_width);
        const std::uint32_t overlay_h = std::min(impl_->overlay_height, buffer_height);
        for (std::uint32_t y = 0; y < overlay_h; ++y) {
            std::uint8_t* row = packed.data() + y * pitch;
            const std::uint8_t* source = impl_->overlay.data() + static_cast<std::size_t>(y) * impl_->overlay_width * 4;
            for (std::uint32_t x = 0; x < overlay_w; ++x) {
                const unsigned alpha = source[x * 4 + 3];
                if (alpha == 0) {
                    continue;
                }
                const unsigned inverse = 255 - alpha;
                row[x * 4 + 0] = static_cast<std::uint8_t>((source[x * 4 + 2] * alpha + row[x * 4 + 0] * inverse) / 255);
                row[x * 4 + 1] = static_cast<std::uint8_t>((source[x * 4 + 1] * alpha + row[x * 4 + 1] * inverse) / 255);
                row[x * 4 + 2] = static_cast<std::uint8_t>((source[x * 4 + 0] * alpha + row[x * 4 + 2] * inverse) / 255);
            }
        }
    }
    if (auto uploaded = impl_->device->upload_buffer(impl_->staging, 0, packed.size(), packed.data()); !uploaded.ok()) {
        return core::failure("cannot fill the present buffer: " + describe(uploaded.error()));
    }

    auto list_result = impl_->device->create_command_list(rhi::QueueType::Graphics);
    if (!list_result.ok()) {
        return core::failure("cannot record the present: " + describe(list_result.error()));
    }
    std::unique_ptr<rhi::CommandList> list = std::move(list_result).value();
    rhi::RhiStatus recorded = list->transition(back_buffer, rhi::ResourceState::Present, rhi::ResourceState::CopyDestination);
    if (recorded.ok()) {
        recorded = list->copy_buffer_to_texture(impl_->staging, 0, back_buffer);
    }
    if (recorded.ok()) {
        recorded = list->transition(back_buffer, rhi::ResourceState::CopyDestination, rhi::ResourceState::Present);
    }
    if (recorded.ok()) {
        recorded = list->close();
    }
    if (recorded.ok()) {
        const std::vector<rhi::CommandList*> lists{list.get()};
        recorded = impl_->device->submit(rhi::QueueType::Graphics, lists, impl_->fence, ++impl_->fence_value);
    }
    if (recorded.ok()) {
        recorded = impl_->device->wait_fence(impl_->fence, impl_->fence_value);
    }
    if (!recorded.ok()) {
        return core::failure("the present copy failed: " + describe(recorded.error()));
    }
    return core::success();
}

}  // namespace engine::runtime
