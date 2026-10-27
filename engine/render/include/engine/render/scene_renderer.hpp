#pragma once

// Scene renderer (US-033; physically based materials, transparency, and image-based lighting in US-034). It draws a FrameSnapshot on one
// device. Each frame is one render graph (US-032) with an upload pass per draw that has items, one scene pass, and one pass that reads the
// targets back. The scene pass has two render passes. The first draws opaque and alpha-mask items into color, entity-ID, and depth-value
// targets with a depth buffer. The second draws alpha-blended items over the color target, with depth testing and no depth writes. The
// same code runs on DX12 and Vulkan; only the view-projection matrix differs, because Vulkan's NDC y points down
// (docs/runtime-conventions.md).
//
// Every draw starts at instance zero, because SV_InstanceID does not add a non-zero first_instance on DX12 while Vulkan's
// gl_InstanceIndex does. Each draw therefore has its own frame buffer (a "slot"). Opaque draws batch the items of one mesh and material,
// and each alpha-blended item gets a slot to itself, so that the blended items draw in the order the ordering policy in
// docs/materials.md gives.
//
// Entity IDs are pick ids from the snapshot, so picking goes through FrameSnapshot::entity_of. The depth value is written to its own
// R32Float target, because a depth buffer cannot be copied to the host. Alpha-blended items write color only: they do not change the
// entity-ID, depth-value, or velocity targets, so picking sees the opaque surface behind a transparent one.
//
// Skinning and motion (US-036). A skinned mesh (one with joints and weights) uses its own vertex layout and five pipelines: the same
// opaque, two-sided, blended, two-sided blended, and shadow variants as a static mesh, with VSSkinned and VSSkinnedShadow. An item's palettes
// travel in the frame buffer after the records, so each draw binds one buffer. Opaque pixels also write a motion vector into a
// two-channel target, from this frame's and the previous frame's clip position of the same point (the previous one is in the camera slot and
// the item record). A skinned item with no pose draws the bind pose (an identity palette).
//
// Lighting and shadows (US-035). Each frame the renderer sorts the snapshot's lights by priority, assigns them to clusters, and uploads
// the lights and the cluster lists in one buffer (set 3). Directional lights with shadows draw cascades into the cascade atlas, and spot
// lights draw one tile each into the local atlas. A tile is re-rendered only when its key changes (lighting.hpp, shadow_atlas.hpp).
// Shadow-only casters (outside the view, but able to shadow it) are drawn into shadow tiles and never into the scene.
//
// GPU indirect draws (US-037). With DrawPath::Indirect, the opaque and alpha-mask items of up to max_draw_groups (mesh, material) groups are
// culled on the GPU. CSCull appends each visible item to its group's DrawIndexed arguments and bumps the group's count, and each group is one
// draw_indexed_indirect_count (DX12 ExecuteIndirect with a count; Vulkan vkCmdDrawIndexedIndirectCount). Each record's first_instance is the
// item's index in the indirect frame buffer, which reaches the shader through a per-instance attribute. Alpha-blended items, shadow
// tiles, and groups past the limit stay on the indexed path (one draw per group with the CPU's visible items). Indirect is used only when the
// shaders, the IndirectCount feature, and the group limit allow it; otherwise the frame is drawn indexed, and indirect_stats() says so.
//
// Meshlets (US-038). A static mesh with attached meshlets (attach_meshlets) draws its opaque and mask items through mesh shaders when the
// device has the MeshShaders feature and the shaders were given. Each (mesh, material) group is one task dispatch: the task stage culls each
// (item, meshlet) pair on the GPU, and the mesh stage writes the survivors with the same vertex transform, pixel shader, and pipeline state as
// the indexed path. Skinned meshes, alpha-blended items, and shadow tiles stay on the other paths. The meshlets come from the same cooked LOD
// 0 as the indexed draws, so set_geometry_path switches between the two without uploading again. When the feature is off, every frame draws
// the conventional way, and geometry_path() reports it.

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/meshlet.hpp"
#include "engine/render/pbr.hpp"
#include "engine/render/post_processor.hpp"
#include "engine/render/postprocess.hpp"
#include "engine/render/render_graph.hpp"
#include "engine/render/denoiser.hpp"
#include "engine/render/dynamic_gi.hpp"
#include "engine/render/rt_effects.hpp"
#include "engine/render/volumetric_fog.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/shadow_atlas.hpp"

#include "engine/gi/probes.hpp"
#include "engine/gi/screen_reflections.hpp"

#include "engine/rhi/device.hpp"
#include "engine/rhi/error.hpp"
#include "engine/rhi/types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace engine::render {

// Byte layout of the frame buffer that shaders/scene.hlsl reads: the camera, then one record per item, then the palettes of the skinned
// items. A record holds the model matrix (64 bytes), the inverse transpose of its 3x3 (three vec4 columns, 48 bytes), the tint (16 bytes),
// the pick id (4 bytes), the bone count and the palette's byte offset (4 bytes each), and the previous model (64 bytes, at 144).
// The camera holds the view-projection (0), the shadow tile's light clip (64, shadow draws only), the previous view-projection (128), and
// the velocity scale (192). A palette is the current pose's matrices, then the previous pose's.
inline constexpr std::uint64_t kFrameCameraOffset = 0;
inline constexpr std::uint64_t kFramePreviousViewProjection = 128;
inline constexpr std::uint64_t kFrameVelocityScale = 192;
// The TAA jitter (NDC offset, two floats) and the linear-output flag (a uint, 1 when the post pass follows; US-039).
inline constexpr std::uint64_t kFrameJitter = 224;
inline constexpr std::uint64_t kFrameLinearOutput = 232;
// The camera's world position (three floats, meters). Fragments are camera-relative, so the probe grid adds it back (US-040).
inline constexpr std::uint64_t kFrameCameraPosition = 240;
inline constexpr std::uint64_t kFrameItemsOffset = 256;
inline constexpr std::uint64_t kFrameItemStride = 208;
inline constexpr std::uint64_t kFrameItemBones = 132;
inline constexpr std::uint64_t kFrameItemPalette = 136;
inline constexpr std::uint64_t kFrameItemPreviousModel = 144;
// The meshlet header of a mesh draw (US-038), in the camera area after the velocity scale: the group's first meshlet record, its meshlet count,
// its item count, the first visibility counter of its task groups, and its task group count. Only meshlet draws write it.
inline constexpr std::uint64_t kFrameMeshletBase = 200;
inline constexpr std::uint64_t kFrameMeshletCount = 204;
inline constexpr std::uint64_t kFrameItemCount = 208;
inline constexpr std::uint64_t kFrameCountsBase = 212;
inline constexpr std::uint64_t kFrameTaskGroups = 216;
inline constexpr std::uint64_t kFrameCullCones = 220;  // 1 for a one-sided group: its meshlets whose cones face away are culled

// Material 0 is the default material. A renderable with no material uses it.
inline constexpr std::uint32_t kDefaultMaterial = 0;

// Sizes of the two shadow atlases and their tiles. The cascade atlas holds the cascades of directional lights; the local atlas holds one
// tile per spot light. A tile is at most the size of its atlas. The defaults are the native profile's budget (docs/lighting.md).
struct ShadowMapSettings {
    std::uint32_t cascade_atlas_size = 4096;
    std::uint32_t cascade_size = 2048;
    std::uint32_t local_atlas_size = 4096;
    std::uint32_t local_size = 2048;
};

// Which path draws the opaque and alpha-mask items (US-037). Indexed draws each group with the CPU's visible items; Indirect culls on the GPU
// and draws each group with one indirect count draw. Indirect falls back to Indexed when it is not available (see the header comment).
enum class DrawPath : std::uint8_t {
    Indexed,
    Indirect,
};

// Which path draws the static opaque and alpha-mask items of a mesh that has meshlets (US-038). Conventional uses the indirect or indexed path
// that DrawPath selects; Meshlets uses mesh shaders when the device has them. Meshlets falls back to Conventional when the feature is
// unavailable, so geometry_path() tells what a frame used.
enum class GeometryPath : std::uint8_t {
    Conventional,
    Meshlets,
};

// Capacity of the meshlet pools, shared by every attached mesh (US-038). attach_meshlets fails with OutOfRange when a pool has no room left.
// task_groups bounds the visibility counters of one frame: one per task group of every meshlet draw.
struct MeshletPoolSettings {
    std::uint32_t meshlets = 1u << 15;         // meshlet records (48 bytes each)
    std::uint32_t vertices = 1u << 19;         // meshlet vertex indices (4 bytes each)
    std::uint32_t triangle_bytes = 1u << 21;   // meshlet triangle corners (1 byte each, three per triangle)
    std::uint32_t mesh_vertices = 1u << 18;    // mesh vertices copied for the mesh stage (32 bytes each)
    std::uint32_t task_groups = 1u << 15;      // visibility counters (4 bytes each) for the task groups of one frame
};

// What the last successful render() did on the meshlet path. `groups` are the mesh and material groups drawn with mesh shaders, `candidates`
// the (item, meshlet) pairs they started with, and `visible` the pairs the task stage kept. The counts come from the GPU, so they are the
// culling result of the frame that rendered.
struct MeshletStats {
    bool active = false;  // the frame drew at least one group with mesh shaders
    std::uint32_t groups = 0;
    std::uint32_t candidates = 0;
    std::uint32_t visible = 0;
};

// What the last successful render() did on the indirect path. `candidates` are the items the GPU culled, `visible` the ones it kept, and
// `drawn` the ones within each group's capacity. `overflow` counts visible items that did not fit and were not drawn, so visible = drawn +
// overflow. With culling disabled every candidate is visible.
struct IndirectStats {
    bool active = false;  // the frame's groups were drawn indirectly (false: the frame drew indexed, or it had no groups)
    std::uint32_t groups = 0;          // groups drawn with indirect count draws
    std::uint32_t indexed_groups = 0;  // groups drawn with direct draws
    std::uint32_t candidates = 0;      // opaque and mask items in the indirect groups
    std::uint32_t visible = 0;         // candidates the GPU kept
    std::uint32_t drawn = 0;           // visible items within their group's capacity
    std::uint32_t overflow = 0;        // visible items past their group's capacity
};

// What the last successful render() did with the lights and the shadow tiles.
struct LightingStats {
    std::uint32_t lights = 0;                // lights in the frame
    std::uint32_t dropped_lights = 0;        // beyond max_lights, lowest priority first
    std::uint32_t shadow_maps = 0;           // shadow tiles the frame uses
    std::uint32_t shadow_maps_rendered = 0;  // of those, the tiles re-rendered this frame
    std::uint32_t shadow_maps_dropped = 0;   // no room in the atlas, or past max_shadow_maps
    std::uint32_t shadow_draws = 0;          // caster draws into the shadow tiles
    std::uint32_t overflow_clusters = 0;     // clusters that dropped at least one light
    std::uint32_t dropped_entries = 0;       // light-cluster pairs that did not fit
};

// What the scene pass shows. Lit is the shaded image. The others replace it with a debug value per pixel: the number of lights in its
// cluster, whether that cluster dropped a light, the cascade used, or the shadow visibility (1 is lit, 0 is shadowed).
enum class LightingView : std::uint32_t {
    Lit = 0,
    ClusterCount = 1,     // grey, scaled by the count over max_lights_per_cluster
    ClusterOverflow = 2,  // red where the cluster dropped a light
    CascadeIndex = 3,     // red, green, blue, yellow for cascades 0 to 3; black outside every cascade
    ShadowFactor = 4,     // grey: the product of the visibilities of the shadowed lights
    ProbeCoverage = 5,    // red where no baked probe covers the point (the environment alone), green where all eight corners are valid
};

// Ray-traced shadows and single-bounce reflections (US-043). The pass traces the camera's primary rays in a compute pass before the scene
// pass, and the scene's lighting reads its shadow and reflection targets (scene.hlsl). Its surface set is the static, non-blended items:
// skinned meshes and alpha-blended items are not traced (rt_effects_geometry_support). The flag is fixed at create, and the device must
// have the RayTracing feature. The other settings can change later through set_ray_traced_settings.
// Dynamic GI (US-044) is the full profile's diffuse bounce: a probe grid traced each frame in the same scene (dynamic_gi.hpp). With it on,
// the scene's diffuse comes from that grid, and the baked grid is not read. It needs enabled, and its flag is fixed at create too.
// Froxel volumetric fog (US-045). The grid is fixed at create; the medium and the history can change later. The fog takes the sun and the
// point lights of the frame (spot lights are not used); only the sun's shadow is traced.
struct FogSettings {
    std::uint32_t width = kFogFullWidth;    // the grid's columns, rows, and slices; fixed at create
    std::uint32_t height = kFogFullHeight;
    std::uint32_t depth = kFogFullDepth;
    glm::vec3 albedo{1.0f};                 // the scattering fraction per channel
    float density = 0.02f;                  // extinction at height zero, per meter
    float height_falloff = 0.05f;           // the density falls off as exp(-height_falloff * y)
    float anisotropy = 0.5f;                // Henyey-Greenstein g
    float max_distance = 200.0f;            // meters; the fog's far edge, and the far end of the froxel slices
    bool shadows = true;                    // trace the sun's shadow in the fog
    float history_weight = 0.9f;            // the temporal blend, see VolumetricFog
    std::uint32_t max_age = 15;
};

// Ray-traced shadows and reflections (US-043), dynamic GI (US-044), and the denoising and volumetrics of US-045. The denoise and fog flags
// are fixed at create, and both need the ray-traced pass. The denoiser filters the shadow and reflection targets before the scene pass reads
// them; the fog is injected before it and applied after it (to the lit color, before the reflection and post passes).
struct RayTracedSettings {
    bool enabled = false;
    float roughness_cutoff = 0.4f;       // surfaces above this roughness keep the environment reflection
    float reflection_distance = 100.0f;  // meters; the reflection ray's length
    float shadow_distance = 1000.0f;     // meters; the shadow ray's length
    bool dynamic_gi = false;             // the dynamic probe grid (US-044), with full_profile_grid()
    glm::vec3 dynamic_sky_radiance{0.3f, 0.35f, 0.4f};  // linear radiance of the uniform sky that a probe ray sees when it misses the scene
    bool denoise = false;                // the temporal denoiser of the shadow and reflection targets (US-045)
    DenoiseSettings denoiser;
    bool volumetric_fog = false;         // the froxel fog (US-045); needs the post pass, which holds the HDR color it adds to
    FogSettings fog;
};

struct SceneRendererDesc {
    std::uint32_t width = 64;  // every target is this size; any size works, the readback rows are padded to the copy pitch
    std::uint32_t height = 64;
    std::uint32_t max_items = 1024;  // one frame holds at most this many items
    LightingSettings lighting;
    CascadeSettings cascades;
    ShadowMapSettings shadows;
    std::uint32_t max_shadow_maps = 16;  // shadow records per frame; cascades and spot lights each take one
    DrawPath draw_path = DrawPath::Indirect;
    std::uint32_t max_draw_groups = 64;     // (mesh, material) groups drawn indirectly in one frame; the rest draw indexed
    std::uint32_t draw_group_capacity = 0;  // records per group in the arguments buffer; zero means max_items
    bool gpu_culling = true;                // false draws every candidate of an indirect group (culling disabled)
    GeometryPath geometry = GeometryPath::Meshlets;  // static meshes with meshlets use mesh shaders when the device has them (US-038)
    MeshletPoolSettings meshlets;
    // HDR and the post pass (US-039). When enabled, the scene color target is linear R16G16B16A16Float, and every render() runs the TAA,
    // bloom, tone-map, and overlay passes (post_processor.hpp). image().color then holds the displayed 8-bit image. Disabled, the scene
    // writes sRGB straight into an 8-bit target, as before US-039. The flag is fixed at create; the other settings can change later.
    PostProcessSettings post;
    // Screen-space reflections (US-040): a compute pass after the opaque pass adds the traced specular, with the environment as the fallback
    // (reflections.hlsl). It needs the post pass, because it works on the HDR color, and the flag is fixed at create.
    bool screen_reflections = false;
    // The march of the reflection pass. max_steps times the step length must cover max_distance, and the step should stay below thickness,
    // or thin surfaces are missed (docs/gi-probes.md).
    gi::ScreenTraceSettings reflections;
    // Ray-traced shadows and reflections (US-043). Needs SceneShaders::ray_traced when enabled.
    RayTracedSettings ray_traced;
};

// The compiled entries of engine/render/shaders/scene.hlsl for the device's backend.
struct SceneShaders {
    rhi::ShaderBinary vertex;       // VSMain
    rhi::ShaderBinary pixel;        // PSMain: opaque and alpha-mask fragments, into color, entity ID, depth value, and motion vector
    rhi::ShaderBinary pixel_blend;  // PSBlend: alpha-blended fragments, into color
    rhi::ShaderBinary shadow_vertex;  // VSShadow: a caster into a shadow tile
    rhi::ShaderBinary shadow_pixel;   // PSShadow: the depth of a caster, into the tile
    // Optional (US-036): VSSkinned and VSSkinnedShadow. Without them the renderer rejects skinned meshes, and everything else still works.
    rhi::ShaderBinary skinned_vertex;
    rhi::ShaderBinary skinned_shadow_vertex;
    // Optional (US-037): VSIndirect, VSSkinnedIndirect, and the compute entry CSCull. Without the first two and CSCull, every frame draws
    // indexed. Without VSSkinnedIndirect, skinned groups draw indexed and static groups do not change.
    rhi::ShaderBinary indirect_vertex;
    rhi::ShaderBinary skinned_indirect_vertex;
    rhi::ShaderBinary cull;
    // Optional (US-047): VSTerrain and VSTerrainShadow of scene.hlsl. They read the terrain stride, with the four splat weights at location 3,
    // and draw through the same pixel and shadow pixel entries. Without both, upload_terrain_mesh fails and nothing else changes.
    rhi::ShaderBinary terrain_vertex;
    rhi::ShaderBinary terrain_shadow_vertex;
    // Optional (US-038): ASMeshlet (amplification, the culling task stage) and MSMeshlet (mesh) of scene.hlsl. The pixel shader is `pixel`.
    // Without both, meshlets are unavailable and every mesh draws the conventional way.
    rhi::ShaderBinary meshlet_task;
    rhi::ShaderBinary meshlet_mesh;
    // Optional (US-039): the four compute entries of postprocess.hlsl. Required when SceneRendererDesc::post is enabled.
    PostShaders post;
    // Optional (US-040): CSReflections of reflections.hlsl. Required when SceneRendererDesc::screen_reflections is enabled.
    rhi::ShaderBinary reflections;
    // Optional (US-043): CSRayTracedEffects of rt_effects.hlsl (SM 6.5, RAY_QUERY). Required when SceneRendererDesc::ray_traced is enabled.
    rhi::ShaderBinary ray_traced;
    // Optional (US-044): CSDynamicGi of dynamic_gi.hlsl (SM 6.5, RAY_QUERY). Required when SceneRendererDesc::ray_traced.dynamic_gi is set.
    rhi::ShaderBinary dynamic_gi;
    // Optional (US-045): CSTemporalDenoise of denoise.hlsl. Required when SceneRendererDesc::ray_traced.denoise is set.
    rhi::ShaderBinary denoise;
    // Optional (US-045): CSFogInject, CSFogIntegrate, and CSFogApply of volumetric_fog.hlsl (SM 6.5, RAY_QUERY). Required when
    // SceneRendererDesc::ray_traced.volumetric_fog is set.
    rhi::ShaderBinary fog_inject;
    rhi::ShaderBinary fog_integrate;
    rhi::ShaderBinary fog_apply;
};

class SceneRenderer {
public:
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<SceneRenderer>> create(rhi::Device& device, const SceneRendererDesc& desc,
                                                                               SceneShaders shaders);

    // Uploads the mesh to GPU memory. Returns its index, which RenderableRef::mesh names. Its bounds are kept for culling. Normals and UVs
    // are derived when the mesh has none (vertex_normals).
    [[nodiscard]] rhi::RhiResult<std::uint32_t> upload_mesh(const MeshData& mesh);

    // Uploads a terrain tile (US-047): the mesh as upload_mesh does, with one normalised four-layer splat weight per vertex. The tile draws
    // through the terrain pipelines, and its splat tints the albedo. Fails with InvalidState without SceneShaders::terrain_vertex and
    // terrain_shadow_vertex, with InvalidDescriptor when the splat count is not the vertex count, or when the mesh is skinned.
    [[nodiscard]] rhi::RhiResult<std::uint32_t> upload_terrain_mesh(const MeshData& mesh, const std::vector<glm::vec4>& splat);

    // Attaches the cooked meshlets of an uploaded static mesh (US-038). The meshlets must be the mesh's triangles (validate_meshlets). Fails
    // with Unsupported when the device or the shaders have no mesh shaders, and with OutOfRange when a meshlet pool is full. The mesh's
    // indexed data stays in place, so the conventional path draws the same geometry.
    [[nodiscard]] rhi::RhiStatus attach_meshlets(std::uint32_t mesh, const MeshletData& meshlets);
    [[nodiscard]] bool has_meshlets(std::uint32_t mesh) const {
        return mesh < gpu_meshes_.size() && gpu_meshes_[mesh].meshlet_count != 0;
    }
    // Selects the path of static meshes for the next render(). It does not upload anything.
    void set_geometry_path(GeometryPath path) noexcept {
        desc_.geometry = path;
    }
    // The path static meshes take now: Meshlets only when it was requested and the device has mesh shaders with the shaders given.
    [[nodiscard]] GeometryPath geometry_path() const noexcept {
        return desc_.geometry == GeometryPath::Meshlets && meshlets_ready_ ? GeometryPath::Meshlets : GeometryPath::Conventional;
    }

    // Uploads an 8-bit RGBA image. Returns its texture id, which MaterialTextures names. The shader decodes sRGB for the slots that hold it.
    [[nodiscard]] rhi::RhiResult<std::uint32_t> upload_texture(const ImageRgba8& image);

    // Creates a material from its parameters and texture slots. Returns its id, which RenderableRef::material names.
    [[nodiscard]] rhi::RhiResult<std::uint32_t> create_material(const MaterialParams& params, const MaterialTextures& textures = {});

    // Creates a material instance: the parent's parameters and textures, with the overrides applied. Returns its id.
    [[nodiscard]] rhi::RhiResult<std::uint32_t> create_material_instance(std::uint32_t parent, const MaterialOverrides& overrides);

    // Builds the image-based lighting from a radiance image and uploads it. Later frames use it. Each renderer starts with a uniform
    // environment of radiance one.
    [[nodiscard]] rhi::RhiStatus set_environment(const EquirectImage& radiance);

    // Sets the baked diffuse probes (US-040) that light the scene. Each later frame interpolates them where they cover a point, and the
    // environment's diffuse fills the rest. The volume must have the format this build reads and a probe count that matches its grid. The
    // renderer does not check the volume against its sources: a caller that changes a source bakes again and calls this with the new volume
    // (gi::is_current reports a stale one). Until a volume is set, the scene is lit by the environment alone.
    [[nodiscard]] rhi::RhiStatus set_gi_volume(const gi::GiVolume& volume);
    // Removes the probes: the scene is lit by the environment alone again.
    [[nodiscard]] rhi::RhiStatus clear_gi_volume();
    [[nodiscard]] bool has_gi_volume() const noexcept {
        return gi_active_;
    }

    // Replaces a mesh's culling bounds (for example with animated bounds, animated_bounds in engine_animation). The radius is object space.
    [[nodiscard]] rhi::RhiStatus set_mesh_bounds(std::uint32_t mesh, const MeshBounds& bounds);

    // Bounds and CPU copies of every uploaded mesh, in upload order. Extraction and the reference rasterizer read them.
    [[nodiscard]] const std::vector<MeshBounds>& bounds() const noexcept {
        return bounds_;
    }
    [[nodiscard]] const std::vector<MeshData>& meshes() const noexcept {
        return meshes_;
    }
    [[nodiscard]] std::uint32_t material_count() const noexcept {
        return static_cast<std::uint32_t>(materials_.size());
    }
    [[nodiscard]] const MaterialParams& material_params(std::uint32_t material) const {
        return materials_[material].params;
    }
    // The environment the GPU reads, for the CPU reference.
    [[nodiscard]] const Environment& environment() const noexcept {
        return environment_;
    }

    // Draws the snapshot and reads the targets back. Fails when the snapshot holds more items than max_items, or names a mesh or a
    // material that does not exist, or when a skinned item cannot be drawn (no skinned shaders, or a palette too short for its mesh).
    [[nodiscard]] rhi::RhiStatus render(const FrameSnapshot& snapshot);

    // The targets of the last successful render().
    [[nodiscard]] const SceneImage& image() const noexcept {
        return image_;
    }
    // Whether render() copies the frame back to the CPU for image(). On by default. The benchmark turns it off for the timed frames: a game
    // never reads the frame back, and the copy (four full-size readbacks and a per-pixel loop) is a CPU cost of every frame. The counter
    // readbacks (indirect and meshlet counts) still run. With it off, image() keeps the last frame it copied.
    void set_readback(bool enabled) noexcept {
        readback_enabled_ = enabled;
    }

    // Selects what the scene pass shows. It takes effect on the next render().
    void set_lighting_view(LightingView view) noexcept {
        view_ = view;
    }
    [[nodiscard]] LightingView lighting_view() const noexcept {
        return view_;
    }
    [[nodiscard]] const LightingStats& lighting_stats() const noexcept {
        return lighting_stats_;
    }
    [[nodiscard]] const IndirectStats& indirect_stats() const noexcept {
        return indirect_stats_;
    }
    [[nodiscard]] const MeshletStats& meshlet_stats() const noexcept {
        return meshlet_stats_;
    }
    // The passes of the last render(), in the order the graph ran them, each with its CPU time and, when the device has timestamp queries,
    // its GPU time. The denoiser and the fog are separate entries, so each is measured on its own (US-045).
    [[nodiscard]] const std::vector<PassTiming>& frame_passes() const noexcept {
        return frame_passes_;
    }
    // Marks the shadow tiles of one light stale, so the next render() draws them again. Every light's tiles are also re-drawn when its
    // parameters or its casters change; this is for content the renderer cannot see.
    void invalidate_shadows(std::uint32_t light_id);
    void invalidate_all_shadows();

    // The post pass (US-039). Each call fails with InvalidState unless the renderer was created with SceneRendererDesc::post enabled.
    [[nodiscard]] bool post_enabled() const noexcept {
        return post_ != nullptr;
    }
    // The ray-traced pass (US-043) exists: its settings enabled it, and the device has the RayTracing feature.
    [[nodiscard]] bool ray_traced_active() const noexcept {
        return ray_traced_;
    }
    // Instances the last render() traced. Zero when the pass is off or the frame had no static items.
    [[nodiscard]] std::uint32_t ray_traced_instances() const noexcept {
        return ray_traced_instances_;
    }
    // Changes the roughness cutoff and the ray lengths. `enabled` must match the renderer's state (it is fixed at create). The next
    // render() uses the new values.
    [[nodiscard]] rhi::RhiStatus set_ray_traced_settings(const RayTracedSettings& settings);
    // Replaces the exposure, bloom, and TAA settings. The next render() uses them.
    [[nodiscard]] rhi::RhiStatus set_post_settings(const PostProcessSettings& settings);
    // The reference overlay, composited last in display space. It must be the target size. It takes effect on the next render().
    [[nodiscard]] rhi::RhiStatus set_overlay(const ImageRgba8& overlay);
    // Marks the next frame as a camera cut, so the TAA history is discarded. A teleport is detected from the camera itself.
    void notify_camera_cut() noexcept {
        camera_cut_ = true;
    }
    // Why the last post pass discarded its history (None when it kept it, or when there is no post pass).
    [[nodiscard]] HistoryReset last_history_reset() const noexcept {
        return post_ ? post_->last_reset() : HistoryReset::None;
    }
    // Changes the target size: every target and readback is rebuilt, and the next frame starts without history (no motion, and the TAA
    // history is reset). The image of the last render() keeps its old size until the next one.
    [[nodiscard]] rhi::RhiStatus resize(std::uint32_t width, std::uint32_t height);

    // Destroys every device object this renderer created. Call it before the device is destroyed; the renderer is not usable afterward.
    [[nodiscard]] rhi::RhiStatus release();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

private:
    // Texture ids of the two defaults that every renderer creates first, in this order.
    static constexpr std::uint32_t kWhiteTexture = 0;
    static constexpr std::uint32_t kFlatNormalTexture = 1;
    // Pipelines: [0] opaque, [1] opaque two-sided, [2] blended, [3] blended two-sided, [4] shadow tile. The skinned variants of the same five
    // follow at kSkinnedPipelineBase (US-036). The four indirect opaque pipelines follow them at kIndirectPipelineBase (US-037): static
    // one-sided, static two-sided, skinned one-sided, and skinned two-sided. The two meshlet opaque pipelines follow them at
    // kMeshletPipelineBase (US-038): one-sided, then two-sided. The terrain opaque tile and its shadow tile (US-047) are last, at
    // kTerrainOpaquePipeline and kTerrainShadowPipeline.
    static constexpr std::size_t kPipelineCount = 18;
    static constexpr std::size_t kMeshletPipelineBase = 14;
    static constexpr std::size_t kShadowPipeline = 4;
    static constexpr std::size_t kSkinnedPipelineBase = 5;
    static constexpr std::size_t kSkinnedShadowPipeline = kSkinnedPipelineBase + kShadowPipeline;
    static constexpr std::size_t kIndirectPipelineBase = 10;
    // The terrain opaque tile and its shadow tile (US-047), at the end of the table.
    static constexpr std::size_t kTerrainOpaquePipeline = 16;
    static constexpr std::size_t kTerrainShadowPipeline = 17;
    // The location of the per-instance input (the item index): the one after a layout's vertex attributes, which is how DXC numbers the
    // inputs of VSIndirect (3) and VSSkinnedIndirect (5) in scene.hlsl.
    static constexpr std::uint32_t kStaticInstanceLocation = 3;
    static constexpr std::uint32_t kSkinnedInstanceLocation = 5;
    // Material slots of the material set, in binding order: base color, metallic-roughness, normal, emissive, occlusion.
    static constexpr std::size_t kMaterialTextureSlots = 5;
    // Environment specular levels; the BRDF table follows them in the environment set.
    static constexpr std::size_t kEnvironmentTextures = kEnvironmentLevels + 1;

    struct GpuMesh {
        rhi::BufferHandle vertices;
        rhi::BufferHandle indices;
        std::uint32_t index_count = 0;
        std::uint32_t vertex_stride = 0;
        std::uint32_t bones = 0;  // joints the palette must hold; zero for a static mesh
        // The mesh's meshlets in the pools (US-038): the first record and how many. Zero count means the mesh has none.
        std::uint32_t meshlet_base = 0;
        std::uint32_t meshlet_count = 0;
        // Ray tracing (US-043): the vertex count, the triangles in object space (one per index triple), and the bottom-level structures,
        // opaque at [0] and non-opaque at [1] for the alpha-mask materials. A structure is built on first use.
        std::uint32_t vertex_count = 0;
        std::vector<RtTriangleRecord> rt_triangles;
        std::array<rhi::AccelerationStructureHandle, 2> blas{};
        // A terrain tile (US-047): its vertices carry the splat, so it draws through the terrain pipelines and nowhere else.
        bool terrain = false;
    };

    struct GpuTexture {
        rhi::TextureHandle texture;
        rhi::TextureDesc desc;
        std::vector<std::uint8_t> alpha;  // ray tracing (US-043): the alpha of each texel, kept for the mask atlas; empty when RT is off
    };

    // Byte offsets of the lighting buffer's arrays (see scene.hlsl and docs/lighting.md).
    struct LightingLayout {
        std::uint64_t light_base = 0;
        std::uint64_t shadow_base = 0;
        std::uint64_t count_base = 0;
        std::uint64_t index_base = 0;
        std::uint64_t bytes = 0;
    };

    struct GpuMaterial {
        MaterialParams params;
        std::array<std::uint32_t, kMaterialTextureSlots> textures{};  // texture ids in slot order
        rhi::BufferHandle parameters;
        rhi::DescriptorSetHandle set;
    };

    // The frame buffer of one draw: the staging buffer that fills it, and the set that binds it. It grows to the largest size it has held, so
    // it is created only when it must get bigger.
    struct FrameSlot {
        rhi::BufferHandle frame;
        rhi::BufferHandle upload;
        rhi::DescriptorSetHandle set;
        rhi::DescriptorSetHandle meshlet_set;  // the same frame buffer with the meshlet pools (US-038); created when the meshlet path exists
        std::uint64_t capacity = 0;  // bytes
    };

    SceneRenderer(rhi::Device& device, const SceneRendererDesc& desc);
    [[nodiscard]] rhi::RhiStatus build(SceneShaders shaders);
    // The scene's color, velocity, ID, depth-value, and depth targets and their readback buffers. Sized by desc_, so resize() rebuilds them.
    [[nodiscard]] rhi::RhiStatus build_targets();
    // Linear HDR when the post pass follows, the 8-bit sRGB target otherwise.
    [[nodiscard]] rhi::Format scene_color_format() const noexcept {
        return desc_.post.enabled ? rhi::Format::R16G16B16A16Float : rhi::Format::R8G8B8A8Unorm;
    }
    // The usage the post pass needs on the color and velocity targets to read them.
    [[nodiscard]] rhi::TextureUsage post_source_usage() const noexcept {
        return desc_.post.enabled ? rhi::TextureUsage::ShaderResource : rhi::TextureUsage::None;
    }
    [[nodiscard]] rhi::RhiStatus build_indirect(const SceneShaders& shaders);
    [[nodiscard]] rhi::RhiStatus build_terrain(const SceneShaders& shaders);
    [[nodiscard]] rhi::RhiResult<std::uint32_t> upload_geometry(const MeshData& mesh, const std::vector<glm::vec4>* splat);
    [[nodiscard]] rhi::RhiStatus build_lighting(const SceneRendererDesc& desc);
    [[nodiscard]] rhi::RhiStatus build_reflections(const SceneShaders& shaders);
    [[nodiscard]] rhi::RhiStatus write_reflection_set();
    [[nodiscard]] rhi::RhiStatus upload_gi(const std::vector<std::uint8_t>& bytes, bool active);
    [[nodiscard]] rhi::RhiStatus build_atlases();
    [[nodiscard]] rhi::RhiStatus build_meshlets(const SceneShaders& shaders);
    // Ray-traced effects (US-043). build_ray_traced creates the pass (when enabled) and the 1x1 dummy outputs; bind_ray_traced_outputs writes
    // the lighting set's shadow and reflection slots (the dummies in the shadow set, which never reads them). prepare_ray_traced builds the
    // frame's bottom-level structures and mask atlas, writes the top-level structure, and binds the scene and frame to the pass.
    [[nodiscard]] rhi::RhiStatus build_ray_traced(const SceneShaders& shaders);
    [[nodiscard]] rhi::RhiStatus bind_ray_traced_outputs();
    // Dynamic GI (US-044): the environment set's history and sampling slots hold the pass's buffers, or zeroed dummies (sampling disabled).
    [[nodiscard]] rhi::RhiStatus bind_dynamic_gi();
    // Denoising and volumetrics (US-045). build_denoisers creates the two denoisers over the pass's targets; build_fog creates the fog and
    // binds its apply pass to the frame; both run after build_ray_traced and again after a resize. update_denoise_and_fog uploads the frame's
    // settings and sets the resets: a camera cut, a moved sun, or a moved point light discards the history.
    [[nodiscard]] rhi::RhiStatus build_denoisers();
    [[nodiscard]] rhi::RhiStatus build_fog();
    [[nodiscard]] rhi::RhiStatus update_denoise_and_fog(const FrameSnapshot& snapshot, const RtFrame& frame);
    [[nodiscard]] rhi::RhiStatus prepare_ray_traced(const FrameSnapshot& snapshot);
    [[nodiscard]] rhi::RhiStatus build_mask_atlas();
    [[nodiscard]] rhi::RhiResult<rhi::AccelerationStructureHandle> build_bottom_level(std::uint32_t mesh, bool masked);
    // Destroys the pass, its structures, and the atlas. Called by release() and before a resize rebuilds the pass.
    void destroy_ray_traced(rhi::RhiStatus& first);
    [[nodiscard]] rhi::RhiStatus ensure_slot(std::size_t slot, std::uint64_t bytes);
    void destroy_slot(FrameSlot& slot, rhi::RhiStatus& first);
    [[nodiscard]] rhi::RhiResult<rhi::TextureHandle> make_texture(std::uint32_t width, std::uint32_t height, rhi::Format format, const char* name);
    [[nodiscard]] rhi::RhiStatus upload_texture_bytes(rhi::TextureHandle texture, std::uint32_t width, std::uint32_t height, rhi::Format format,
                                                      const std::vector<std::uint8_t>& bytes);
    // Copies `bytes` into the first bytes of a buffer of `size` bytes. `before` is the state the buffer is in now, and `after` the one it ends in.
    [[nodiscard]] rhi::RhiStatus upload_buffer_bytes(rhi::BufferHandle buffer, std::uint64_t size, const std::vector<std::uint8_t>& bytes,
                                                     rhi::BufferUsage usage = rhi::BufferUsage::ShaderResource | rhi::BufferUsage::CopyDestination,
                                                     rhi::ResourceState after = rhi::ResourceState::ShaderRead,
                                                     rhi::ResourceState before = rhi::ResourceState::Common);

    rhi::Device& device_;
    SceneRendererDesc desc_;
    rhi::DescriptorSetLayoutHandle frame_layout_;
    rhi::DescriptorSetLayoutHandle material_layout_;
    rhi::DescriptorSetLayoutHandle environment_layout_;
    rhi::DescriptorSetLayoutHandle lighting_layout_;
    std::array<rhi::PipelineHandle, kPipelineCount> pipelines_{};
    rhi::TextureHandle color_;
    rhi::TextureHandle surface_;      // R16G16B16A16Float: the shading normal and the roughness (US-040)
    rhi::TextureHandle base_;         // R16G16B16A16Float: the Fresnel base and the occlusion (US-040)
    rhi::TextureHandle reflected_;    // the lit color after the reflection pass (US-040); the post pass reads it instead of color_ when enabled
    rhi::TextureHandle ids_;
    rhi::TextureHandle depth_values_;
    rhi::TextureHandle velocity_;
    rhi::TextureHandle depth_;
    rhi::BufferHandle readback_color_;
    rhi::BufferHandle readback_ids_;
    rhi::BufferHandle readback_depth_;
    rhi::BufferHandle readback_velocity_;
    bool skinned_ready_ = false;  // the skinned pipelines exist (SceneShaders carried the skinned entries)
    // The indirect path (US-037): the cull pipeline, its set, and the buffers it reads and writes. Each is built only when the shaders allow
    // it; indirect_ready_ says whether the static and skinned indirect pipelines exist, and skinned_indirect_ready_ the skinned ones.
    bool indirect_ready_ = false;
    bool skinned_indirect_ready_ = false;
    bool terrain_ready_ = false;  // the terrain pipelines exist (SceneShaders carried the terrain entries)
    rhi::DescriptorSetLayoutHandle cull_layout_;
    rhi::PipelineHandle cull_pipeline_;
    rhi::DescriptorSetHandle cull_set_;
    rhi::BufferHandle cull_upload_;   // CPU-written header, group table, and candidate groups, copied to cull_data_ each frame
    rhi::BufferHandle cull_data_;
    rhi::BufferHandle cull_args_;     // DrawIndexed records, max_draw_groups * capacity of them
    rhi::BufferHandle cull_counts_;   // one count per group; zeroed each frame, read by the draws
    rhi::BufferHandle counts_zero_;   // zeros, copied over cull_counts_ each frame
    rhi::BufferHandle counts_readback_;
    rhi::BufferHandle instance_ids_;  // per-instance input: the identity sequence 0..max_items-1
    std::uint32_t draw_group_capacity_ = 0;
    IndirectStats indirect_stats_;
    // The meshlet path (US-038). meshlets_ready_ says the two pipelines and the pools exist. The pools are fixed-size buffers that every
    // attached mesh appends to; their CPU copies are the bytes uploaded on each attach, so the GPU data is the whole list in order.
    bool meshlets_ready_ = false;
    bool meshlet_pools_uploaded_ = false;  // the pools hold data, so they are in ShaderRead and the next upload starts from there
    rhi::DescriptorSetLayoutHandle meshlet_layout_;
    rhi::BufferHandle meshlet_records_;      // MeshletRecords: 48 bytes per meshlet
    rhi::BufferHandle meshlet_vertices_;     // MeshletVertexIndices: pool indices of the mesh vertices, 4 bytes each
    rhi::BufferHandle meshlet_triangles_;    // MeshletTriangleBytes: meshlet-local corners, 1 byte each
    rhi::BufferHandle mesh_vertex_pool_;     // MeshVertexPool: position, normal, and UV, 32 bytes per vertex
    rhi::BufferHandle meshlet_counts_;       // MeshletCounts: one survivor count per task group, written by the task stage
    rhi::BufferHandle meshlet_counts_readback_;
    std::vector<std::uint8_t> pool_records_;   // CPU copies of the pools, in the order the GPU reads them
    std::vector<std::uint8_t> pool_vertices_;
    std::vector<std::uint8_t> pool_triangles_;
    std::vector<std::uint8_t> pool_mesh_vertices_;
    std::uint32_t pool_mesh_vertex_count_ = 0;
    MeshletStats meshlet_stats_;
    MotionHistory history_;       // the last successful frame's camera, for the motion vectors
    bool has_history_ = false;
    std::unique_ptr<PostProcessor> post_;  // the post pass (US-039); null when SceneRendererDesc::post is off
    std::uint64_t frame_index_ = 0;        // frames rendered; the TAA jitter sequence is a function of it
    bool camera_cut_ = false;              // notify_camera_cut() was called since the last frame
    std::vector<GpuMesh> gpu_meshes_;
    std::vector<GpuTexture> textures_;
    std::vector<GpuMaterial> materials_;
    std::vector<FrameSlot> slots_;  // grows to the most draws one frame has made
    rhi::BufferHandle environment_buffer_;
    std::array<rhi::TextureHandle, kEnvironmentTextures> environment_textures_{};
    rhi::DescriptorSetHandle environment_set_;
    Environment environment_;
    rhi::BufferHandle gi_buffer_;           // the probe grid, bound at environment binding 7 (GiData in scene.hlsl)
    bool gi_active_ = false;                // a volume is set; false is the empty grid
    std::uint64_t gi_bytes_ = 0;            // size of gi_buffer_
    rhi::BufferHandle reflection_params_;   // the reflection pass's parameters, uploaded each frame
    bool reflection_params_ready_ = false;  // a graph has read reflection_params_, so it is in ShaderRead
    rhi::DescriptorSetLayoutHandle reflection_layout_;
    rhi::PipelineHandle reflection_pipeline_;
    rhi::DescriptorSetHandle reflection_set_;
    LightingLayout lighting_layout_offsets_;
    rhi::BufferHandle lighting_buffer_;
    rhi::BufferHandle lighting_upload_;
    rhi::DescriptorSetHandle lighting_set_;
    // Shadow tiles bind this set instead: the lighting buffer, and a 1x1 texture in place of the atlases. The atlases are the targets of the
    // shadow pass, so no draw in that pass may reference them in the shader-read state.
    rhi::DescriptorSetHandle shadow_lighting_set_;
    rhi::TextureHandle shadow_dummy_;
    // Ray-traced effects (US-043). ray_traced_ says the pass exists; the lighting set's shadow and reflection slots then hold its outputs,
    // and the 1x1 dummies hold the neutral values (shadow 1, reflection 0) otherwise. The top-level structure is rebuilt when the frame's
    // instances outgrow it. The mask atlas packs the base-color alpha of the alpha-mask materials, one column of textures.
    bool ray_traced_ = false;
    rhi::ShaderBinary ray_traced_shader_;
    std::unique_ptr<RtEffects> rt_effects_;
    // The dynamic probe grid (US-044), when the full profile enables it. The dummies hold the environment set's slots otherwise.
    std::unique_ptr<DynamicGi> dynamic_gi_;
    rhi::BufferHandle dynamic_history_dummy_;
    rhi::BufferHandle dynamic_sampling_dummy_;
    rhi::TextureHandle rt_shadow_dummy_;
    rhi::TextureHandle rt_reflection_dummy_;
    // Denoising and volumetrics (US-045). The denoisers and the fog exist when their flags were set at create; the fog's lit output is
    // fogged_, which the reflection and post passes read in place of color_. The shaders are kept for the rebuild on resize.
    bool denoise_ = false;
    bool fog_ = false;
    rhi::ShaderBinary denoise_shader_;
    rhi::ShaderBinary fog_inject_shader_;
    rhi::ShaderBinary fog_integrate_shader_;
    rhi::ShaderBinary fog_apply_shader_;
    std::unique_ptr<TemporalDenoiser> denoise_shadow_;
    std::unique_ptr<TemporalDenoiser> denoise_reflection_;
    std::unique_ptr<VolumetricFog> volumetric_fog_;
    rhi::TextureHandle fogged_;
    // The previous frame's camera, sun, and point lights, which the resets and the previous view of the ray-traced pass compare with.
    bool frame_history_ = false;
    glm::dvec3 previous_camera_position_{0.0};
    glm::vec3 previous_camera_forward_{0.0f, 0.0f, -1.0f};
    glm::vec3 previous_camera_up_{0.0f, 1.0f, 0.0f};
    std::optional<glm::vec3> previous_sun_direction_;  // toward the sun, unit; empty when the frame had no sun
    std::vector<glm::dvec3> previous_local_lights_;     // world positions of the fog's point lights, in the order they were given
    std::vector<PassTiming> frame_passes_;              // the last render()'s graph passes (frame_passes())
    rhi::AccelerationStructureHandle top_level_;
    std::uint32_t top_level_capacity_ = 0;
    rhi::TextureHandle mask_atlas_;
    bool mask_atlas_dirty_ = true;
    std::vector<glm::vec4> mask_rects_;  // per texture id: its rectangle in mask_atlas_ (offset xy, size zw), zero when it is not in the atlas
    std::uint32_t ray_traced_instances_ = 0;
    ShadowAtlas cascade_atlas_;
    ShadowAtlas local_atlas_;
    rhi::TextureHandle cascade_color_;
    rhi::TextureHandle cascade_depth_;
    rhi::TextureHandle local_color_;
    rhi::TextureHandle local_depth_;
    LightingView view_ = LightingView::Lit;
    LightingStats lighting_stats_;
    std::vector<MeshData> meshes_;
    std::vector<MeshBounds> bounds_;
    SceneImage image_;
    bool readback_enabled_ = true;  // set_readback(); copies the frame to image_ in render()
    bool released_ = false;
};

}  // namespace engine::render
