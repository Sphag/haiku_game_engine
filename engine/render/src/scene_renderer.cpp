#include "engine/render/scene_renderer.hpp"

#include "engine/core/coordinates.hpp"
#include "engine/render/render_graph.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <utility>

namespace engine::render {
namespace {

using rhi::BufferDesc;
using rhi::BufferHandle;
using rhi::BufferUsage;
using rhi::Format;
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiResult;
using rhi::RhiStatus;
using rhi::TextureDesc;
using rhi::TextureUsage;

// Position, normal, and UV, interleaved. The attributes are at locations 0, 1, and 2, which the shader reads as TEXCOORD0 to TEXCOORD2.
constexpr std::uint32_t kVertexStride = 2 * sizeof(glm::vec3) + sizeof(glm::vec2);
constexpr std::uint32_t kNormalOffset = sizeof(glm::vec3);
constexpr std::uint32_t kUvOffset = 2 * sizeof(glm::vec3);
// A skinned vertex adds the packed joint word (location 3) and the four weights (location 4) after the UV (US-036).
constexpr std::uint32_t kJointsOffset = kVertexStride;
constexpr std::uint32_t kWeightsOffset = kJointsOffset + sizeof(std::uint32_t);
constexpr std::uint32_t kSkinnedVertexStride = kWeightsOffset + sizeof(glm::vec4);
// A terrain vertex (US-047) adds the four splat weights (location 3) after the UV, and nothing else. It is drawn only by the terrain pipelines.
constexpr std::uint32_t kTerrainSplatOffset = kVertexStride;
constexpr std::uint32_t kTerrainVertexStride = kTerrainSplatOffset + sizeof(glm::vec4);
constexpr std::uint64_t kMatrixBytes = sizeof(glm::mat4);

// Byte layout of a material's parameters, as shaders/scene.hlsl reads them: the base color (0), emissive and occlusion strength (16),
// metallic, roughness, normal scale, and alpha cutoff (32), then the blend mode and the two-sided flag (48).
constexpr std::uint64_t kMaterialBytes = 64;
// The environment buffer: nine SH coefficients, each a vec4.
constexpr std::uint64_t kEnvironmentBytes = 9 * 16;

// The lighting buffer (US-035), as scene.hlsl reads it. A header of 256 bytes comes first; its words name the offsets of the arrays. Then
// one 64-byte record per light, one 128-byte record per shadow map, one cluster count per cluster, and max_lights_per_cluster indices per
// cluster. Offsets of the arrays are computed in build_lighting() and stored in the header.
constexpr std::uint64_t kLightingHeaderBytes = 256;
constexpr std::uint64_t kLightRecordBytes = 64;
constexpr std::uint64_t kShadowRecordBytes = 128;
constexpr std::uint32_t kNoShadowIndex = 0xffffffffu;
constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
// Meshlet pool layouts (US-038): a record is 48 bytes (scene.hlsl kMeshletStride), a mesh vertex is the static 32-byte vertex, and a task
// group covers kTaskThreads pairs (scene.hlsl kTaskThreads, kTaskGroupsX).
constexpr std::uint64_t kMeshletRecordBytes = 48;
constexpr std::uint64_t kTaskThreads = 32;
constexpr std::uint64_t kTaskGroupsX = 65535;

constexpr std::array<float, 4> kBackground{0.0f, 0.0f, 0.0f, 0.0f};

// The resets of the denoiser and the fog (US-045). A camera that moves more than this in one frame is a cut; a sun that turns more than
// kLightResetDegrees, or a point light that moves more than kLightResetMeters, starts the history again.
constexpr float kCameraCutMeters = 5.0f;
constexpr float kCameraCutDegrees = 30.0f;
constexpr float kLightResetDegrees = 0.5f;
constexpr float kLightResetMeters = 1.0e-3f;

// The direction toward the sun (the last directional light, as prepare_ray_traced reads it), or nothing when the frame has none.
std::optional<glm::vec3> sun_toward(const FrameSnapshot& snapshot) {
    std::optional<glm::vec3> sun;
    for (const Light& light : snapshot.lights()) {
        if (light.type == LightType::Directional) {
            sun = -light.direction;
        }
    }
    return sun;
}

// The world positions of the point lights the fog takes, in the order the frame lists them, up to the fog's limit.
std::vector<glm::dvec3> point_light_positions(const FrameSnapshot& snapshot) {
    std::vector<glm::dvec3> positions;
    for (const Light& light : snapshot.lights()) {
        if (light.type == LightType::Point && positions.size() < kFogMaxLights) {
            positions.push_back(light.position);
        }
    }
    return positions;
}

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

TextureDesc texture_desc(std::uint32_t width, std::uint32_t height, Format format, TextureUsage usage, const char* name) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

// Palette entries an item's record uses: its own pose, or the bind pose when it has none, which is one identity per joint its mesh uses.
// Zero for a static mesh.
std::uint32_t item_bones(const SnapshotItem& item, const std::vector<std::uint32_t>& mesh_bones) {
    return item.bone_count != 0 ? item.bone_count : mesh_bones[item.mesh];
}

// Bytes of a frame buffer for these items: the camera, one record per item, then the palettes (current, then previous) of the skinned items.
std::uint64_t frame_size(const std::vector<const SnapshotItem*>& items, const std::vector<std::uint32_t>& mesh_bones) {
    std::uint64_t bytes = kFrameItemsOffset + kFrameItemStride * items.size();
    for (const SnapshotItem* item : items) {
        bytes += 2 * kMatrixBytes * item_bones(*item, mesh_bones);
    }
    return bytes;
}

// Row pitch and total size of a target's readback. The copy writes each row at a 256-byte aligned pitch.
std::uint64_t readback_size(std::uint32_t width, std::uint32_t height, Format format) {
    return rhi::copy_row_pitch(width, format) * height;
}

void put_floats(std::vector<std::uint8_t>& bytes, std::uint64_t offset, const float* values, std::size_t count) {
    std::memcpy(bytes.data() + offset, values, count * sizeof(float));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void put_f32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, float value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// The per-frame camera values at the head of a frame buffer.
// The meshlet header of a mesh draw (US-038); the layout is kFrameMeshletBase and the offsets after it.
struct MeshletHeader {
    std::uint32_t base = 0;         // first meshlet record of the mesh in the pool
    std::uint32_t count = 0;        // meshlets of the mesh
    std::uint32_t items = 0;        // items of the draw
    std::uint32_t counts_base = 0;  // first visibility counter of the draw's task groups
    std::uint32_t task_groups = 0;  // task groups of the draw
    bool cull_cones = true;         // false for a two-sided group, whose back faces are drawn
};

struct FrameCamera {
    glm::mat4 view_projection{1.0f};           // clip from the frame's camera-relative space, in the backend's convention
    glm::mat4 previous_view_projection{1.0f};  // the previous frame's, for the same space
    glm::vec2 velocity_scale{0.5f, -0.5f};     // UV units per NDC unit, with the backend's y sign
    glm::vec3 camera_motion{0.0f};             // this frame's camera position minus the previous one, meters
    glm::vec3 camera_position{0.0f};           // the camera's world position, meters; the probe grid adds it to camera-relative points (US-040)
    glm::vec2 jitter{0.0f};                    // TAA jitter, NDC; added to the rasterized position of scene draws only (US-039)
    bool linear_output = false;                // the scene pass writes linear HDR color for the post pass (US-039)
    const glm::mat4* light_clip = nullptr;     // shadow draws only: the light's unremapped clip matrix, read by the shadow pixel stage
    const MeshletHeader* meshlet = nullptr;    // meshlet draws only: the header the task and mesh stages read (US-038)
};

// Writes the frame buffer: the camera, one record per item in draw order, then the palettes. The layout is documented in shaders/scene.hlsl.
// Each record's previous model is the item's previous model moved by the camera's motion, so it is relative to the previous camera.
std::vector<std::uint8_t> pack_frame(const FrameSnapshot& snapshot, const std::vector<const SnapshotItem*>& items,
                                     const std::vector<std::uint32_t>& mesh_bones, const FrameCamera& camera) {
    std::vector<std::uint8_t> bytes(frame_size(items, mesh_bones), 0);
    put_floats(bytes, kFrameCameraOffset, &camera.view_projection[0][0], 16);
    if (camera.light_clip != nullptr) {
        put_floats(bytes, 64, &(*camera.light_clip)[0][0], 16);
    }
    put_floats(bytes, kFramePreviousViewProjection, &camera.previous_view_projection[0][0], 16);
    put_floats(bytes, kFrameVelocityScale, &camera.velocity_scale.x, 2);
    put_floats(bytes, kFrameJitter, &camera.jitter.x, 2);
    put_floats(bytes, kFrameCameraPosition, &camera.camera_position.x, 3);
    put_u32(bytes, kFrameLinearOutput, camera.linear_output ? 1u : 0u);
    if (camera.meshlet != nullptr) {
        put_u32(bytes, kFrameMeshletBase, camera.meshlet->base);
        put_u32(bytes, kFrameMeshletCount, camera.meshlet->count);
        put_u32(bytes, kFrameItemCount, camera.meshlet->items);
        put_u32(bytes, kFrameCountsBase, camera.meshlet->counts_base);
        put_u32(bytes, kFrameTaskGroups, camera.meshlet->task_groups);
        put_u32(bytes, kFrameCullCones, camera.meshlet->cull_cones ? 1u : 0u);
    }
    const glm::mat4 motion = glm::translate(glm::mat4(1.0f), camera.camera_motion);
    std::uint64_t palette = kFrameItemsOffset + kFrameItemStride * items.size();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const SnapshotItem& item = *items[i];
        const std::uint64_t record = kFrameItemsOffset + kFrameItemStride * i;
        put_floats(bytes, record, &item.model[0][0], 16);
        // The normal matrix is the inverse transpose of the model's 3x3, which keeps normals perpendicular under non-uniform scale.
        const glm::mat3 normal_matrix = glm::inverseTranspose(glm::mat3(item.model));
        for (int column = 0; column < 3; ++column) {
            const float values[4] = {normal_matrix[column].x, normal_matrix[column].y, normal_matrix[column].z, 0.0f};
            put_floats(bytes, record + 64 + 16 * static_cast<std::uint64_t>(column), values, 4);
        }
        put_floats(bytes, record + 112, &item.color.r, 4);
        std::memcpy(bytes.data() + record + 128, &item.pick_id, sizeof(std::uint32_t));

        const std::uint32_t bones = item_bones(item, mesh_bones);
        put_u32(bytes, record + kFrameItemBones, bones);
        put_u32(bytes, record + kFrameItemPalette, bones == 0 ? 0u : static_cast<std::uint32_t>(palette));
        const glm::mat4 previous = motion * item.previous_model;
        put_floats(bytes, record + kFrameItemPreviousModel, &previous[0][0], 16);
        if (bones == 0) {
            continue;
        }
        const std::uint64_t count = bones * kMatrixBytes;
        if (item.bone_count != 0) {
            std::memcpy(bytes.data() + palette, snapshot.palettes().data() + item.palette, count);
            std::memcpy(bytes.data() + palette + count, snapshot.previous_palettes().data() + item.palette, count);
        } else {
            // The bind pose: every joint's skinning matrix is the identity, in both the current and the previous pose.
            for (std::uint32_t j = 0; j < 2 * bones; ++j) {
                const glm::mat4 identity(1.0f);
                std::memcpy(bytes.data() + palette + j * kMatrixBytes, &identity, kMatrixBytes);
            }
        }
        palette += 2 * count;
    }
    return bytes;
}

std::vector<std::uint8_t> pack_material(const MaterialParams& params) {
    std::vector<std::uint8_t> bytes(kMaterialBytes, 0);
    const float base[4] = {params.base_color.r, params.base_color.g, params.base_color.b, params.base_color.a};
    put_floats(bytes, 0, base, 4);
    const float emissive[4] = {params.emissive.r, params.emissive.g, params.emissive.b, params.occlusion_strength};
    put_floats(bytes, 16, emissive, 4);
    const float factors[4] = {params.metallic, params.roughness, params.normal_scale, params.alpha_cutoff};
    put_floats(bytes, 32, factors, 4);
    const std::uint32_t modes[2] = {static_cast<std::uint32_t>(params.blend), params.two_sided ? 1u : 0u};
    std::memcpy(bytes.data() + 48, modes, sizeof(modes));
    return bytes;
}

std::vector<std::uint8_t> pack_environment(const Environment& environment) {
    std::vector<std::uint8_t> bytes(kEnvironmentBytes, 0);
    for (std::size_t i = 0; i < 9; ++i) {
        const float values[4] = {environment.diffuse_sh[i].r, environment.diffuse_sh[i].g, environment.diffuse_sh[i].b, 0.0f};
        put_floats(bytes, 16 * i, values, 4);
    }
    return bytes;
}

// Interleaves the mesh's positions, normals, and UVs, and for a skinned mesh its joints and weights. Missing normals are derived, and missing
// UVs are zero. The joints are packed one byte each, low byte first, which the shader unpacks.
std::vector<std::uint8_t> pack_vertices(const MeshData& mesh, std::uint32_t stride) {
    const std::vector<glm::vec3> normals = vertex_normals(mesh);
    const bool skinned = !mesh.joints.empty();
    std::vector<std::uint8_t> bytes(mesh.positions.size() * stride, 0);
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        std::uint8_t* vertex = bytes.data() + i * stride;
        std::memcpy(vertex, &mesh.positions[i], sizeof(glm::vec3));
        std::memcpy(vertex + kNormalOffset, &normals[i], sizeof(glm::vec3));
        const glm::vec2 uv = mesh.uvs.empty() ? glm::vec2(0.0f) : mesh.uvs[i];
        std::memcpy(vertex + kUvOffset, &uv, sizeof(glm::vec2));
        if (skinned) {
            const glm::uvec4& joints = mesh.joints[i];
            const std::uint32_t packed = joints.x | (joints.y << 8) | (joints.z << 16) | (joints.w << 24);
            std::memcpy(vertex + kJointsOffset, &packed, sizeof(packed));
            std::memcpy(vertex + kWeightsOffset, &mesh.weights[i], sizeof(glm::vec4));
        }
    }
    return bytes;
}

// A terrain tile's vertices: the static layout, with the splat weight of each vertex after its UV (US-047).
std::vector<std::uint8_t> pack_terrain_vertices(const MeshData& mesh, const std::vector<glm::vec4>& splat) {
    std::vector<std::uint8_t> bytes = pack_vertices(mesh, kTerrainVertexStride);
    for (std::size_t i = 0; i < splat.size(); ++i) {
        std::memcpy(bytes.data() + i * kTerrainVertexStride + kTerrainSplatOffset, &splat[i], sizeof(glm::vec4));
    }
    return bytes;
}

// Rows of an 8-bit image at the copy pitch, which a texture upload needs.
std::vector<std::uint8_t> pack_rgba8_rows(const ImageRgba8& image) {
    const std::uint64_t pitch = rhi::copy_row_pitch(image.width, Format::R8G8B8A8Unorm);
    std::vector<std::uint8_t> bytes(pitch * image.height, 0);
    const std::uint64_t row = static_cast<std::uint64_t>(image.width) * 4;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::memcpy(bytes.data() + y * pitch, image.pixels.data() + y * row, row);
    }
    return bytes;
}

// Rows of half-float RGBA texels at the copy pitch. `texel` returns the values for (x, y).
std::vector<std::uint8_t> pack_half_rows(std::uint32_t width, std::uint32_t height, const std::function<glm::vec4(std::uint32_t, std::uint32_t)>& texel) {
    const std::uint64_t pitch = rhi::copy_row_pitch(width, Format::R16G16B16A16Float);
    std::vector<std::uint8_t> bytes(pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const glm::vec4 value = texel(x, y);
            const std::uint16_t halves[4] = {glm::packHalf1x16(value.r), glm::packHalf1x16(value.g), glm::packHalf1x16(value.b),
                                             glm::packHalf1x16(value.a)};
            std::memcpy(bytes.data() + y * pitch + x * 8, halves, sizeof(halves));
        }
    }
    return bytes;
}

// Adds an access to a pass unless the pass already has it. A resource can be used by many draws, and each access is declared once.
void add_access(std::vector<ResourceAccess>& accesses, ResourceId resource, Access access) {
    for (const ResourceAccess& existing : accesses) {
        if (existing.resource == resource && existing.access == access) {
            return;
        }
    }
    accesses.push_back(ResourceAccess{resource, access});
}

// Destroys a handle and keeps the first failure. The cleanup paths use it, so a failed destroy does not hide the error that started them.
template <typename Handle>
void destroy_into(rhi::Device& device, Handle& handle, RhiStatus& first) {
    if (!handle.valid()) {
        return;
    }
    if (const RhiStatus status = device.destroy(handle); !status.ok() && first.ok()) {
        first = status;
    }
    handle = Handle{};
}

// The environment's texture for index i: the specular levels, then the BRDF table.
TextureDesc environment_texture_desc(const Environment& environment, std::size_t index) {
    if (index < environment.specular.size()) {
        const EquirectImage& level = environment.specular[index];
        return texture_desc(level.width, level.height, Format::R16G16B16A16Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination,
                            "environment specular");
    }
    return texture_desc(environment.lut_size, environment.lut_size, Format::R16G16B16A16Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination,
                        "environment BRDF table");
}

// Returns the texture in slot `index`, or the default when the slot is unset.
std::uint32_t slot_or(const std::optional<std::uint32_t>& value, std::uint32_t fallback) {
    return value ? *value : fallback;
}

// Draw state shared by every pass that reads the scene: the pipeline, the three sets, and the mesh buffers.
// The cull buffer's header (CSCull reads it at these byte offsets, scene.hlsl): six planes, then the candidate count, the group count, the
// capacity, the culling switch, and the byte offsets of the group table and of the candidate groups. Each group is 32 bytes: its index
// count, its radius, its center, and padding. The candidate groups follow, one word each.
constexpr std::uint64_t kCullItemsOffset = 96;
constexpr std::uint64_t kCullGroupsOffset = 100;
constexpr std::uint64_t kCullCapacityOffset = 104;
constexpr std::uint64_t kCullEnabledOffset = 108;
constexpr std::uint64_t kCullGroupBaseOffset = 112;
constexpr std::uint64_t kCullItemBaseOffset = 116;
constexpr std::uint64_t kCullHeaderBytes = 128;
constexpr std::uint64_t kCullGroupBytes = 32;
constexpr std::uint64_t kCullThreads = 64;  // CSCull's [numthreads] along x

// The vertex attributes of a static and of a skinned mesh (the layouts of kVertexStride and kSkinnedVertexStride).
std::vector<rhi::VertexAttribute> static_attributes() {
    return {rhi::VertexAttribute{0, Format::R32G32B32Float, 0}, rhi::VertexAttribute{1, Format::R32G32B32Float, kNormalOffset},
            rhi::VertexAttribute{2, Format::R32G32Float, kUvOffset}};
}

std::vector<rhi::VertexAttribute> skinned_attributes() {
    std::vector<rhi::VertexAttribute> attributes = static_attributes();
    attributes.push_back(rhi::VertexAttribute{3, Format::R32Uint, kJointsOffset});
    attributes.push_back(rhi::VertexAttribute{4, Format::R32G32B32A32Float, kWeightsOffset});
    return attributes;
}

// The static attributes and the splat weights at location 3 (US-047).
std::vector<rhi::VertexAttribute> terrain_attributes() {
    std::vector<rhi::VertexAttribute> attributes = static_attributes();
    attributes.push_back(rhi::VertexAttribute{3, Format::R32G32B32A32Float, kTerrainSplatOffset});
    return attributes;
}

// One indirect group as the cull buffer describes it: the mesh's index count and bounds.
struct CullGroup {
    std::uint32_t index_count = 0;
    MeshBounds bounds;
};

// The cull buffer's size: its header, the group table of max_draw_groups, and one word per candidate.
std::uint64_t cull_buffer_bytes(const SceneRendererDesc& desc) {
    return kCullHeaderBytes + kCullGroupBytes * desc.max_draw_groups + 4ull * desc.max_items;
}

// The counts: one word per group.
std::uint64_t count_buffer_bytes(const SceneRendererDesc& desc) {
    return rhi::kIndirectCountSize * desc.max_draw_groups;
}

// The cull buffer for one frame. `candidate_groups` gives each candidate's group. The planes are the same frustum the CPU culls with, so
// CSCull and extract_frame agree on what is visible. `group_slots` is the group limit, which sets where the candidate groups start.
std::vector<std::uint8_t> pack_cull(const Frustum& frustum, const std::vector<std::uint32_t>& candidate_groups, const std::vector<CullGroup>& groups,
                                    std::uint32_t capacity, bool culling, std::uint64_t group_slots, std::uint64_t bytes) {
    std::vector<std::uint8_t> cull(bytes, 0);
    for (std::size_t plane = 0; plane < frustum.planes.size(); ++plane) {
        put_floats(cull, 16 * plane, &frustum.planes[plane][0], 4);
    }
    put_u32(cull, kCullItemsOffset, static_cast<std::uint32_t>(candidate_groups.size()));
    put_u32(cull, kCullGroupsOffset, static_cast<std::uint32_t>(groups.size()));
    put_u32(cull, kCullCapacityOffset, capacity);
    put_u32(cull, kCullEnabledOffset, culling ? 1u : 0u);
    put_u32(cull, kCullGroupBaseOffset, static_cast<std::uint32_t>(kCullHeaderBytes));
    put_u32(cull, kCullItemBaseOffset, static_cast<std::uint32_t>(kCullHeaderBytes + kCullGroupBytes * group_slots));
    for (std::size_t group = 0; group < groups.size(); ++group) {
        const std::uint64_t base = kCullHeaderBytes + kCullGroupBytes * group;
        put_u32(cull, base, groups[group].index_count);
        put_f32(cull, base + 4, groups[group].bounds.radius);
        const float center[3] = {groups[group].bounds.center.x, groups[group].bounds.center.y, groups[group].bounds.center.z};
        put_floats(cull, base + 8, center, 3);
    }
    const std::uint64_t item_base = kCullHeaderBytes + kCullGroupBytes * group_slots;
    for (std::size_t item = 0; item < candidate_groups.size(); ++item) {
        put_u32(cull, item_base + 4 * item, candidate_groups[item]);
    }
    return cull;
}

struct BoundDraw {
    rhi::DescriptorSetHandle frame_set;
    rhi::DescriptorSetHandle material_set;
    rhi::DescriptorSetHandle environment_set;
    rhi::DescriptorSetHandle lighting_set;
    rhi::PipelineHandle pipeline;
    ResourceId vertices;
    ResourceId indices;
    std::uint32_t index_count = 0;
    std::uint32_t instances = 0;
    // Indirect draws (US-037): the per-instance input, and the records and count the cull pass wrote. `max_draws` is the group's capacity.
    bool indirect = false;
    ResourceId instance_ids;
    ResourceId args;
    ResourceId counts;
    std::uint64_t args_offset = 0;
    std::uint64_t count_offset = 0;
    std::uint32_t max_draws = 0;
    // Meshlet draws (US-038): one task dispatch of task_x by task_y groups. The meshlet pools are read through the frame set, so the draw binds
    // no vertex or index buffer.
    bool meshlet = false;
    std::uint32_t task_x = 0;
    std::uint32_t task_y = 0;
};

rhi::RhiStatus draw_bound(PassContext& ctx, const BoundDraw& draw) {
    rhi::CommandList& commands = ctx.commands();
    if (const RhiStatus status = commands.set_pipeline(draw.pipeline); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = commands.set_descriptor_set(0, draw.frame_set); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = commands.set_descriptor_set(1, draw.material_set); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = commands.set_descriptor_set(2, draw.environment_set); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = commands.set_descriptor_set(3, draw.lighting_set); !status.ok()) {
        return status;
    }
    if (draw.meshlet) {
        return commands.dispatch_mesh(draw.task_x, draw.task_y, 1);
    }
    if (const RhiStatus status = commands.set_vertex_buffer(0, ctx.buffer(draw.vertices)); !status.ok()) {
        return status;
    }
    if (draw.indirect) {
        if (const RhiStatus status = commands.set_vertex_buffer(1, ctx.buffer(draw.instance_ids)); !status.ok()) {
            return status;
        }
    }
    if (const RhiStatus status = commands.set_index_buffer(ctx.buffer(draw.indices), rhi::IndexFormat::Uint32); !status.ok()) {
        return status;
    }
    if (draw.indirect) {
        return commands.draw_indexed_indirect_count(ctx.buffer(draw.args), draw.args_offset, ctx.buffer(draw.counts), draw.count_offset, draw.max_draws);
    }
    return commands.draw_indexed(draw.index_count, draw.instances, 0, 0, 0);
}

// The probe grid buffer (US-040), as scene.hlsl reads it: a 32-byte header (origin, spacing, dims), then 144 bytes per probe, nine float4 SH
// coefficients with w set for a valid probe. It is bound at environment binding 7. The reflection pass's buffer (reflections.hlsl) holds the
// view-projection and its inverse, the UV scale, the size, the planes, and the trace settings.
constexpr std::uint64_t kGiHeaderBytes = 32;
constexpr std::uint64_t kGiProbeBytes = 144;
constexpr std::uint32_t kGiBinding = 7;
// The dynamic probe grid's history and sampling block (US-044), at environment bindings 8 and 9.
constexpr std::uint32_t kDynamicHistoryBinding = 8;
constexpr std::uint32_t kDynamicSamplingBinding = 9;
constexpr std::uint64_t kDynamicProbeBytes = 144;
constexpr std::uint64_t kDynamicSamplingHeaderBytes = 48;
constexpr std::uint64_t kReflectionBytes = 176;

// The color formats of an opaque pipeline, in the order PSMain writes them: scene color, entity ID, depth value, motion vector, and the two
// surface targets of the reflection pass (US-040).
std::vector<Format> opaque_formats(Format color) {
    return {color, Format::R32Uint, Format::R32Float, Format::R32G32Float, Format::R16G16B16A16Float, Format::R16G16B16A16Float};
}

// Packs a probe grid for GiData. A null volume is the empty grid: a header with zero dims, which makes the shader use the environment alone.
std::vector<std::uint8_t> pack_gi_volume(const gi::GiVolume* volume) {
    const std::size_t probes = volume == nullptr ? 0 : volume->probes.size();
    std::vector<std::uint8_t> bytes(kGiHeaderBytes + kGiProbeBytes * probes, 0);
    if (volume == nullptr) {
        return bytes;
    }
    put_floats(bytes, 0, &volume->grid.origin.x, 3);
    put_floats(bytes, 12, &volume->grid.spacing, 1);
    put_u32(bytes, 16, volume->grid.dims.x);
    put_u32(bytes, 20, volume->grid.dims.y);
    put_u32(bytes, 24, volume->grid.dims.z);
    for (std::size_t i = 0; i < probes; ++i) {
        const gi::Probe& probe = volume->probes[i];
        const float valid = probe.state == gi::ProbeState::Valid ? 1.0f : 0.0f;
        for (std::size_t k = 0; k < 9; ++k) {
            const float coefficient[4] = {probe.sh[k].x, probe.sh[k].y, probe.sh[k].z, valid};
            put_floats(bytes, kGiHeaderBytes + kGiProbeBytes * i + 16 * k, coefficient, 4);
        }
    }
    return bytes;
}

// The reflection pass's parameters (reflections.hlsl). The view-projection is the frame's, so the trace uses the same clip space as the scene.
std::vector<std::uint8_t> pack_reflection_params(const glm::mat4& view_projection, glm::vec2 velocity_scale, std::uint32_t width,
                                                 std::uint32_t height, float near_plane, float far_plane, const gi::ScreenTraceSettings& settings) {
    std::vector<std::uint8_t> bytes(kReflectionBytes, 0);
    put_floats(bytes, 0, &view_projection[0][0], 16);
    const glm::mat4 inverse = glm::inverse(view_projection);
    put_floats(bytes, 64, &inverse[0][0], 16);
    put_floats(bytes, 128, &velocity_scale.x, 2);
    put_u32(bytes, 136, width);
    put_u32(bytes, 140, height);
    const float planes[2] = {near_plane, far_plane};
    put_floats(bytes, 144, planes, 2);
    put_u32(bytes, 152, settings.max_steps);
    const float step = settings.max_distance / static_cast<float>(settings.max_steps);
    put_floats(bytes, 156, &step, 1);
    const float trace[3] = {settings.thickness, settings.edge_fade, settings.roughness_cutoff};
    put_floats(bytes, 160, trace, 3);
    return bytes;
}

}  // namespace

SceneRenderer::SceneRenderer(rhi::Device& device, const SceneRendererDesc& desc)
    : device_(device), desc_(desc), frame_layout_{}, material_layout_{}, environment_layout_{}, lighting_layout_{}, color_{}, ids_{},
      depth_values_{}, depth_{}, readback_color_{}, readback_ids_{}, readback_depth_{}, environment_buffer_{}, environment_set_{},
      lighting_buffer_{}, lighting_upload_{}, lighting_set_{}, cascade_atlas_(desc.shadows.cascade_atlas_size),
      local_atlas_(desc.shadows.local_atlas_size), cascade_color_{}, cascade_depth_{}, local_color_{}, local_depth_{} {}

RhiResult<std::unique_ptr<SceneRenderer>> SceneRenderer::create(rhi::Device& device, const SceneRendererDesc& desc, SceneShaders shaders) {
    if (desc.width == 0 || desc.height == 0 || desc.max_items == 0) {
        return rhi::fail<std::unique_ptr<SceneRenderer>>(RhiErrorCode::OutOfRange, "scene renderer: sizes and item capacity must be non-zero");
    }
    std::unique_ptr<SceneRenderer> renderer(new SceneRenderer(device, desc));
    if (const RhiStatus built = renderer->build(std::move(shaders)); !built.ok()) {
        // Free what was created so far. The error that matters is the one from build().
        static_cast<void>(renderer->release());
        return core::failure(built.error());
    }
    return core::success(std::move(renderer));
}

RhiStatus SceneRenderer::build_targets() {
    const std::uint32_t w = desc_.width;
    const std::uint32_t h = desc_.height;
    const TextureUsage target_usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    // The depth values feed the reflection pass as well (US-040) and the fog's apply pass (US-045), so they are sampled when either is on.
    const TextureUsage depth_values_usage =
        target_usage | ((desc_.screen_reflections || fog_) ? TextureUsage::ShaderResource : TextureUsage::None);
    // The color and velocity targets also feed the post pass, so they carry the shader-resource usage when it is on.
    const TextureUsage color_usage = target_usage | post_source_usage();
    RhiResult<rhi::TextureHandle> texture = device_.create_texture(texture_desc(w, h, scene_color_format(), color_usage, "scene color"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    color_ = std::move(texture).value();
    texture = device_.create_texture(texture_desc(w, h, Format::R32Uint, target_usage, "scene entity ids"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    ids_ = std::move(texture).value();
    texture = device_.create_texture(texture_desc(w, h, Format::R32Float, depth_values_usage, "scene depth values"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    depth_values_ = std::move(texture).value();
    texture = device_.create_texture(texture_desc(w, h, Format::R32G32Float, color_usage, "scene velocity"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    velocity_ = std::move(texture).value();
    // The reflection inputs of the opaque pass (US-040): the shading normal with the roughness, and the Fresnel base with the occlusion.
    texture = device_.create_texture(texture_desc(w, h, Format::R16G16B16A16Float, color_usage, "scene surface"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    surface_ = std::move(texture).value();
    texture = device_.create_texture(texture_desc(w, h, Format::R16G16B16A16Float, color_usage, "scene base"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    base_ = std::move(texture).value();
    if (desc_.screen_reflections) {
        // The reflected color is the lit color with the traced specular added. The post pass reads it in place of the scene color.
        texture = device_.create_texture(texture_desc(w, h, scene_color_format(), TextureUsage::ShaderResource | TextureUsage::UnorderedAccess,
                                                      "reflected color"));
        if (!texture.ok()) {
            return core::failure(texture.error());
        }
        reflected_ = std::move(texture).value();
    }
    if (fog_) {
        // The lit frame with the fog added (US-045). The reflection and post passes read it in place of the scene color.
        texture = device_.create_texture(texture_desc(w, h, scene_color_format(), TextureUsage::ShaderResource | TextureUsage::UnorderedAccess,
                                                      "fogged color"));
        if (!texture.ok()) {
            return core::failure(texture.error());
        }
        fogged_ = std::move(texture).value();
    }
    texture = device_.create_texture(texture_desc(w, h, Format::D32Float, TextureUsage::DepthStencil, "scene depth"));
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    depth_ = std::move(texture).value();

    const BufferUsage readback_usage = BufferUsage::CpuRead | BufferUsage::CopyDestination;
    RhiResult<BufferHandle> readback = device_.create_buffer(buffer_desc(readback_size(w, h, Format::R8G8B8A8Unorm), readback_usage, "scene color readback"));
    if (!readback.ok()) {
        return core::failure(readback.error());
    }
    readback_color_ = std::move(readback).value();
    readback = device_.create_buffer(buffer_desc(readback_size(w, h, Format::R32Uint), readback_usage, "scene id readback"));
    if (!readback.ok()) {
        return core::failure(readback.error());
    }
    readback_ids_ = std::move(readback).value();
    readback = device_.create_buffer(buffer_desc(readback_size(w, h, Format::R32Float), readback_usage, "scene depth readback"));
    if (!readback.ok()) {
        return core::failure(readback.error());
    }
    readback_depth_ = std::move(readback).value();
    readback = device_.create_buffer(buffer_desc(readback_size(w, h, Format::R32G32Float), readback_usage, "scene velocity readback"));
    if (!readback.ok()) {
        return core::failure(readback.error());
    }
    readback_velocity_ = std::move(readback).value();
    return core::success();
}

RhiStatus SceneRenderer::build(SceneShaders shaders) {
    // The denoise and fog flags are fixed here. Both need the ray-traced pass, and the fog also needs the post pass for the HDR color it adds to.
    if (desc_.ray_traced.denoise && !desc_.ray_traced.enabled) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "SceneRendererDesc::ray_traced.denoise needs ray_traced.enabled");
    }
    if (desc_.ray_traced.volumetric_fog && (!desc_.ray_traced.enabled || !desc_.post.enabled)) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "SceneRendererDesc::ray_traced.volumetric_fog needs ray_traced.enabled and post");
    }
    denoise_ = desc_.ray_traced.denoise;
    fog_ = desc_.ray_traced.volumetric_fog;
    if (const RhiStatus targets = build_targets(); !targets.ok()) {
        return targets;
    }

    // Set 0: the frame buffer at t0, read by the vertex, pixel, and cull stages. Set 1: the material parameters at t0 and five maps at t1..t5, read by the pixel
    // stage. Set 2: the environment at t0, five specular levels at t1..t5, and the BRDF table at t6. The set index is the register space.
    rhi::DescriptorSetLayoutDesc frame_desc;
    // The compute stage reads it too: the indirect cull walks the candidates' records (US-037).
    frame_desc.bindings.push_back(rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1,
                                                             rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel | rhi::ShaderStage::Compute});
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device_.create_descriptor_set_layout(frame_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    frame_layout_ = std::move(layout).value();

    rhi::DescriptorSetLayoutDesc material_desc;
    for (std::uint32_t binding = 0; binding <= kMaterialTextureSlots; ++binding) {
        material_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    }
    layout = device_.create_descriptor_set_layout(material_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    material_layout_ = std::move(layout).value();

    // The reflection pass (US-040) reads the environment too, so the set is visible to compute. The probe grid is bound at binding 7.
    rhi::DescriptorSetLayoutDesc environment_desc;
    for (std::uint32_t binding = 0; binding <= kEnvironmentTextures; ++binding) {
        environment_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1,
                                                                       rhi::ShaderStage::Pixel | rhi::ShaderStage::Compute});
    }
    environment_desc.bindings.push_back(rhi::DescriptorBindingDesc{kGiBinding, rhi::DescriptorType::ShaderResource, 1,
                                                                   rhi::ShaderStage::Pixel | rhi::ShaderStage::Compute});
    environment_desc.bindings.push_back(rhi::DescriptorBindingDesc{kDynamicHistoryBinding, rhi::DescriptorType::ShaderResource, 1,
                                                                   rhi::ShaderStage::Pixel | rhi::ShaderStage::Compute});
    environment_desc.bindings.push_back(rhi::DescriptorBindingDesc{kDynamicSamplingBinding, rhi::DescriptorType::ShaderResource, 1,
                                                                   rhi::ShaderStage::Pixel | rhi::ShaderStage::Compute});
    layout = device_.create_descriptor_set_layout(environment_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    environment_layout_ = std::move(layout).value();
    if (const RhiStatus built = build_lighting(desc_); !built.ok()) {
        return built;
    }

    // Five pipelines for static meshes and five for skinned ones (kSkinnedPipelineBase), in this order: opaque, opaque two-sided, blended,
    // blended two-sided, and the shadow tile. The blended ones write color alone, so they cannot write the integer entity-ID target, and the
    // render pass that draws them has the same single color target. The shadow pipeline draws a tile: depth values into an R32Float tile,
    // with no culling, so a one-sided caster still shadows from either side. The opaque ones write color, entity ID, depth value, and the
    // motion vector, in the order the pixel shader writes them. A skinned pipeline is built only when its shaders were given.
    skinned_ready_ = !shaders.skinned_vertex.bytecode.empty() && !shaders.skinned_shadow_vertex.bytecode.empty();
    for (std::size_t index = 0; index < kIndirectPipelineBase; ++index) {
        const bool skinned = index >= kSkinnedPipelineBase;
        const std::size_t local = skinned ? index - kSkinnedPipelineBase : index;
        if (skinned && !skinned_ready_) {
            continue;
        }
        const bool shadow = local == kShadowPipeline;
        const bool blended = local == 2 || local == 3;
        const bool two_sided = local == 1 || local == 3 || shadow;
        const rhi::ShaderBinary& vertex = shadow ? (skinned ? shaders.skinned_shadow_vertex : shaders.shadow_vertex)
                                                 : (skinned ? shaders.skinned_vertex : shaders.vertex);
        rhi::PipelineDesc pipeline;
        pipeline.kind = rhi::PipelineKind::Graphics;
        pipeline.shaders = shadow ? std::vector<rhi::ShaderBinary>{vertex, shaders.shadow_pixel}
                                  : std::vector<rhi::ShaderBinary>{vertex, blended ? shaders.pixel_blend : shaders.pixel};
        pipeline.set_layouts = {frame_layout_, material_layout_, environment_layout_, lighting_layout_};
        pipeline.vertex_stride = skinned ? kSkinnedVertexStride : kVertexStride;
        pipeline.vertex_attributes = skinned ? skinned_attributes() : static_attributes();
        if (shadow) {
            pipeline.color_formats = {Format::R32Float};
            pipeline.depth_format = Format::D32Float;
            pipeline.cull_mode = rhi::CullMode::None;
            pipeline.alpha_blend = false;
            pipeline.depth_write = true;
            pipeline.debug_name = "shadow tile";
        } else {
            if (blended) {
                pipeline.color_formats = {scene_color_format()};
            } else {
                pipeline.color_formats = opaque_formats(scene_color_format());
            }
            pipeline.depth_format = Format::D32Float;
            pipeline.cull_mode = two_sided ? rhi::CullMode::None : rhi::CullMode::Back;
            pipeline.alpha_blend = blended;
            pipeline.depth_write = !blended;
            pipeline.debug_name = blended ? "scene blended" : "scene opaque";
            if (two_sided) {
                pipeline.debug_name += " two-sided";
            }
        }
        if (skinned) {
            pipeline.debug_name = "skinned " + pipeline.debug_name;
        }
        RhiResult<rhi::PipelineHandle> created = device_.create_pipeline(pipeline);
        if (!created.ok()) {
            return core::failure(created.error());
        }
        pipelines_[index] = std::move(created).value();
    }

    if (const RhiStatus indirect = build_indirect(shaders); !indirect.ok()) {
        return indirect;
    }
    if (const RhiStatus terrain = build_terrain(shaders); !terrain.ok()) {
        return terrain;
    }
    if (const RhiStatus meshlets = build_meshlets(shaders); !meshlets.ok()) {
        return meshlets;
    }

    // Defaults, in the order of kWhiteTexture and kFlatNormalTexture. A flat normal is (0.5, 0.5, 1) in bytes.
    const std::vector<std::uint8_t> white{255, 255, 255, 255};
    const std::vector<std::uint8_t> flat{128, 128, 255, 255};
    for (const std::vector<std::uint8_t>* pixel : {&white, &flat}) {
        ImageRgba8 image;
        image.width = 1;
        image.height = 1;
        image.pixels = *pixel;
        if (const RhiResult<std::uint32_t> id = upload_texture(image); !id.ok()) {
            return core::failure(id.error());
        }
    }
    if (const RhiResult<std::uint32_t> id = create_material(MaterialParams{}); !id.ok()) {
        return core::failure(id.error());
    }
    if (desc_.post.enabled) {
        RhiResult<std::unique_ptr<PostProcessor>> post = PostProcessor::create(device_, desc_.width, desc_.height, desc_.post, shaders.post);
        if (!post.ok()) {
            return core::failure(post.error());
        }
        post_ = std::move(post).value();
    }
    EquirectImage uniform;
    uniform.width = 16;
    uniform.height = 8;
    uniform.texels.assign(static_cast<std::size_t>(uniform.width) * uniform.height, glm::vec3(1.0f));
    if (const RhiStatus set = set_environment(uniform); !set.ok()) {
        return set;
    }
    // The environment set exists now, so the probe grid can be bound: until a volume is set, the grid is empty.
    if (const RhiStatus cleared = clear_gi_volume(); !cleared.ok()) {
        return cleared;
    }
    // The ray-traced pass and its dummies (US-043). They bind into the lighting sets, which build_lighting created.
    if (const RhiStatus built = build_ray_traced(shaders); !built.ok()) {
        return built;
    }
    if (desc_.screen_reflections) {
        return build_reflections(shaders);
    }
    return core::success();
}

// The indirect path (US-037): the cull pipeline and its set, the buffers the cull writes and the draws read, and four opaque pipelines whose
// vertex stage takes the item index from the per-instance attribute. The path is left off when the shaders or the IndirectCount feature are
// missing, or when the group limit is zero; then every frame draws indexed.
RhiStatus SceneRenderer::build_indirect(const SceneShaders& shaders) {
    if (desc_.draw_path != DrawPath::Indirect || desc_.max_draw_groups == 0 || shaders.cull.bytecode.empty() ||
        shaders.indirect_vertex.bytecode.empty() || !device_.capabilities().enabled(rhi::Feature::IndirectCount)) {
        return core::success();
    }
    const std::uint64_t groups = desc_.max_draw_groups;
    draw_group_capacity_ = desc_.draw_group_capacity == 0 ? desc_.max_items : desc_.draw_group_capacity;
    const std::uint64_t cull_bytes = cull_buffer_bytes(desc_);
    const std::uint64_t args_bytes = groups * draw_group_capacity_ * rhi::kDrawIndexedIndirectStride;
    const std::uint64_t count_bytes = count_buffer_bytes(desc_);

    // Set 1 of the cull entry: CullData at t0, CullArgs at u1, and CullCounts at u2 (the binding is the register; scene.hlsl).
    rhi::DescriptorSetLayoutDesc cull_desc;
    cull_desc.bindings.push_back(rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
    cull_desc.bindings.push_back(rhi::DescriptorBindingDesc{1, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
    cull_desc.bindings.push_back(rhi::DescriptorBindingDesc{2, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device_.create_descriptor_set_layout(cull_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    cull_layout_ = std::move(layout).value();

    rhi::PipelineDesc cull;
    cull.kind = rhi::PipelineKind::Compute;
    cull.shaders = {shaders.cull};
    cull.set_layouts = {frame_layout_, cull_layout_};
    cull.debug_name = "indirect cull";
    RhiResult<rhi::PipelineHandle> cull_pipeline = device_.create_pipeline(cull);
    if (!cull_pipeline.ok()) {
        return core::failure(cull_pipeline.error());
    }
    cull_pipeline_ = std::move(cull_pipeline).value();

    const BufferUsage gpu_buffer = BufferUsage::ShaderResource | BufferUsage::CopyDestination;
    RhiResult<BufferHandle> buffer = device_.create_buffer(buffer_desc(cull_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "cull upload"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    cull_upload_ = std::move(buffer).value();
    buffer = device_.create_buffer(buffer_desc(cull_bytes, gpu_buffer, "cull data"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    cull_data_ = std::move(buffer).value();
    buffer = device_.create_buffer(buffer_desc(args_bytes, BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument, "indirect arguments"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    cull_args_ = std::move(buffer).value();
    const BufferUsage count_usage = BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument | BufferUsage::CopySource | BufferUsage::CopyDestination;
    buffer = device_.create_buffer(buffer_desc(count_bytes, count_usage, "indirect counts"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    cull_counts_ = std::move(buffer).value();
    buffer = device_.create_buffer(buffer_desc(count_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "indirect counts zero"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    counts_zero_ = std::move(buffer).value();
    buffer = device_.create_buffer(buffer_desc(count_bytes, BufferUsage::CpuRead | BufferUsage::CopyDestination, "indirect counts readback"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    counts_readback_ = std::move(buffer).value();
    buffer = device_.create_buffer(buffer_desc(4ull * desc_.max_items, BufferUsage::Vertex | BufferUsage::CopyDestination, "instance ids"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    instance_ids_ = std::move(buffer).value();

    // The zeros the counts start from, and the identity sequence the per-instance input reads.
    if (const RhiStatus zeroed = device_.upload_buffer(counts_zero_, 0, count_bytes, std::vector<std::uint8_t>(count_bytes, 0).data()); !zeroed.ok()) {
        return zeroed;
    }
    std::vector<std::uint8_t> identity(4ull * desc_.max_items);
    for (std::uint32_t item = 0; item < desc_.max_items; ++item) {
        put_u32(identity, 4ull * item, item);
    }
    if (const RhiStatus filled = upload_buffer_bytes(instance_ids_, identity.size(), identity, BufferUsage::Vertex | BufferUsage::CopyDestination,
                                                     ResourceState::Common);
        !filled.ok()) {
        return filled;
    }

    RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(cull_layout_);
    if (!set.ok()) {
        return core::failure(set.error());
    }
    cull_set_ = std::move(set).value();
    if (const RhiStatus written = device_.write_buffer(cull_set_, 0, 0, cull_data_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_buffer(cull_set_, 1, 0, cull_args_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_buffer(cull_set_, 2, 0, cull_counts_); !written.ok()) {
        return written;
    }

    // Four pipelines: static one-sided, static two-sided, skinned one-sided, skinned two-sided. The per-instance input is the item index.
    skinned_indirect_ready_ = skinned_ready_ && !shaders.skinned_indirect_vertex.bytecode.empty();
    for (std::size_t local = 0; local < 4; ++local) {
        const bool skinned = local >= 2;
        const bool two_sided = (local & 1) != 0;
        if (skinned && !skinned_indirect_ready_) {
            continue;
        }
        rhi::PipelineDesc pipeline;
        pipeline.kind = rhi::PipelineKind::Graphics;
        pipeline.shaders = {skinned ? shaders.skinned_indirect_vertex : shaders.indirect_vertex, shaders.pixel};
        pipeline.set_layouts = {frame_layout_, material_layout_, environment_layout_, lighting_layout_};
        pipeline.vertex_stride = skinned ? kSkinnedVertexStride : kVertexStride;
        pipeline.vertex_attributes = skinned ? skinned_attributes() : static_attributes();
        pipeline.instance_stride = 4;
        pipeline.instance_attributes = {rhi::VertexAttribute{skinned ? kSkinnedInstanceLocation : kStaticInstanceLocation, Format::R32Uint, 0}};
        pipeline.color_formats = opaque_formats(scene_color_format());
        pipeline.depth_format = Format::D32Float;
        pipeline.cull_mode = two_sided ? rhi::CullMode::None : rhi::CullMode::Back;
        pipeline.alpha_blend = false;
        pipeline.depth_write = true;
        pipeline.debug_name = std::string(skinned ? "skinned " : "") + "scene indirect opaque" + (two_sided ? " two-sided" : "");
        RhiResult<rhi::PipelineHandle> created = device_.create_pipeline(pipeline);
        if (!created.ok()) {
            return core::failure(created.error());
        }
        pipelines_[kIndirectPipelineBase + local] = std::move(created).value();
    }
    indirect_ready_ = true;
    return core::success();
}

// The terrain pipelines (US-047): the opaque tile and its shadow tile over the terrain stride. They take the static frame layouts, so a tile's
// records and material bind as a static mesh's do. Only terrain meshes draw through them.
RhiStatus SceneRenderer::build_terrain(const SceneShaders& shaders) {
    terrain_ready_ = !shaders.terrain_vertex.bytecode.empty() && !shaders.terrain_shadow_vertex.bytecode.empty();
    if (!terrain_ready_) {
        return core::success();
    }
    rhi::PipelineDesc opaque;
    opaque.kind = rhi::PipelineKind::Graphics;
    opaque.shaders = {shaders.terrain_vertex, shaders.pixel};
    opaque.set_layouts = {frame_layout_, material_layout_, environment_layout_, lighting_layout_};
    opaque.vertex_stride = kTerrainVertexStride;
    opaque.vertex_attributes = terrain_attributes();
    opaque.color_formats = opaque_formats(scene_color_format());
    opaque.depth_format = Format::D32Float;
    opaque.cull_mode = rhi::CullMode::Back;
    opaque.alpha_blend = false;
    opaque.depth_write = true;
    opaque.debug_name = "terrain opaque";
    RhiResult<rhi::PipelineHandle> created_opaque = device_.create_pipeline(opaque);
    if (!created_opaque.ok()) {
        return core::failure(created_opaque.error());
    }
    pipelines_[kTerrainOpaquePipeline] = std::move(created_opaque).value();

    rhi::PipelineDesc shadow;
    shadow.kind = rhi::PipelineKind::Graphics;
    shadow.shaders = {shaders.terrain_shadow_vertex, shaders.shadow_pixel};
    shadow.set_layouts = {frame_layout_, material_layout_, environment_layout_, lighting_layout_};
    shadow.vertex_stride = kTerrainVertexStride;
    shadow.vertex_attributes = terrain_attributes();
    shadow.color_formats = {Format::R32Float};
    shadow.depth_format = Format::D32Float;
    shadow.cull_mode = rhi::CullMode::None;
    shadow.alpha_blend = false;
    shadow.depth_write = true;
    shadow.debug_name = "terrain shadow tile";
    RhiResult<rhi::PipelineHandle> created_shadow = device_.create_pipeline(shadow);
    if (!created_shadow.ok()) {
        return core::failure(created_shadow.error());
    }
    pipelines_[kTerrainShadowPipeline] = std::move(created_shadow).value();
    return core::success();
}

// The meshlet path (US-038): the layout of the mesh pipelines' set 0, the pools every attached mesh appends to, the visibility counters, and
// the two opaque pipelines. The path needs the MeshShaders feature and both entries. Without them the renderer draws the conventional way, and
// attach_meshlets fails.
RhiStatus SceneRenderer::build_meshlets(const SceneShaders& shaders) {
    if (shaders.meshlet_task.bytecode.empty() || shaders.meshlet_mesh.bytecode.empty() || !device_.capabilities().enabled(rhi::Feature::MeshShaders)) {
        return core::success();
    }
    // Set 0 of the mesh pipelines: the frame buffer at t0 (read by all three stages), the pools at t1..t4, and the counters at u5. The binding is the
    // register number, as in scene.hlsl.
    rhi::DescriptorSetLayoutDesc layout_desc;
    const rhi::ShaderStage mesh_stages = rhi::ShaderStage::Amplification | rhi::ShaderStage::Mesh;
    layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1,
                                                              mesh_stages | rhi::ShaderStage::Pixel});
    for (std::uint32_t binding = 1; binding <= 4; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, mesh_stages});
    }
    layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{5, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Amplification});
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device_.create_descriptor_set_layout(layout_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    meshlet_layout_ = std::move(layout).value();

    const MeshletPoolSettings& pool = desc_.meshlets;
    const BufferUsage gpu_buffer = BufferUsage::ShaderResource | BufferUsage::CopyDestination;
    const BufferUsage counts_usage = BufferUsage::UnorderedAccess | BufferUsage::CopySource | BufferUsage::CopyDestination;
    const BufferUsage counts_readback_usage = BufferUsage::CpuRead | BufferUsage::CopyDestination;
    struct Pool {
        rhi::BufferHandle* handle;
        std::uint64_t bytes;
        BufferUsage usage;
        const char* name;
    };
    const std::array<Pool, 6> pools{{
        {&meshlet_records_, static_cast<std::uint64_t>(pool.meshlets) * kMeshletRecordBytes, gpu_buffer, "meshlet records"},
        {&meshlet_vertices_, static_cast<std::uint64_t>(pool.vertices) * 4, gpu_buffer, "meshlet vertices"},
        {&meshlet_triangles_, pool.triangle_bytes, gpu_buffer, "meshlet triangles"},
        {&mesh_vertex_pool_, static_cast<std::uint64_t>(pool.mesh_vertices) * kVertexStride, gpu_buffer, "mesh vertex pool"},
        {&meshlet_counts_, static_cast<std::uint64_t>(pool.task_groups) * 4, counts_usage, "meshlet counts"},
        {&meshlet_counts_readback_, static_cast<std::uint64_t>(pool.task_groups) * 4, counts_readback_usage, "meshlet counts readback"},
    }};
    for (const Pool& buffer : pools) {
        RhiResult<BufferHandle> created = device_.create_buffer(buffer_desc(buffer.bytes, buffer.usage, buffer.name));
        if (!created.ok()) {
            return core::failure(created.error());
        }
        *buffer.handle = std::move(created).value();
    }

    // Two opaque pipelines: one-sided and two-sided. They write the four targets of the opaque pass, in the order PSMain writes them, and read
    // the pixel stage's material and environment sets as the indexed opaque pipelines do.
    for (std::size_t local = 0; local < 2; ++local) {
        const bool two_sided = local == 1;
        rhi::PipelineDesc pipeline;
        pipeline.kind = rhi::PipelineKind::Mesh;
        pipeline.shaders = {shaders.meshlet_task, shaders.meshlet_mesh, shaders.pixel};
        pipeline.set_layouts = {meshlet_layout_, material_layout_, environment_layout_, lighting_layout_};
        pipeline.color_formats = opaque_formats(scene_color_format());
        pipeline.depth_format = Format::D32Float;
        pipeline.cull_mode = two_sided ? rhi::CullMode::None : rhi::CullMode::Back;
        pipeline.alpha_blend = false;
        pipeline.depth_write = true;
        pipeline.debug_name = two_sided ? "scene meshlet opaque two-sided" : "scene meshlet opaque";
        RhiResult<rhi::PipelineHandle> created = device_.create_pipeline(pipeline);
        if (!created.ok()) {
            return core::failure(created.error());
        }
        pipelines_[kMeshletPipelineBase + local] = std::move(created).value();
    }
    meshlets_ready_ = true;
    return core::success();
}

// Appends a mesh's cooked meshlets to the pools. The pool bytes are built in full, then uploaded, and only then committed, so a failed upload
// leaves the pools as they were. Every upload copies the whole list, so the GPU always holds exactly what was attached.
RhiStatus SceneRenderer::attach_meshlets(std::uint32_t mesh, const MeshletData& meshlets) {
    if (mesh >= gpu_meshes_.size()) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "attach_meshlets: the mesh index is not an uploaded mesh");
    }
    if (!meshlets_ready_) {
        return rhi::fail(RhiErrorCode::Unsupported, "attach_meshlets: mesh shaders are not available (the MeshShaders feature or the shader entries)");
    }
    if (gpu_meshes_[mesh].bones > 0) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "attach_meshlets: skinned meshes draw on the indirect path, not with meshlets");
    }
    if (gpu_meshes_[mesh].meshlet_count != 0) {
        return rhi::fail(RhiErrorCode::InvalidState, "attach_meshlets: the mesh already has meshlets");
    }
    if (const std::optional<std::string> problem = validate_meshlets(meshes_[mesh], meshlets)) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "attach_meshlets: " + *problem);
    }

    const MeshletPoolSettings& pool = desc_.meshlets;
    const std::uint64_t record_count = pool_records_.size() / kMeshletRecordBytes;
    const std::uint64_t vertex_count = pool_vertices_.size() / 4;
    const std::uint64_t triangle_count = pool_triangles_.size();
    const std::uint64_t mesh_vertex_count = pool_mesh_vertex_count_;
    if (record_count + meshlets.meshlets.size() > pool.meshlets || vertex_count + meshlets.vertices.size() > pool.vertices ||
        triangle_count + meshlets.triangles.size() > pool.triangle_bytes ||
        mesh_vertex_count + meshes_[mesh].positions.size() > pool.mesh_vertices) {
        return rhi::fail(RhiErrorCode::OutOfRange, "attach_meshlets: a meshlet pool has no room for the mesh");
    }

    // The mesh's vertices join the pool in the static layout the indexed path uses, so both paths read the same values.
    std::vector<std::uint8_t> records = pool_records_;
    std::vector<std::uint8_t> vertices = pool_vertices_;
    std::vector<std::uint8_t> triangles = pool_triangles_;
    std::vector<std::uint8_t> mesh_vertices = pool_mesh_vertices_;
    const std::vector<std::uint8_t> packed = pack_vertices(meshes_[mesh], kVertexStride);
    mesh_vertices.insert(mesh_vertices.end(), packed.begin(), packed.end());
    for (const std::uint32_t index : meshlets.vertices) {
        const std::uint32_t pooled = static_cast<std::uint32_t>(mesh_vertex_count) + index;
        const std::size_t at = vertices.size();
        vertices.resize(at + 4);
        put_u32(vertices, at, pooled);
    }
    triangles.insert(triangles.end(), meshlets.triangles.begin(), meshlets.triangles.end());
    for (const MeshletCluster& meshlet : meshlets.meshlets) {
        const std::size_t at = records.size();
        records.resize(at + kMeshletRecordBytes);
        put_u32(records, at, static_cast<std::uint32_t>(vertex_count + meshlet.vertex_offset));
        put_u32(records, at + 4, meshlet.vertex_count);
        put_u32(records, at + 8, static_cast<std::uint32_t>(triangle_count + meshlet.triangle_offset));
        put_u32(records, at + 12, meshlet.triangle_count);
        put_floats(records, at + 16, &meshlet.center.x, 3);
        put_f32(records, at + 28, meshlet.radius);
        put_floats(records, at + 32, &meshlet.cone_axis.x, 3);
        put_f32(records, at + 44, meshlet.cone_cutoff);
    }

    // The upload starts from the pools' current state: from Common before the first attach, and from ShaderRead after it.
    const rhi::ResourceState before = meshlet_pools_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    const std::array<std::pair<rhi::BufferHandle, const std::vector<std::uint8_t>*>, 4> uploads{{{meshlet_records_, &records},
                                                                                                  {meshlet_vertices_, &vertices},
                                                                                                  {meshlet_triangles_, &triangles},
                                                                                                  {mesh_vertex_pool_, &mesh_vertices}}};
    const std::array<std::uint64_t, 4> capacities{desc_.meshlets.meshlets * kMeshletRecordBytes, desc_.meshlets.vertices * 4ull,
                                                  desc_.meshlets.triangle_bytes, desc_.meshlets.mesh_vertices * kVertexStride};
    for (std::size_t i = 0; i < uploads.size(); ++i) {
        if (const RhiStatus uploaded = upload_buffer_bytes(uploads[i].first, capacities[i], *uploads[i].second, BufferUsage::ShaderResource | BufferUsage::CopyDestination,
                                                           ResourceState::ShaderRead, before);
            !uploaded.ok()) {
            return uploaded;
        }
    }

    pool_records_ = std::move(records);
    pool_vertices_ = std::move(vertices);
    pool_triangles_ = std::move(triangles);
    pool_mesh_vertices_ = std::move(mesh_vertices);
    pool_mesh_vertex_count_ = static_cast<std::uint32_t>(mesh_vertex_count + meshes_[mesh].positions.size());
    meshlet_pools_uploaded_ = true;
    gpu_meshes_[mesh].meshlet_base = static_cast<std::uint32_t>(record_count);
    gpu_meshes_[mesh].meshlet_count = static_cast<std::uint32_t>(meshlets.meshlets.size());
    return core::success();
}

RhiStatus SceneRenderer::build_lighting(const SceneRendererDesc& desc) {
    const LightingSettings& lighting = desc.lighting;
    const ShadowMapSettings& shadows = desc.shadows;
    if (lighting.max_lights == 0 || lighting.tile_px == 0 || lighting.slices == 0 || lighting.max_lights_per_cluster == 0 ||
        lighting.max_lights_per_cluster > 0xFFFFu || desc.max_shadow_maps == 0) {
        return rhi::fail(RhiErrorCode::OutOfRange, "scene renderer: lighting budgets must be non-zero and the cluster list must fit 16 bits");
    }
    if (shadows.cascade_size == 0 || shadows.cascade_size > shadows.cascade_atlas_size || shadows.local_size == 0 ||
        shadows.local_size > shadows.local_atlas_size || desc.cascades.count == 0 || desc.cascades.count > kMaxCascades) {
        return rhi::fail(RhiErrorCode::OutOfRange, "scene renderer: shadow tiles must fit their atlases and the cascade count must be 1 to 4");
    }

    rhi::DescriptorSetLayoutDesc lighting_desc;
    lighting_desc.bindings.push_back(rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    lighting_desc.bindings.push_back(rhi::DescriptorBindingDesc{1, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    lighting_desc.bindings.push_back(rhi::DescriptorBindingDesc{2, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    // The ray-traced shadow and reflection targets (US-043), read by the scene pass at the same pixel.
    lighting_desc.bindings.push_back(rhi::DescriptorBindingDesc{3, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    lighting_desc.bindings.push_back(rhi::DescriptorBindingDesc{4, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device_.create_descriptor_set_layout(lighting_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    lighting_layout_ = std::move(layout).value();

    // The cluster grid of the target. The cluster count is fixed by the target size and the settings, so the buffer is sized once.
    const std::uint64_t tiles_x = (desc.width + lighting.tile_px - 1) / lighting.tile_px;
    const std::uint64_t tiles_y = (desc.height + lighting.tile_px - 1) / lighting.tile_px;
    const std::uint64_t clusters = tiles_x * tiles_y * lighting.slices;
    lighting_layout_offsets_.light_base = kLightingHeaderBytes;
    lighting_layout_offsets_.shadow_base = lighting_layout_offsets_.light_base + kLightRecordBytes * lighting.max_lights;
    lighting_layout_offsets_.count_base = lighting_layout_offsets_.shadow_base + kShadowRecordBytes * desc.max_shadow_maps;
    lighting_layout_offsets_.index_base = lighting_layout_offsets_.count_base + 4 * clusters;
    lighting_layout_offsets_.bytes = lighting_layout_offsets_.index_base + 4 * clusters * lighting.max_lights_per_cluster;
    const std::uint64_t bytes = lighting_layout_offsets_.bytes;

    RhiResult<BufferHandle> buffer = device_.create_buffer(buffer_desc(bytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "lighting"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    lighting_buffer_ = std::move(buffer).value();
    buffer = device_.create_buffer(buffer_desc(bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "lighting upload"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    lighting_upload_ = std::move(buffer).value();
    // An empty frame: no lights, and the buffer is in ShaderRead for the first frame.
    if (const RhiStatus zeroed = upload_buffer_bytes(lighting_buffer_, bytes, std::vector<std::uint8_t>(bytes, 0)); !zeroed.ok()) {
        return zeroed;
    }

    RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(lighting_layout_);
    if (!set.ok()) {
        return core::failure(set.error());
    }
    lighting_set_ = std::move(set).value();

    if (const RhiStatus atlases = build_atlases(); !atlases.ok()) {
        return atlases;
    }
    if (const RhiStatus written = device_.write_buffer(lighting_set_, 0, 0, lighting_buffer_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(lighting_set_, 1, 0, cascade_color_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(lighting_set_, 2, 0, local_color_); !written.ok()) {
        return written;
    }

    // The shadow set: the same buffer, and a 1x1 texel in both atlas slots (never read by the shadow pass, but bound, so it must be valid).
    RhiResult<rhi::TextureHandle> dummy = make_texture(1, 1, Format::R32Float, "shadow dummy");
    if (!dummy.ok()) {
        return core::failure(dummy.error());
    }
    shadow_dummy_ = std::move(dummy).value();
    std::vector<std::uint8_t> texel(rhi::copy_row_pitch(1, Format::R32Float), 0);
    const float one = 1.0f;
    std::memcpy(texel.data(), &one, sizeof(one));
    if (const RhiStatus uploaded = upload_texture_bytes(shadow_dummy_, 1, 1, Format::R32Float, texel); !uploaded.ok()) {
        return uploaded;
    }
    RhiResult<rhi::DescriptorSetHandle> shadow_set = device_.create_descriptor_set(lighting_layout_);
    if (!shadow_set.ok()) {
        return core::failure(shadow_set.error());
    }
    shadow_lighting_set_ = std::move(shadow_set).value();
    if (const RhiStatus written = device_.write_buffer(shadow_lighting_set_, 0, 0, lighting_buffer_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(shadow_lighting_set_, 1, 0, shadow_dummy_); !written.ok()) {
        return written;
    }
    return device_.write_texture(shadow_lighting_set_, 2, 0, shadow_dummy_);
}

namespace {

// The triangles of a mesh in object space, one record per index triple, as the ray-traced pass reads them.
std::vector<RtTriangleRecord> ray_traced_triangles(const MeshData& mesh) {
    std::vector<RtTriangleRecord> triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (std::size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
        RtTriangleRecord triangle;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const std::uint32_t vertex = mesh.indices[first + corner];
            triangle.positions[corner] = mesh.positions[vertex];
            triangle.uvs[corner] = mesh.uvs.empty() ? glm::vec2(0.0f) : mesh.uvs[vertex];
        }
        triangles.push_back(triangle);
    }
    return triangles;
}

// A mesh buffer's usage. A ray-traced renderer adds ShaderResource, because the bottom-level build reads the geometry as a shader resource.
rhi::BufferUsage mesh_buffer_usage(rhi::BufferUsage base, bool ray_traced) {
    return ray_traced ? (base | rhi::BufferUsage::ShaderResource) : base;
}

}  // namespace

RhiStatus SceneRenderer::set_ray_traced_settings(const RayTracedSettings& settings) {
    if (settings.enabled != ray_traced_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_ray_traced_settings: the enabled flag is fixed at create");
    }
    if (settings.dynamic_gi != static_cast<bool>(dynamic_gi_)) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_ray_traced_settings: the dynamic GI flag is fixed at create");
    }
    if (settings.denoise != denoise_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_ray_traced_settings: the denoise flag is fixed at create");
    }
    if (settings.volumetric_fog != fog_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_ray_traced_settings: the volumetric fog flag is fixed at create");
    }
    if (settings.fog.width != desc_.ray_traced.fog.width || settings.fog.height != desc_.ray_traced.fog.height ||
        settings.fog.depth != desc_.ray_traced.fog.depth) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_ray_traced_settings: the fog grid is fixed at create");
    }
    if (!std::isfinite(settings.roughness_cutoff) || settings.roughness_cutoff < 0.0f || settings.roughness_cutoff > 1.0f ||
        !std::isfinite(settings.reflection_distance) || settings.reflection_distance < 0.0f || !std::isfinite(settings.shadow_distance) ||
        settings.shadow_distance < 0.0f) {
        return rhi::fail(RhiErrorCode::OutOfRange, "set_ray_traced_settings: the cutoff must be in [0, 1] and the distances non-negative");
    }
    desc_.ray_traced = settings;
    return core::success();
}

RhiStatus SceneRenderer::build_ray_traced(const SceneShaders& shaders) {
    if (desc_.ray_traced.dynamic_gi && !desc_.ray_traced.enabled) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "SceneRendererDesc::ray_traced.dynamic_gi needs ray_traced.enabled");
    }
    // The dummies are 1x1 RGBA16F: a shadow of 1 (lit) and a reflection of 0 (none), the values the scene reads when the pass is off.
    RhiResult<rhi::TextureHandle> shadow = make_texture(1, 1, Format::R16G16B16A16Float, "ray-traced shadow dummy");
    if (!shadow.ok()) {
        return core::failure(shadow.error());
    }
    rt_shadow_dummy_ = std::move(shadow).value();
    RhiResult<rhi::TextureHandle> reflection = make_texture(1, 1, Format::R16G16B16A16Float, "ray-traced reflection dummy");
    if (!reflection.ok()) {
        return core::failure(reflection.error());
    }
    rt_reflection_dummy_ = std::move(reflection).value();
    std::vector<std::uint8_t> lit(rhi::copy_row_pitch(1, Format::R16G16B16A16Float), 0);
    const std::vector<std::uint8_t> dark(lit.size(), 0);
    constexpr std::uint16_t kHalfOne = 0x3c00;  // 1.0 in binary16
    for (std::size_t channel = 0; channel < 4; ++channel) {
        std::memcpy(lit.data() + 2 * channel, &kHalfOne, sizeof(kHalfOne));
    }
    if (const RhiStatus uploaded = upload_texture_bytes(rt_shadow_dummy_, 1, 1, Format::R16G16B16A16Float, lit); !uploaded.ok()) {
        return uploaded;
    }
    if (const RhiStatus uploaded = upload_texture_bytes(rt_reflection_dummy_, 1, 1, Format::R16G16B16A16Float, dark); !uploaded.ok()) {
        return uploaded;
    }

    // The dynamic GI slots are bound in every frame, so their dummies exist whether or not the pass does: zeroed history, and a sampling
    // block with enabled = 0, which makes the scene read the baked grid.
    RhiResult<BufferHandle> history_dummy = device_.create_buffer(buffer_desc(kDynamicProbeBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination,
                                                                             "dynamic GI history dummy"));
    if (!history_dummy.ok()) {
        return core::failure(history_dummy.error());
    }
    dynamic_history_dummy_ = std::move(history_dummy).value();
    RhiResult<BufferHandle> sampling_dummy = device_.create_buffer(
        buffer_desc(kDynamicSamplingHeaderBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "dynamic GI sampling dummy"));
    if (!sampling_dummy.ok()) {
        return core::failure(sampling_dummy.error());
    }
    dynamic_sampling_dummy_ = std::move(sampling_dummy).value();
    const std::vector<std::uint8_t> zeros_history(kDynamicProbeBytes, 0);
    if (const RhiStatus uploaded = upload_buffer_bytes(dynamic_history_dummy_, kDynamicProbeBytes, zeros_history); !uploaded.ok()) {
        return uploaded;
    }
    const std::vector<std::uint8_t> zeros_sampling(kDynamicSamplingHeaderBytes, 0);
    if (const RhiStatus uploaded = upload_buffer_bytes(dynamic_sampling_dummy_, kDynamicSamplingHeaderBytes, zeros_sampling); !uploaded.ok()) {
        return uploaded;
    }

    if (desc_.ray_traced.enabled) {
        if (!device_.capabilities().enabled(rhi::Feature::RayTracing)) {
            return rhi::fail(RhiErrorCode::FeatureDisabled, "SceneRendererDesc::ray_traced needs the device's RayTracing feature");
        }
        if (shaders.ray_traced.bytecode.empty()) {
            return rhi::fail(RhiErrorCode::InvalidState, "SceneRendererDesc::ray_traced needs SceneShaders::ray_traced (CSRayTracedEffects)");
        }
        ray_traced_shader_ = shaders.ray_traced;
        RhiResult<std::unique_ptr<RtEffects>> effects = RtEffects::create(device_, desc_.width, desc_.height, ray_traced_shader_);
        if (!effects.ok()) {
            return core::failure(effects.error());
        }
        rt_effects_ = std::move(effects).value();
        ray_traced_ = true;
        if (desc_.ray_traced.dynamic_gi) {
            if (shaders.dynamic_gi.bytecode.empty()) {
                return rhi::fail(RhiErrorCode::InvalidState, "SceneRendererDesc::ray_traced.dynamic_gi needs SceneShaders::dynamic_gi (CSDynamicGi)");
            }
            RhiResult<std::unique_ptr<DynamicGi>> dynamic = DynamicGi::create(device_, full_profile_grid(), shaders.dynamic_gi);
            if (!dynamic.ok()) {
                return core::failure(dynamic.error());
            }
            dynamic_gi_ = std::move(dynamic).value();
        }
        if (denoise_) {
            if (shaders.denoise.bytecode.empty()) {
                return rhi::fail(RhiErrorCode::InvalidState, "SceneRendererDesc::ray_traced.denoise needs SceneShaders::denoise (CSTemporalDenoise)");
            }
            denoise_shader_ = shaders.denoise;
            if (const RhiStatus built = build_denoisers(); !built.ok()) {
                return built;
            }
        }
        if (fog_) {
            if (shaders.fog_inject.bytecode.empty() || shaders.fog_integrate.bytecode.empty() || shaders.fog_apply.bytecode.empty()) {
                return rhi::fail(RhiErrorCode::InvalidState, "SceneRendererDesc::ray_traced.volumetric_fog needs SceneShaders::fog_inject, fog_integrate, and fog_apply");
            }
            fog_inject_shader_ = shaders.fog_inject;
            fog_integrate_shader_ = shaders.fog_integrate;
            fog_apply_shader_ = shaders.fog_apply;
            if (const RhiStatus built = build_fog(); !built.ok()) {
                return built;
            }
        }
    }
    if (const RhiStatus bound = bind_dynamic_gi(); !bound.ok()) {
        return bound;
    }
    return bind_ray_traced_outputs();
}

RhiStatus SceneRenderer::build_denoisers() {
    // The denoisers are sized to the frame, and they read the pass's targets. A rebuild (after a resize) releases the old ones first; their
    // histories restart.
    for (std::unique_ptr<TemporalDenoiser>* denoiser : {&denoise_shadow_, &denoise_reflection_}) {
        if (*denoiser) {
            if (const RhiStatus released = (*denoiser)->release(); !released.ok()) {
                return released;
            }
            denoiser->reset();
        }
    }
    RhiResult<std::unique_ptr<TemporalDenoiser>> shadow = TemporalDenoiser::create(device_, desc_.width, desc_.height, denoise_shader_);
    if (!shadow.ok()) {
        return core::failure(shadow.error());
    }
    denoise_shadow_ = std::move(shadow).value();
    RhiResult<std::unique_ptr<TemporalDenoiser>> reflection = TemporalDenoiser::create(device_, desc_.width, desc_.height, denoise_shader_);
    if (!reflection.ok()) {
        return core::failure(reflection.error());
    }
    denoise_reflection_ = std::move(reflection).value();

    // The geometry is shared: the pass writes the primary hit's depth, normal, and motion (rt_effects.hlsl), so both denoisers see this frame's.
    DenoiseInputs inputs;
    inputs.depth = rt_effects_->depth();
    inputs.normal = rt_effects_->normal();
    inputs.motion = rt_effects_->motion();
    inputs.value = rt_effects_->shadow();
    if (const RhiStatus bound = denoise_shadow_->set_inputs(inputs); !bound.ok()) {
        return bound;
    }
    inputs.value = rt_effects_->reflection();
    return denoise_reflection_->set_inputs(inputs);
}

RhiStatus SceneRenderer::build_fog() {
    if (!volumetric_fog_) {
        const FogSettings& fog = desc_.ray_traced.fog;
        RhiResult<std::unique_ptr<VolumetricFog>> created =
            VolumetricFog::create(device_, fog.width, fog.height, fog.depth, fog_inject_shader_, fog_integrate_shader_, fog_apply_shader_);
        if (!created.ok()) {
            return core::failure(created.error());
        }
        volumetric_fog_ = std::move(created).value();
    }
    // The apply pass reads the lit color and the depth, and writes the fogged color. The targets are the frame's, so this runs after each
    // rebuild of them.
    FogApplyTargets targets;
    targets.color = color_;
    targets.depth = depth_values_;
    targets.output = fogged_;
    targets.width = desc_.width;
    targets.height = desc_.height;
    return volumetric_fog_->set_apply_targets(targets);
}

RhiStatus SceneRenderer::update_denoise_and_fog(const FrameSnapshot& snapshot, const RtFrame& frame) {
    const Camera& camera = snapshot.camera();
    const std::optional<glm::vec3> sun = sun_toward(snapshot);
    const std::vector<glm::dvec3> local_lights = point_light_positions(snapshot);

    // A camera cut (or the first frame) discards the history of both effects. A moved sun discards the shadow and reflection history; a moved
    // point light discards the fog's, which is the only pass that takes the local lights.
    const bool cut = camera_cut_ || (frame_history_ && camera_cut(CameraPose{glm::vec3(previous_camera_position_), previous_camera_forward_},
                                                                  CameraPose{glm::vec3(camera.position), frame.forward}, kCameraCutMeters,
                                                                  kCameraCutDegrees));
    bool sun_moved = sun.has_value() != previous_sun_direction_.has_value();
    if (sun && previous_sun_direction_) {
        sun_moved = light_changed(*previous_sun_direction_, *sun, kLightResetDegrees);
    }
    bool locals_moved = local_lights.size() != previous_local_lights_.size();
    for (std::size_t i = 0; !locals_moved && i < local_lights.size(); ++i) {
        locals_moved = glm::length(glm::vec3(local_lights[i] - previous_local_lights_[i])) > kLightResetMeters;
    }
    const bool first = !frame_history_;

    if (denoise_) {
        const bool reset = first || cut || sun_moved;
        if (const RhiStatus updated = denoise_shadow_->update_frame(desc_.ray_traced.denoiser, reset); !updated.ok()) {
            return updated;
        }
        if (const RhiStatus updated = denoise_reflection_->update_frame(desc_.ray_traced.denoiser, reset); !updated.ok()) {
            return updated;
        }
        if (const RhiStatus bound = bind_ray_traced_outputs(); !bound.ok()) {
            return bound;
        }
    }
    if (fog_) {
        const FogSettings& settings = desc_.ray_traced.fog;
        // The camera is at the origin of the traced scene, so the eye and the lights are camera-relative, as the RT pass's are.
        const float tan_half = std::tan(0.5f * frame.fov_y_radians);
        FogFrame fog;
        fog.eye = glm::vec3(0.0f);
        fog.forward = frame.forward;
        fog.up = frame.up * tan_half;
        fog.right = glm::normalize(glm::cross(frame.forward, frame.up)) * (tan_half * frame.aspect);
        fog.previous_eye = frame.previous_eye;
        fog.previous_forward = frame.previous_forward;
        fog.previous_up = frame.previous_up * tan_half;
        fog.previous_right = glm::normalize(glm::cross(frame.previous_forward, frame.previous_up)) * (tan_half * frame.aspect);
        fog.sun_direction = frame.sun_direction;
        fog.sun_radiance = frame.sun_radiance;
        fog.albedo = settings.albedo;
        fog.density = settings.density;
        fog.height_falloff = settings.height_falloff;
        fog.anisotropy = settings.anisotropy;
        fog.near_plane = static_cast<float>(camera.near_plane);
        fog.far_plane = settings.max_distance;
        fog.camera_near = static_cast<float>(camera.near_plane);
        fog.camera_far = static_cast<float>(camera.far_plane);
        fog.shadows = settings.shadows;
        fog.reset = first || cut || sun_moved || locals_moved;
        fog.history_weight = settings.history_weight;
        fog.max_age = settings.max_age;
        for (const Light& light : snapshot.lights()) {
            if (light.type == LightType::Point && fog.lights.size() < kFogMaxLights) {
                FogLight local;
                local.position = glm::vec3(light.position - camera.position);
                local.radiance = light.color * light.intensity;
                local.range = light.range;
                fog.lights.push_back(local);
            }
        }
        if (const RhiStatus bound = volumetric_fog_->set_scene(top_level_); !bound.ok()) {
            return bound;
        }
        if (const RhiStatus updated = volumetric_fog_->update_frame(fog); !updated.ok()) {
            return updated;
        }
    }
    return core::success();
}

RhiStatus SceneRenderer::bind_dynamic_gi() {
    if (!environment_set_.valid()) {
        return rhi::fail(RhiErrorCode::InvalidState, "bind_dynamic_gi: the environment is not set");
    }
    const BufferHandle history = dynamic_gi_ ? dynamic_gi_->history() : dynamic_history_dummy_;
    const BufferHandle sampling = dynamic_gi_ ? dynamic_gi_->sampling() : dynamic_sampling_dummy_;
    if (const RhiStatus written = device_.write_buffer(environment_set_, kDynamicHistoryBinding, 0, history); !written.ok()) {
        return written;
    }
    return device_.write_buffer(environment_set_, kDynamicSamplingBinding, 0, sampling);
}

RhiStatus SceneRenderer::bind_ray_traced_outputs() {
    // With the denoiser on, the scene reads the texture the denoiser writes in this frame (its next output). The descriptors are read when the
    // frame runs, so they are written before the graph is executed.
    rhi::TextureHandle shadow = ray_traced_ ? rt_effects_->shadow() : rt_shadow_dummy_;
    rhi::TextureHandle reflection = ray_traced_ ? rt_effects_->reflection() : rt_reflection_dummy_;
    if (ray_traced_ && denoise_) {
        shadow = denoise_shadow_->next_output();
        reflection = denoise_reflection_->next_output();
    }
    if (const RhiStatus written = device_.write_texture(lighting_set_, 3, 0, shadow); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(lighting_set_, 4, 0, reflection); !written.ok()) {
        return written;
    }
    // The shadow set never reads these, but every binding must hold a texture in the state its descriptor needs.
    if (const RhiStatus written = device_.write_texture(shadow_lighting_set_, 3, 0, rt_shadow_dummy_); !written.ok()) {
        return written;
    }
    return device_.write_texture(shadow_lighting_set_, 4, 0, rt_reflection_dummy_);
}

RhiStatus SceneRenderer::prepare_ray_traced(const FrameSnapshot& snapshot) {
    ray_traced_instances_ = 0;
    if (!ray_traced_) {
        return core::success();
    }
    // The shadow target is the sun's, and scene.hlsl applies it to every directional light, so the frame may hold one.
    std::optional<Light> sun;
    std::size_t directional = 0;
    for (const Light& light : snapshot.lights()) {
        if (light.type == LightType::Directional) {
            ++directional;
            sun = light;
        }
    }
    if (directional > 1) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "ray-traced shadows take one directional light: the shadow target is the sun's");
    }

    // The traced items are the static, non-blended ones. Skinned meshes and alpha-blended items stay on the raster path (see
    // rt_effects_geometry_support). A shadow-only item is traced too: it can cast onto the view from outside it.
    const auto traced = [this](const SnapshotItem& item) {
        const GpuMesh& mesh = gpu_meshes_[item.mesh];
        return mesh.bones == 0 && materials_[item.material].params.blend != BlendMode::AlphaBlend && !mesh.rt_triangles.empty();
    };
    for (const SnapshotItem& item : snapshot.items()) {
        if (!traced(item)) {
            continue;
        }
        const bool masked = materials_[item.material].params.blend == BlendMode::AlphaMask;
        if (!gpu_meshes_[item.mesh].blas[masked ? 1 : 0].valid()) {
            RhiResult<rhi::AccelerationStructureHandle> built = build_bottom_level(item.mesh, masked);
            if (!built.ok()) {
                return core::failure(built.error());
            }
            gpu_meshes_[item.mesh].blas[masked ? 1 : 0] = built.value();
        }
    }
    if (mask_atlas_dirty_) {
        if (const RhiStatus built = build_mask_atlas(); !built.ok()) {
            return built;
        }
    }

    RtScene scene;
    std::vector<rhi::AccelerationStructureHandle> structures;
    std::map<std::uint32_t, std::uint32_t> triangle_base;  // mesh -> its first triangle in scene.triangles; each mesh's block is added once
    for (const SnapshotItem& item : snapshot.items()) {
        if (!traced(item)) {
            continue;
        }
        const GpuMesh& mesh = gpu_meshes_[item.mesh];
        const GpuMaterial& material = materials_[item.material];
        const bool masked = material.params.blend == BlendMode::AlphaMask;
        const auto [base, inserted] = triangle_base.try_emplace(item.mesh, static_cast<std::uint32_t>(scene.triangles.size()));
        if (inserted) {
            scene.triangles.insert(scene.triangles.end(), mesh.rt_triangles.begin(), mesh.rt_triangles.end());
        }
        RtInstanceRecord record;
        record.transform = item.model;
        record.albedo = glm::vec3(material.params.base_color) * glm::vec3(item.color);
        record.metallic = material.params.metallic;
        record.roughness = material.params.roughness;
        record.triangle_base = base->second;
        record.triangle_count = static_cast<std::uint32_t>(mesh.rt_triangles.size());
        record.masked = masked;
        if (masked) {
            // The raster test is base alpha times the factor against the cutoff; the texel alone is compared here, so the cutoff is divided by it.
            record.mask_rect = mask_rects_[material.textures[0]];
            const float opacity = material.params.base_color.a * item.color.a;
            record.mask_cutoff = opacity > 0.0f ? std::min(1.0f, material.params.alpha_cutoff / opacity) : 1.0f;
        }
        scene.instances.push_back(record);
        structures.push_back(mesh.blas[masked ? 1 : 0]);
    }

    // A frame with no traced items still builds an empty top level, so the pass writes its targets.
    const auto count = static_cast<std::uint32_t>(scene.instances.size());
    if (count > top_level_capacity_) {
        const std::uint32_t capacity = std::max({count, 2 * top_level_capacity_, 16u});
        rhi::TopLevelDesc top;
        top.max_instances = capacity;
        top.allow_refit = false;
        top.debug_name = "ray-traced scene";
        RhiResult<rhi::AccelerationStructureHandle> created = device_.create_top_level(top);
        if (!created.ok()) {
            return core::failure(created.error());
        }
        RhiStatus first = core::success();
        destroy_into(device_, top_level_, first);
        top_level_ = created.value();
        top_level_capacity_ = capacity;
        if (!first.ok()) {
            return first;
        }
    }
    std::vector<rhi::TopLevelInstance> instances;
    instances.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        instances.push_back(make_top_level_instance(scene.instances[i], i, structures[i]));
    }
    if (const RhiStatus written = device_.write_top_level(top_level_, instances); !written.ok()) {
        return written;
    }
    scene.top_level = top_level_;
    scene.mask_atlas = mask_atlas_;
    if (const RhiStatus bound = rt_effects_->set_scene(scene); !bound.ok()) {
        return bound;
    }
    if (dynamic_gi_) {
        if (const RhiStatus bound = dynamic_gi_->set_scene(scene); !bound.ok()) {
            return bound;
        }
    }

    // The model matrices are relative to the camera, so the camera is at the origin of the traced scene.
    const Camera& camera = snapshot.camera();
    RtFrame frame;
    frame.eye = glm::vec3(0.0f);
    frame.forward = glm::vec3(camera.forward());
    frame.up = glm::vec3(camera.orientation * glm::dvec3(0.0, 1.0, 0.0));
    frame.fov_y_radians = static_cast<float>(camera.fov_y_radians);
    frame.aspect = static_cast<float>(desc_.width) / static_cast<float>(desc_.height);
    frame.sun_radiance = sun ? sun->color * sun->intensity : glm::vec3(0.0f);
    if (sun) {
        frame.sun_direction = -sun->direction;  // the light's direction is the way it travels; the shadow ray goes back toward the sun
    }
    frame.shadows = true;
    frame.reflections = true;
    frame.roughness_cutoff = desc_.ray_traced.roughness_cutoff;
    frame.reflection_distance = desc_.ray_traced.reflection_distance;
    frame.shadow_distance = desc_.ray_traced.shadow_distance;
    // The previous camera, relative to this one. The first frame has no previous camera, so it has no motion.
    frame.previous_eye = frame_history_ ? glm::vec3(previous_camera_position_ - camera.position) : frame.eye;
    frame.previous_forward = frame_history_ ? previous_camera_forward_ : frame.forward;
    frame.previous_up = frame_history_ ? previous_camera_up_ : frame.up;
    if (const RhiStatus updated = rt_effects_->update_frame(frame); !updated.ok()) {
        return updated;
    }
    if (dynamic_gi_) {
        // The grid follows the camera in world cells. The traced scene is camera-relative, so the pass places its probes relative to the
        // camera's world position, which the scene shader adds back when it samples.
        const glm::vec3 camera_world = glm::vec3(camera.position);
        gi::DynamicLighting lighting;
        lighting.sun_direction = frame.sun_direction;
        lighting.sun_radiance = frame.sun_radiance;
        lighting.sky_radiance = desc_.ray_traced.dynamic_sky_radiance;
        dynamic_gi_->grid().follow(camera_world);
        dynamic_gi_->grid().set_lighting(lighting);
        if (const RhiStatus updated = dynamic_gi_->update_frame(camera_world); !updated.ok()) {
            return updated;
        }
    }
    if (const RhiStatus updated = update_denoise_and_fog(snapshot, frame); !updated.ok()) {
        return updated;
    }
    ray_traced_instances_ = count;
    return core::success();
}

RhiStatus SceneRenderer::build_mask_atlas() {
    // The base-color textures of the alpha-mask materials, each once, in material order. They are stacked in one column, as wide as the widest.
    std::vector<std::uint32_t> ids;
    for (const GpuMaterial& material : materials_) {
        if (material.params.blend != BlendMode::AlphaMask) {
            continue;
        }
        const std::uint32_t id = material.textures[0];
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    mask_rects_.assign(textures_.size(), glm::vec4(0.0f));
    ImageRgba8 atlas;
    if (ids.empty()) {
        // No mask to read: one opaque texel, which the pass never reaches (no instance is masked).
        atlas.width = 1;
        atlas.height = 1;
        atlas.pixels = {0, 0, 0, 255};
    } else {
        atlas.width = 1;
        for (const std::uint32_t id : ids) {
            atlas.width = std::max(atlas.width, textures_[id].desc.width);
            atlas.height += textures_[id].desc.height;
        }
        atlas.pixels.assign(static_cast<std::size_t>(atlas.width) * atlas.height * 4, 0);
        std::uint32_t row = 0;
        for (const std::uint32_t id : ids) {
            const GpuTexture& texture = textures_[id];
            const std::uint32_t w = texture.desc.width;
            const std::uint32_t h = texture.desc.height;
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint8_t alpha = texture.alpha.size() == static_cast<std::size_t>(w) * h ? texture.alpha[static_cast<std::size_t>(y) * w + x] : 255;
                    atlas.pixels[(static_cast<std::size_t>(row + y) * atlas.width + x) * 4 + 3] = alpha;
                }
            }
            // The shader clamps the UV to [0, 1], so UV 1 must land on the last texel: the rectangle spans w - 1 texels (at least half a texel).
            const float span_x = std::max(static_cast<float>(w) - 1.0f, 0.5f);
            const float span_y = std::max(static_cast<float>(h) - 1.0f, 0.5f);
            mask_rects_[id] = glm::vec4(0.0f, static_cast<float>(row) / static_cast<float>(atlas.height), span_x / static_cast<float>(atlas.width),
                                        span_y / static_cast<float>(atlas.height));
            row += h;
        }
    }
    RhiResult<rhi::TextureHandle> created = make_texture(atlas.width, atlas.height, Format::R8G8B8A8Unorm, "mask atlas");
    if (!created.ok()) {
        return core::failure(created.error());
    }
    const rhi::TextureHandle texture = created.value();
    if (const RhiStatus uploaded = upload_texture_bytes(texture, atlas.width, atlas.height, Format::R8G8B8A8Unorm, pack_rgba8_rows(atlas)); !uploaded.ok()) {
        static_cast<void>(device_.destroy(texture));
        return uploaded;
    }
    RhiStatus first = core::success();
    destroy_into(device_, mask_atlas_, first);
    mask_atlas_ = texture;
    mask_atlas_dirty_ = false;
    return first;
}

RhiResult<rhi::AccelerationStructureHandle> SceneRenderer::build_bottom_level(std::uint32_t mesh, bool masked) {
    const GpuMesh& gpu = gpu_meshes_[mesh];
    rhi::BottomLevelDesc desc;
    desc.vertex_buffer = gpu.vertices;
    desc.vertex_stride = gpu.vertex_stride;
    desc.position_offset = 0;
    desc.vertex_count = gpu.vertex_count;
    desc.index_buffer = gpu.indices;
    desc.index_format = rhi::IndexFormat::Uint32;
    desc.index_count = gpu.index_count;
    desc.opaque = !masked;  // a masked mesh's candidates reach the alpha test in the pass (rt_effects.hlsl)
    desc.debug_name = masked ? "mesh bottom level (masked)" : "mesh bottom level";
    RhiResult<rhi::AccelerationStructureHandle> created = device_.create_bottom_level(desc);
    if (!created.ok()) {
        return core::failure(created.error());
    }
    const rhi::AccelerationStructureHandle structure = created.value();

    // The graph's accesses move the geometry to ShaderRead for the build, and back to Common, the state the draws leave it in.
    const std::uint64_t vertex_bytes = static_cast<std::uint64_t>(gpu.vertex_count) * gpu.vertex_stride;
    const std::uint64_t index_bytes = static_cast<std::uint64_t>(gpu.index_count) * sizeof(std::uint32_t);
    RenderGraph graph;
    const ResourceId vertex_res = graph.import_buffer("mesh vertices", gpu.vertices,
        buffer_desc(vertex_bytes, mesh_buffer_usage(BufferUsage::Vertex | BufferUsage::CopyDestination, true), "mesh vertices"),
        ResourceState::Common, ResourceState::Common);
    const ResourceId index_res = graph.import_buffer("mesh indices", gpu.indices,
        buffer_desc(index_bytes, mesh_buffer_usage(BufferUsage::Index | BufferUsage::CopyDestination, true), "mesh indices"),
        ResourceState::Common, ResourceState::Common);
    graph.add_pass("build bottom level", PassKind::Compute, {{vertex_res, Access::ShaderRead}, {index_res, Access::ShaderRead}},
                   [=](PassContext& ctx) { return ctx.commands().build_bottom_level(structure, rhi::BuildMode::Build); });
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        static_cast<void>(device_.destroy(structure));
        return rhi::fail<rhi::AccelerationStructureHandle>(RhiErrorCode::InvalidState, "build_bottom_level: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    if (!executed.ok()) {
        static_cast<void>(device_.destroy(structure));
        return core::failure(executed.error());
    }
    return core::success(structure);
}

void SceneRenderer::destroy_ray_traced(RhiStatus& first) {
    if (dynamic_gi_) {
        if (const RhiStatus released = dynamic_gi_->release(); !released.ok() && first.ok()) {
            first = released;
        }
        dynamic_gi_.reset();
    }
    destroy_into(device_, dynamic_history_dummy_, first);
    destroy_into(device_, dynamic_sampling_dummy_, first);
    if (rt_effects_) {
        if (const RhiStatus released = rt_effects_->release(); !released.ok() && first.ok()) {
            first = released;
        }
        rt_effects_.reset();
    }
    destroy_into(device_, top_level_, first);
    top_level_capacity_ = 0;
    for (GpuMesh& mesh : gpu_meshes_) {
        for (rhi::AccelerationStructureHandle& structure : mesh.blas) {
            destroy_into(device_, structure, first);
        }
    }
    destroy_into(device_, mask_atlas_, first);
    destroy_into(device_, rt_shadow_dummy_, first);
    destroy_into(device_, rt_reflection_dummy_, first);
    for (std::unique_ptr<TemporalDenoiser>* denoiser : {&denoise_shadow_, &denoise_reflection_}) {
        if (*denoiser) {
            if (const RhiStatus released = (*denoiser)->release(); !released.ok() && first.ok()) {
                first = released;
            }
            denoiser->reset();
        }
    }
    if (volumetric_fog_) {
        if (const RhiStatus released = volumetric_fog_->release(); !released.ok() && first.ok()) {
            first = released;
        }
        volumetric_fog_.reset();
    }
    destroy_into(device_, fogged_, first);
    ray_traced_ = false;
}

RhiStatus SceneRenderer::build_atlases() {
    // The cascade atlas and the local atlas each hold an R32Float depth-value target and a D32 depth buffer of their own size.
    const TextureUsage tile_usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    struct Atlas {
        std::uint32_t size;
        rhi::TextureHandle* color;
        rhi::TextureHandle* depth;
        const char* name;
    };
    const std::array<Atlas, 2> atlases{{{desc_.shadows.cascade_atlas_size, &cascade_color_, &cascade_depth_, "cascade atlas"},
                                        {desc_.shadows.local_atlas_size, &local_color_, &local_depth_, "local shadow atlas"}}};
    for (const Atlas& atlas : atlases) {
        RhiResult<rhi::TextureHandle> color = device_.create_texture(texture_desc(atlas.size, atlas.size, Format::R32Float, tile_usage, atlas.name));
        if (!color.ok()) {
            return core::failure(color.error());
        }
        *atlas.color = std::move(color).value();
        RhiResult<rhi::TextureHandle> depth = device_.create_texture(texture_desc(atlas.size, atlas.size, Format::D32Float, TextureUsage::DepthStencil,
                                                                                   atlas.name));
        if (!depth.ok()) {
            return core::failure(depth.error());
        }
        *atlas.depth = std::move(depth).value();
    }

    // Clear the color atlases to the far depth, so an unrendered texel reads as lit, and leave them in ShaderRead for the scene pass.
    RenderGraph graph;
    std::array<ResourceId, 2> colors{};
    for (std::size_t i = 0; i < atlases.size(); ++i) {
        const Atlas& atlas = atlases[i];
        colors[i] = graph.import_texture(atlas.name, *atlas.color, texture_desc(atlas.size, atlas.size, Format::R32Float, tile_usage, atlas.name),
                                         ResourceState::Common, ResourceState::ShaderRead);
        graph.add_pass(std::string("clear ") + atlas.name, PassKind::Graphics, {{colors[i], Access::ColorWrite}}, [=](PassContext& ctx) {
            rhi::RenderPassDesc pass;
            pass.color_targets = {ctx.texture(colors[i])};
            pass.clear_color = std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
            rhi::CommandList& commands = ctx.commands();
            if (const RhiStatus status = commands.begin_render_pass(pass); !status.ok()) {
                return status;
            }
            return commands.end_render_pass();
        });
    }
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        return rhi::fail(RhiErrorCode::InvalidState, "build_atlases: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    if (!executed.ok()) {
        return core::failure(executed.error());
    }
    return core::success();
}

RhiStatus SceneRenderer::ensure_slot(std::size_t slot, std::uint64_t bytes) {
    if (slots_.size() <= slot) {
        slots_.resize(slot + 1);
    }
    FrameSlot& frame_slot = slots_[slot];
    if (frame_slot.capacity >= bytes) {
        return core::success();
    }
    // Grow by doubling, so a frame that gets a little bigger each time does not reallocate every time.
    const std::uint64_t capacity = std::max(bytes, frame_slot.capacity * 2);
    RhiStatus first = core::success();
    destroy_slot(frame_slot, first);
    if (!first.ok()) {
        return first;
    }

    RhiResult<BufferHandle> frame = device_.create_buffer(buffer_desc(capacity, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "scene frame"));
    if (!frame.ok()) {
        return core::failure(frame.error());
    }
    frame_slot.frame = std::move(frame).value();
    RhiResult<BufferHandle> upload = device_.create_buffer(buffer_desc(capacity, BufferUsage::CopySource | BufferUsage::CpuWrite, "scene frame upload"));
    if (!upload.ok()) {
        destroy_slot(frame_slot, first);
        return core::failure(upload.error());
    }
    frame_slot.upload = std::move(upload).value();
    RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(frame_layout_);
    if (!set.ok()) {
        destroy_slot(frame_slot, first);
        return core::failure(set.error());
    }
    frame_slot.set = std::move(set).value();
    if (const RhiStatus written = device_.write_buffer(frame_slot.set, 0, 0, frame_slot.frame); !written.ok()) {
        destroy_slot(frame_slot, first);
        return written;
    }
    // The meshlet set binds the same frame buffer, and the pools and counters, for the mesh stages (US-038).
    if (meshlets_ready_) {
        RhiResult<rhi::DescriptorSetHandle> meshlet_set = device_.create_descriptor_set(meshlet_layout_);
        if (!meshlet_set.ok()) {
            destroy_slot(frame_slot, first);
            return core::failure(meshlet_set.error());
        }
        frame_slot.meshlet_set = std::move(meshlet_set).value();
        const std::array<std::pair<std::uint32_t, rhi::BufferHandle>, 6> bindings{{{0, frame_slot.frame},
                                                                                  {1, meshlet_records_},
                                                                                  {2, meshlet_vertices_},
                                                                                  {3, meshlet_triangles_},
                                                                                  {4, mesh_vertex_pool_},
                                                                                  {5, meshlet_counts_}}};
        for (const auto& [binding, buffer] : bindings) {
            if (const RhiStatus written = device_.write_buffer(frame_slot.meshlet_set, binding, 0, buffer); !written.ok()) {
                destroy_slot(frame_slot, first);
                return written;
            }
        }
    }
    frame_slot.capacity = capacity;
    return core::success();
}

void SceneRenderer::destroy_slot(FrameSlot& slot, rhi::RhiStatus& first) {
    destroy_into(device_, slot.meshlet_set, first);
    destroy_into(device_, slot.set, first);
    destroy_into(device_, slot.upload, first);
    destroy_into(device_, slot.frame, first);
    slot.capacity = 0;
}

RhiResult<rhi::TextureHandle> SceneRenderer::make_texture(std::uint32_t width, std::uint32_t height, Format format, const char* name) {
    return device_.create_texture(texture_desc(width, height, format, TextureUsage::ShaderResource | TextureUsage::CopyDestination, name));
}

RhiStatus SceneRenderer::upload_texture_bytes(rhi::TextureHandle texture, std::uint32_t width, std::uint32_t height, Format format,
                                              const std::vector<std::uint8_t>& bytes) {
    RhiResult<BufferHandle> staging = device_.create_buffer(buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "texture staging"));
    if (!staging.ok()) {
        return core::failure(staging.error());
    }
    const BufferHandle staging_buffer = std::move(staging).value();
    if (const RhiStatus uploaded = device_.upload_buffer(staging_buffer, 0, bytes.size(), bytes.data()); !uploaded.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return uploaded;
    }

    // The texture starts in Common and stays in ShaderRead, the state every later read expects.
    RenderGraph graph;
    const ResourceId staging_res = graph.import_buffer("texture staging", staging_buffer,
        buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "texture staging"), ResourceState::CopySource, ResourceState::CopySource);
    const ResourceId texture_res = graph.import_texture("texture", texture,
        texture_desc(width, height, format, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "texture"), ResourceState::Common,
        ResourceState::ShaderRead);
    graph.add_pass("upload texture", PassKind::Graphics, {{staging_res, Access::CopySource}, {texture_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer_to_texture(ctx.buffer(staging_res), 0, ctx.texture(texture_res)); });
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return rhi::fail(RhiErrorCode::InvalidState, "upload_texture: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    static_cast<void>(device_.destroy(staging_buffer));
    if (!executed.ok()) {
        return core::failure(executed.error());
    }
    return core::success();
}

RhiStatus SceneRenderer::upload_buffer_bytes(BufferHandle buffer, std::uint64_t size, const std::vector<std::uint8_t>& bytes, BufferUsage usage,
                                             ResourceState after, ResourceState before) {
    // Only the bytes given are copied, so a pool of fixed size can take a prefix of its data (US-038).
    const std::uint64_t copy = bytes.size();
    if (copy == 0 || copy > size) {
        return rhi::fail(RhiErrorCode::OutOfRange, "upload_buffer: the data must be non-empty and fit the buffer");
    }
    RhiResult<BufferHandle> staging = device_.create_buffer(buffer_desc(copy, BufferUsage::CopySource | BufferUsage::CpuWrite, "buffer staging"));
    if (!staging.ok()) {
        return core::failure(staging.error());
    }
    const BufferHandle staging_buffer = std::move(staging).value();
    if (const RhiStatus uploaded = device_.upload_buffer(staging_buffer, 0, copy, bytes.data()); !uploaded.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return uploaded;
    }
    RenderGraph graph;
    const ResourceId staging_res = graph.import_buffer("buffer staging", staging_buffer,
        buffer_desc(copy, BufferUsage::CopySource | BufferUsage::CpuWrite, "buffer staging"), ResourceState::CopySource, ResourceState::CopySource);
    const ResourceId buffer_res = graph.import_buffer("buffer", buffer, buffer_desc(size, usage, "buffer"), before, after);
    graph.add_pass("upload buffer", PassKind::Graphics, {{staging_res, Access::CopySource}, {buffer_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer(ctx.buffer(staging_res), 0, ctx.buffer(buffer_res), 0, copy); });
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return rhi::fail(RhiErrorCode::InvalidState, "upload_buffer: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    static_cast<void>(device_.destroy(staging_buffer));
    if (!executed.ok()) {
        return core::failure(executed.error());
    }
    return core::success();
}

RhiResult<std::uint32_t> SceneRenderer::upload_mesh(const MeshData& mesh) {
    return upload_geometry(mesh, nullptr);
}

RhiResult<std::uint32_t> SceneRenderer::upload_terrain_mesh(const MeshData& mesh, const std::vector<glm::vec4>& splat) {
    if (!terrain_ready_) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidState, "upload_terrain_mesh: a terrain mesh needs the terrain shader entries (SceneShaders::terrain_vertex and terrain_shadow_vertex)");
    }
    if (!mesh.joints.empty()) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidDescriptor, "upload_terrain_mesh: a terrain mesh cannot be skinned");
    }
    if (splat.size() != mesh.positions.size()) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidDescriptor, "upload_terrain_mesh: the splat needs one weight per vertex");
    }
    return upload_geometry(mesh, &splat);
}

// The upload behind upload_mesh and upload_terrain_mesh. A non-null `splat` makes a terrain mesh: the terrain stride, with the weights.
RhiResult<std::uint32_t> SceneRenderer::upload_geometry(const MeshData& mesh, const std::vector<glm::vec4>* splat) {
    if (const std::optional<std::string> problem = validate_mesh(mesh)) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidDescriptor, "upload_mesh: " + *problem);
    }
    const bool terrain = splat != nullptr;
    const std::uint32_t stride = terrain ? kTerrainVertexStride : (mesh.joints.empty() ? kVertexStride : kSkinnedVertexStride);
    if (!mesh.joints.empty() && !skinned_ready_) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidState, "upload_mesh: a skinned mesh needs the skinned shader entries (SceneShaders::skinned_vertex)");
    }
    const std::uint64_t vertex_bytes = mesh.positions.size() * stride;
    const std::uint64_t index_bytes = mesh.indices.size() * sizeof(std::uint32_t);

    // Vertices and indices are copied through one staging buffer and then into device-local buffers in one graph execution.
    RhiResult<BufferHandle> staging = device_.create_buffer(buffer_desc(vertex_bytes + index_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "mesh staging"));
    if (!staging.ok()) {
        return core::failure(staging.error());
    }
    const BufferHandle staging_buffer = std::move(staging).value();
    std::vector<std::uint8_t> bytes = terrain ? pack_terrain_vertices(mesh, *splat) : pack_vertices(mesh, stride);
    bytes.resize(vertex_bytes + index_bytes);
    std::memcpy(bytes.data() + vertex_bytes, mesh.indices.data(), index_bytes);
    if (const RhiStatus uploaded = device_.upload_buffer(staging_buffer, 0, bytes.size(), bytes.data()); !uploaded.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return core::failure(uploaded.error());
    }
    const BufferUsage vertex_usage = mesh_buffer_usage(BufferUsage::Vertex | BufferUsage::CopyDestination, ray_traced_);
    const BufferUsage index_usage = mesh_buffer_usage(BufferUsage::Index | BufferUsage::CopyDestination, ray_traced_);
    RhiResult<BufferHandle> vertices = device_.create_buffer(buffer_desc(vertex_bytes, vertex_usage, "mesh vertices"));
    RhiResult<BufferHandle> indices = device_.create_buffer(buffer_desc(index_bytes, index_usage, "mesh indices"));
    if (!vertices.ok() || !indices.ok()) {
        const rhi::RhiError error = vertices.ok() ? indices.error() : vertices.error();
        if (vertices.ok()) {
            static_cast<void>(device_.destroy(vertices.value()));
        }
        if (indices.ok()) {
            static_cast<void>(device_.destroy(indices.value()));
        }
        static_cast<void>(device_.destroy(staging_buffer));
        return core::failure(error);
    }
    GpuMesh gpu;
    gpu.vertices = vertices.value();
    gpu.indices = indices.value();
    gpu.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    gpu.vertex_stride = stride;
    gpu.terrain = terrain;
    gpu.bones = skin_joint_count(mesh);
    gpu.vertex_count = static_cast<std::uint32_t>(mesh.positions.size());
    if (ray_traced_) {
        gpu.rt_triangles = ray_traced_triangles(mesh);
    }

    RenderGraph graph;
    const ResourceId staging_res = graph.import_buffer("mesh staging", staging_buffer,
        buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "mesh staging"), ResourceState::CopySource, ResourceState::CopySource);
    const ResourceId vertex_res = graph.import_buffer("mesh vertices", gpu.vertices,
        buffer_desc(vertex_bytes, vertex_usage, "mesh vertices"), ResourceState::Common, ResourceState::Common);
    const ResourceId index_res = graph.import_buffer("mesh indices", gpu.indices,
        buffer_desc(index_bytes, index_usage, "mesh indices"), ResourceState::Common, ResourceState::Common);
    graph.add_pass("upload mesh", PassKind::Graphics,
                   {{staging_res, Access::CopySource}, {vertex_res, Access::CopyDestination}, {index_res, Access::CopyDestination}},
                   [=](PassContext& ctx) {
                       const RhiStatus copied_vertices = ctx.commands().copy_buffer(ctx.buffer(staging_res), 0, ctx.buffer(vertex_res), 0, vertex_bytes);
                       if (!copied_vertices.ok()) {
                           return copied_vertices;
                       }
                       return ctx.commands().copy_buffer(ctx.buffer(staging_res), vertex_bytes, ctx.buffer(index_res), 0, index_bytes);
                   });
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        static_cast<void>(device_.destroy(gpu.vertices));
        static_cast<void>(device_.destroy(gpu.indices));
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidState, "upload_mesh: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    static_cast<void>(device_.destroy(staging_buffer));
    if (!executed.ok()) {
        static_cast<void>(device_.destroy(gpu.vertices));
        static_cast<void>(device_.destroy(gpu.indices));
        return core::failure(executed.error());
    }

    gpu_meshes_.push_back(gpu);
    meshes_.push_back(mesh);
    bounds_.push_back(compute_bounds(mesh));
    return core::success(static_cast<std::uint32_t>(gpu_meshes_.size() - 1));
}

RhiStatus SceneRenderer::set_mesh_bounds(std::uint32_t mesh, const MeshBounds& bounds) {
    if (mesh >= bounds_.size()) {
        return rhi::fail(RhiErrorCode::OutOfRange, "set_mesh_bounds: no mesh has that index");
    }
    if (!std::isfinite(bounds.center.x) || !std::isfinite(bounds.center.y) || !std::isfinite(bounds.center.z) || !std::isfinite(bounds.radius) ||
        bounds.radius < 0.0f) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "set_mesh_bounds: the bounds must be finite with a non-negative radius");
    }
    bounds_[mesh] = bounds;
    return core::success();
}

RhiResult<std::uint32_t> SceneRenderer::upload_texture(const ImageRgba8& image) {
    if (const std::optional<std::string> problem = validate_image(image)) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidDescriptor, "upload_texture: " + *problem);
    }
    RhiResult<rhi::TextureHandle> created = make_texture(image.width, image.height, Format::R8G8B8A8Unorm, "material texture");
    if (!created.ok()) {
        return core::failure(created.error());
    }
    const rhi::TextureHandle texture = std::move(created).value();
    if (const RhiStatus uploaded = upload_texture_bytes(texture, image.width, image.height, Format::R8G8B8A8Unorm, pack_rgba8_rows(image)); !uploaded.ok()) {
        static_cast<void>(device_.destroy(texture));
        return core::failure(uploaded.error());
    }
    // The ray-traced mask atlas (US-043) needs each texel's alpha, so the alpha is kept while the pass is on.
    std::vector<std::uint8_t> alpha;
    if (ray_traced_) {
        alpha.resize(static_cast<std::size_t>(image.width) * image.height);
        for (std::size_t texel = 0; texel < alpha.size(); ++texel) {
            alpha[texel] = image.pixels[texel * 4 + 3];
        }
    }
    textures_.push_back(GpuTexture{texture, texture_desc(image.width, image.height, Format::R8G8B8A8Unorm,
                                                          TextureUsage::ShaderResource | TextureUsage::CopyDestination, "material texture"),
                                   std::move(alpha)});
    mask_atlas_dirty_ = true;
    return core::success(static_cast<std::uint32_t>(textures_.size() - 1));
}

RhiResult<std::uint32_t> SceneRenderer::create_material(const MaterialParams& params, const MaterialTextures& textures) {
    if (const std::optional<std::string> problem = validate_material(params)) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::InvalidDescriptor, "create_material: " + *problem);
    }
    GpuMaterial material;
    material.params = params;
    material.textures = {slot_or(textures.base_color, kWhiteTexture), slot_or(textures.metallic_roughness, kWhiteTexture),
                         slot_or(textures.normal, kFlatNormalTexture), slot_or(textures.emissive, kWhiteTexture),
                         slot_or(textures.occlusion, kWhiteTexture)};
    for (const std::uint32_t id : material.textures) {
        if (id >= textures_.size()) {
            return rhi::fail<std::uint32_t>(RhiErrorCode::OutOfRange, "create_material: a texture id names no uploaded texture");
        }
    }

    RhiResult<BufferHandle> parameters = device_.create_buffer(buffer_desc(kMaterialBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "material parameters"));
    if (!parameters.ok()) {
        return core::failure(parameters.error());
    }
    material.parameters = std::move(parameters).value();
    RhiStatus first = upload_buffer_bytes(material.parameters, kMaterialBytes, pack_material(params));
    if (!first.ok()) {
        destroy_into(device_, material.parameters, first);
        return core::failure(first.error());
    }
    RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(material_layout_);
    if (!set.ok()) {
        destroy_into(device_, material.parameters, first);
        return core::failure(set.error());
    }
    material.set = std::move(set).value();
    first = device_.write_buffer(material.set, 0, 0, material.parameters);
    for (std::size_t slot = 0; first.ok() && slot < kMaterialTextureSlots; ++slot) {
        first = device_.write_texture(material.set, static_cast<std::uint32_t>(slot + 1), 0, textures_[material.textures[slot]].texture);
    }
    if (!first.ok()) {
        destroy_into(device_, material.set, first);
        destroy_into(device_, material.parameters, first);
        return core::failure(first.error());
    }
    materials_.push_back(material);
    mask_atlas_dirty_ = true;
    return core::success(static_cast<std::uint32_t>(materials_.size() - 1));
}

RhiResult<std::uint32_t> SceneRenderer::create_material_instance(std::uint32_t parent, const MaterialOverrides& overrides) {
    if (parent >= materials_.size()) {
        return rhi::fail<std::uint32_t>(RhiErrorCode::OutOfRange, "create_material_instance: the parent material does not exist");
    }
    const GpuMaterial& source = materials_[parent];
    MaterialTextures textures;
    textures.base_color = source.textures[0];
    textures.metallic_roughness = source.textures[1];
    textures.normal = source.textures[2];
    textures.emissive = source.textures[3];
    textures.occlusion = source.textures[4];
    return create_material(resolve(source.params, overrides), textures);
}

RhiStatus SceneRenderer::set_environment(const EquirectImage& radiance) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_environment: the renderer was released");
    }
    if (const std::optional<std::string> problem = validate_equirect(radiance)) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "set_environment: " + *problem);
    }
    const Environment environment = build_environment(radiance);

    // The new resources are complete before the set points at them, and the old ones are destroyed only after that.
    RhiStatus first = core::success();
    BufferHandle buffer;
    std::array<rhi::TextureHandle, kEnvironmentTextures> textures{};
    const auto discard_new = [&]() {
        RhiStatus ignored = core::success();
        destroy_into(device_, buffer, ignored);
        for (rhi::TextureHandle& texture : textures) {
            destroy_into(device_, texture, ignored);
        }
    };

    RhiResult<BufferHandle> created = device_.create_buffer(buffer_desc(kEnvironmentBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "environment"));
    if (!created.ok()) {
        return core::failure(created.error());
    }
    buffer = std::move(created).value();
    if (first = upload_buffer_bytes(buffer, kEnvironmentBytes, pack_environment(environment)); !first.ok()) {
        discard_new();
        return first;
    }
    for (std::size_t i = 0; i < kEnvironmentTextures; ++i) {
        const TextureDesc desc = environment_texture_desc(environment, i);
        RhiResult<rhi::TextureHandle> texture = make_texture(desc.width, desc.height, desc.format, desc.debug_name.c_str());
        if (!texture.ok()) {
            discard_new();
            return core::failure(texture.error());
        }
        textures[i] = std::move(texture).value();
        std::vector<std::uint8_t> bytes;
        if (i < environment.specular.size()) {
            const EquirectImage& level = environment.specular[i];
            bytes = pack_half_rows(level.width, level.height, [&level](std::uint32_t x, std::uint32_t y) {
                const glm::vec3& value = level.texels[static_cast<std::size_t>(y) * level.width + x];
                return glm::vec4(value, 1.0f);
            });
        } else {
            bytes = pack_half_rows(environment.lut_size, environment.lut_size, [&environment](std::uint32_t x, std::uint32_t y) {
                const glm::vec2& value = environment.brdf_lut[static_cast<std::size_t>(y) * environment.lut_size + x];
                return glm::vec4(value.x, value.y, 0.0f, 1.0f);
            });
        }
        if (first = upload_texture_bytes(textures[i], desc.width, desc.height, desc.format, bytes); !first.ok()) {
            discard_new();
            return first;
        }
    }

    if (!environment_set_.valid()) {
        RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(environment_layout_);
        if (!set.ok()) {
            discard_new();
            return core::failure(set.error());
        }
        environment_set_ = std::move(set).value();
    }
    first = device_.write_buffer(environment_set_, 0, 0, buffer);
    for (std::uint32_t i = 0; first.ok() && i < kEnvironmentTextures; ++i) {
        first = device_.write_texture(environment_set_, i + 1, 0, textures[i]);
    }
    if (!first.ok()) {
        discard_new();
        return first;
    }

    RhiStatus retired = core::success();
    destroy_into(device_, environment_buffer_, retired);
    for (rhi::TextureHandle& texture : environment_textures_) {
        destroy_into(device_, texture, retired);
    }
    environment_buffer_ = buffer;
    for (std::size_t i = 0; i < kEnvironmentTextures; ++i) {
        environment_textures_[i] = textures[i];
    }
    environment_ = environment;
    return retired;
}

void SceneRenderer::invalidate_shadows(std::uint32_t light_id) {
    for (std::uint32_t i = 0; i < kMaxCascades; ++i) {
        cascade_atlas_.invalidate(light_id * kMaxCascades + i);
    }
    local_atlas_.invalidate(light_id * kMaxCascades);
}

void SceneRenderer::invalidate_all_shadows() {
    cascade_atlas_.invalidate_all();
    local_atlas_.invalidate_all();
}

RhiStatus SceneRenderer::render(const FrameSnapshot& snapshot) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "render: the renderer was released");
    }
    if (snapshot.items().size() > desc_.max_items) {
        return rhi::fail(RhiErrorCode::OutOfRange, "render: the snapshot holds more items than max_items");
    }
    for (const SnapshotItem& item : snapshot.items()) {
        if (item.mesh >= gpu_meshes_.size()) {
            return rhi::fail(RhiErrorCode::OutOfRange, "render: a snapshot item names a mesh that was not uploaded");
        }
        if (item.material >= materials_.size()) {
            return rhi::fail(RhiErrorCode::OutOfRange, "render: a snapshot item names a material that does not exist");
        }
        const GpuMesh& gpu = gpu_meshes_[item.mesh];
        if (gpu.bones > 0) {
            if (!skinned_ready_) {
                return rhi::fail(RhiErrorCode::InvalidState, "render: a skinned mesh needs the skinned shader entries (SceneShaders::skinned_vertex)");
            }
            if (item.bone_count != 0 && item.bone_count < gpu.bones) {
                return rhi::fail(RhiErrorCode::OutOfRange, "render: a skin pose has fewer joints than its mesh uses");
            }
        } else if (item.bone_count != 0) {
            return rhi::fail(RhiErrorCode::InvalidDescriptor, "render: a skin pose was given for a mesh without skin weights");
        }
    }
    // The bones each mesh's items need in their records: the mesh's joint count for a skinned mesh, zero for a static one.
    std::vector<std::uint32_t> mesh_bones(gpu_meshes_.size());
    for (std::size_t m = 0; m < gpu_meshes_.size(); ++m) {
        mesh_bones[m] = gpu_meshes_[m].bones;
    }

    // Lights: each must be valid, and a shadow-casting light needs an id no other shadow-casting light uses. Shadow tiles are keyed by id.
    const std::vector<Light>& lights = snapshot.lights();
    std::vector<std::uint32_t> shadow_ids;
    for (const Light& light : lights) {
        if (const std::optional<std::string> problem = validate_light(light)) {
            return rhi::fail(RhiErrorCode::InvalidDescriptor, "render: " + *problem);
        }
        if (light.casts_shadow && light.type != LightType::Point) {
            if (light.id >= (1u << 28)) {
                return rhi::fail(RhiErrorCode::OutOfRange, "render: a shadow-casting light id must be below 2^28");
            }
            shadow_ids.push_back(light.id);
        }
    }
    std::sort(shadow_ids.begin(), shadow_ids.end());
    if (std::adjacent_find(shadow_ids.begin(), shadow_ids.end()) != shadow_ids.end()) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "render: two shadow-casting lights share an id");
    }
    const Camera& camera = snapshot.camera();
    const std::optional<ClusterGrid> grid = make_cluster_grid(desc_.lighting, camera, desc_.width, desc_.height);
    if (!grid) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "render: the lighting settings or the camera cannot form a cluster grid");
    }

    // Lights in priority order, highest first; equal priorities keep the author's order. Past max_lights, the lowest priorities drop.
    std::vector<std::size_t> order(lights.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&lights](std::size_t a, std::size_t b) { return lights[a].priority > lights[b].priority; });
    const std::size_t kept = std::min<std::size_t>(lights.size(), desc_.lighting.max_lights);
    order.resize(kept);

    // The frame's matrices. The snapshot's are in DX convention; Vulkan reads the flipped matrix (docs/runtime-conventions.md). Shadow
    // matrices follow the same rule, and their tile rows follow the backend's sign (shadow_atlas.hpp).
    const bool vulkan = device_.backend() == rhi::Backend::Vulkan;
    const glm::mat4 dx_view_projection = snapshot.view_projection();
    const glm::mat4 view_projection = vulkan ? core::coords::to_vulkan_clip(dx_view_projection) : dx_view_projection;

    // Shadow maps: one per cascade of a directional light, and one per spot light. Point lights light the scene without a shadow.
    struct ShadowPlan {
        std::uint32_t id = 0;
        std::uint32_t kind = 0;  // 0 cascade, 1 spot
        bool cascade_atlas = true;
        std::uint32_t tile_size = 0;
        glm::mat4 light{1.0f};  // camera-relative, DX-convention clip
        float split_far = 0.0f;
        float texel_world = 0.0f;  // world size of one texel of the map, at the tile's far edge
        glm::vec3 anchor{0.0f};    // cascade: the direction toward the light; spot: the light's camera-relative position
        float depth_bias = 0.0f;
        float light_bias = 0.0f;
        std::uint32_t pcf_radius = 0;
        bool granted = false;
        bool render = false;
        AtlasRect rect;
        std::vector<const SnapshotItem*> casters;
    };
    std::vector<ShadowPlan> plans;
    std::uint32_t plans_dropped = 0;
    std::vector<std::uint32_t> light_first(kept, kNoShadowIndex);
    std::vector<std::uint32_t> light_count(kept, 0);
    const auto add_plan = [&](ShadowPlan plan, std::size_t slot) {
        if (plans.size() >= desc_.max_shadow_maps) {
            ++plans_dropped;
            return;
        }
        if (light_count[slot] == 0) {
            light_first[slot] = static_cast<std::uint32_t>(plans.size());
        }
        ++light_count[slot];
        plans.push_back(std::move(plan));
    };
    for (std::size_t slot = 0; slot < kept; ++slot) {
        const Light& light = lights[order[slot]];
        if (!light.casts_shadow) {
            continue;
        }
        if (light.type == LightType::Directional) {
            const std::vector<CascadePlan> cascades = plan_cascades(camera, light.direction, desc_.cascades, desc_.shadows.cascade_size);
            for (std::uint32_t i = 0; i < cascades.size(); ++i) {
                ShadowPlan plan;
                plan.id = light.id * kMaxCascades + i;
                plan.kind = 0;
                plan.cascade_atlas = true;
                plan.tile_size = desc_.shadows.cascade_size;
                plan.light = cascades[i].view_projection;
                plan.split_far = cascades[i].split_far;
                // The cascade's light box spans 2 * radius, and its tile holds cascade_size texels across it.
                plan.texel_world = 2.0f * cascades[i].radius / static_cast<float>(desc_.shadows.cascade_size);
                plan.anchor = -light.direction;
                plan.depth_bias = light.depth_bias;
                plan.light_bias = light.light_bias;
                plan.pcf_radius = light.pcf_radius;
                add_plan(std::move(plan), slot);
            }
        } else if (light.type == LightType::Spot) {
            ShadowPlan plan;
            plan.id = light.id * kMaxCascades;
            plan.kind = 1;
            plan.cascade_atlas = false;
            plan.tile_size = desc_.shadows.local_size;
            plan.light = spot_view_projection(glm::vec3(light.position - camera.position), light.direction, light.outer_angle, light.range);
            // The cone's width at its range, over the tile: the largest texel the map has.
            plan.texel_world = 2.0f * light.range * std::tan(light.outer_angle) / static_cast<float>(desc_.shadows.local_size);
            plan.anchor = glm::vec3(light.position - camera.position);
            plan.depth_bias = light.depth_bias;
            plan.light_bias = light.light_bias;
            plan.pcf_radius = light.pcf_radius;
            add_plan(std::move(plan), slot);
        }
    }

    // Casters of each shadow map: the items whose bounding sphere reaches its frustum. Transparent items cast nothing. The key of a tile is
    // a hash of its matrix, its tuning, and its casters, so the tile is redrawn when any of those change.
    cascade_atlas_.begin_frame();
    local_atlas_.begin_frame();
    for (ShadowPlan& plan : plans) {
        const Frustum frustum = make_frustum(plan.light);
        for (const SnapshotItem& item : snapshot.items()) {
            if (materials_[item.material].params.blend == BlendMode::AlphaBlend) {
                continue;
            }
            const MeshBounds& bounds = bounds_[item.mesh];
            const glm::vec3 center = glm::vec3(item.model * glm::vec4(bounds.center, 1.0f));
            const float scale = std::max({glm::length(glm::vec3(item.model[0])), glm::length(glm::vec3(item.model[1])),
                                          glm::length(glm::vec3(item.model[2]))});
            if (sphere_visible(frustum, center, bounds.radius * scale)) {
                plan.casters.push_back(&item);
            }
        }
        std::uint64_t key = hash_bytes(&plan.light[0][0], sizeof(glm::mat4), kFnvBasis);
        const float tuning[] = {plan.depth_bias, plan.light_bias, static_cast<float>(plan.pcf_radius), plan.split_far,
                                static_cast<float>(plan.tile_size), static_cast<float>(plan.kind)};
        key = hash_bytes(tuning, sizeof(tuning), key);
        for (const SnapshotItem* item : plan.casters) {
            key = hash_bytes(&item->model[0][0], sizeof(glm::mat4), key);
            key = hash_bytes(&item->mesh, sizeof(item->mesh), key);
            if (item->bone_count != 0) {
                key = hash_bytes(&snapshot.palettes()[item->palette], item->bone_count * sizeof(glm::mat4), key);
            }
        }
        ShadowAtlas& atlas = plan.cascade_atlas ? cascade_atlas_ : local_atlas_;
        const ShadowTileRequest request = atlas.request(plan.id, plan.tile_size, key);
        plan.granted = request.granted;
        plan.rect = request.rect;
        plan.render = request.granted && request.render;
    }
    cascade_atlas_.end_frame();
    local_atlas_.end_frame();

    // Clusters. Each kept light goes to the clusters its sphere reaches; the list is in priority order, so a full cluster keeps the first.
    std::vector<ClusteredLight> clustered;
    clustered.reserve(kept);
    for (std::size_t slot = 0; slot < kept; ++slot) {
        const Light& light = lights[order[slot]];
        ClusteredLight entry;
        entry.index = static_cast<std::uint32_t>(slot);
        entry.type = light.type;
        if (light.type != LightType::Directional) {
            entry.center = glm::vec3(light.position - camera.position);
            entry.radius = light.range;
        }
        clustered.push_back(entry);
    }
    const ClusterAssignment assignment = assign_clusters(*grid, dx_view_projection, clustered);

    // The lighting buffer: header, light records, shadow records, cluster counts, and cluster indices.
    const LightingLayout& layout = lighting_layout_offsets_;
    std::vector<std::uint8_t> lighting(layout.bytes, 0);
    const std::uint32_t atlas_cascade = desc_.shadows.cascade_atlas_size;
    const std::uint32_t atlas_local = desc_.shadows.local_atlas_size;
    put_u32(lighting, 0, grid->tiles_x);
    put_u32(lighting, 4, grid->tiles_y);
    put_u32(lighting, 8, grid->slices);
    put_u32(lighting, 12, grid->max_lights_per_cluster);
    put_f32(lighting, 16, static_cast<float>(grid->tile_px));
    put_f32(lighting, 20, grid->near_plane);
    put_f32(lighting, 24, grid->far_plane);
    put_f32(lighting, 28, static_cast<float>(grid->slices) / std::log(grid->far_plane / grid->near_plane));
    const float forward[3] = {grid->forward.x, grid->forward.y, grid->forward.z};
    put_floats(lighting, 32, forward, 3);
    put_u32(lighting, 48, static_cast<std::uint32_t>(kept));
    put_u32(lighting, 52, static_cast<std::uint32_t>(plans.size()));
    put_u32(lighting, 56, static_cast<std::uint32_t>(view_));
    put_f32(lighting, 60, tile_row_sign(vulkan));
    put_u32(lighting, 64, static_cast<std::uint32_t>(layout.light_base));
    put_u32(lighting, 68, static_cast<std::uint32_t>(layout.shadow_base));
    put_u32(lighting, 72, static_cast<std::uint32_t>(layout.count_base));
    put_u32(lighting, 76, static_cast<std::uint32_t>(layout.index_base));
    put_f32(lighting, 80, static_cast<float>(atlas_cascade));
    put_f32(lighting, 84, static_cast<float>(atlas_local));

    for (std::size_t slot = 0; slot < kept; ++slot) {
        const Light& light = lights[order[slot]];
        const std::uint64_t base = layout.light_base + kLightRecordBytes * slot;
        const bool positional = light.type != LightType::Directional;
        const glm::vec3 position = positional ? glm::vec3(light.position - camera.position) : glm::vec3(0.0f);
        const float values[4] = {position.x, position.y, position.z, positional ? light.range : 0.0f};
        put_floats(lighting, base, values, 4);
        const float direction[4] = {light.direction.x, light.direction.y, light.direction.z, 0.0f};
        put_floats(lighting, base + 16, direction, 4);
        put_u32(lighting, base + 28, static_cast<std::uint32_t>(light.type));
        const float color[4] = {light.color.r, light.color.g, light.color.b, light.intensity};
        put_floats(lighting, base + 32, color, 4);
        const bool spot = light.type == LightType::Spot;
        put_f32(lighting, base + 48, spot ? std::cos(light.inner_angle) : 0.0f);
        put_f32(lighting, base + 52, spot ? std::cos(light.outer_angle) : 0.0f);
        put_u32(lighting, base + 56, light_first[slot]);
        put_u32(lighting, base + 60, light_count[slot]);
    }
    for (std::size_t j = 0; j < plans.size(); ++j) {
        const ShadowPlan& plan = plans[j];
        const std::uint64_t base = layout.shadow_base + kShadowRecordBytes * j;
        const glm::mat4 light_clip = vulkan ? core::coords::to_vulkan_clip(plan.light) : plan.light;
        put_floats(lighting, base, &light_clip[0][0], 16);
        put_f32(lighting, base + 64, static_cast<float>(plan.rect.x));
        put_f32(lighting, base + 68, static_cast<float>(plan.rect.y));
        put_f32(lighting, base + 72, static_cast<float>(plan.rect.size));
        put_u32(lighting, base + 76, plan.granted ? (plan.cascade_atlas ? 0u : 1u) : kNoShadowIndex);
        put_f32(lighting, base + 80, plan.depth_bias);
        put_f32(lighting, base + 84, plan.light_bias);
        put_u32(lighting, base + 88, plan.pcf_radius);
        put_f32(lighting, base + 92, plan.split_far);
        put_u32(lighting, base + 96, plan.kind);
        put_f32(lighting, base + 100, plan.texel_world);
        const float anchor[3] = {plan.anchor.x, plan.anchor.y, plan.anchor.z};
        put_floats(lighting, base + 104, anchor, 3);
    }
    std::memcpy(lighting.data() + layout.count_base, assignment.counts.data(), assignment.counts.size() * sizeof(std::uint32_t));
    std::memcpy(lighting.data() + layout.index_base, assignment.indices.data(), assignment.indices.size() * sizeof(std::uint32_t));
    if (const RhiStatus uploaded = device_.upload_buffer(lighting_upload_, 0, layout.bytes, lighting.data()); !uploaded.ok()) {
        return uploaded;
    }

    // A shadow tile's texels stay as they were drawn, so one tile's change can leave another tile's old depth behind when the caster moves
    // away. The unit of redraw is therefore the atlas: when any tile of an atlas changes, that atlas is cleared and every granted tile in it
    // is drawn again. An unchanged atlas draws nothing.
    bool dirty[2] = {false, false};  // [0] cascade atlas, [1] local atlas
    for (const ShadowPlan& plan : plans) {
        if (plan.render) {
            dirty[plan.cascade_atlas ? 0 : 1] = true;
        }
    }

    // The camera values every scene draw shares. The previous view-projection and the camera's motion put last frame's pose into this frame's
    // space. The first frame has no history, so its motion is zero. The velocity scale is UV units per NDC unit: x right, and y down, which
    // is -0.5 on DX12 (NDC y up) and +0.5 on Vulkan (NDC y down, after to_vulkan_clip).
    const glm::mat4 previous_dx = has_history_ ? history_.view_projection : snapshot.view_projection();
    const glm::dvec3 previous_position = has_history_ ? history_.camera_position : camera.position;
    FrameCamera scene_camera;
    scene_camera.view_projection = view_projection;
    scene_camera.previous_view_projection = vulkan ? core::coords::to_vulkan_clip(previous_dx) : previous_dx;
    scene_camera.velocity_scale = vulkan ? glm::vec2(0.5f, 0.5f) : glm::vec2(0.5f, -0.5f);
    scene_camera.camera_motion = glm::vec3(camera.position - previous_position);
    scene_camera.camera_position = glm::vec3(camera.position);
    // With the post pass the scene writes linear HDR. When TAA is on, the projection moves by this frame's sub-pixel jitter, which the vertex
    // stage adds to the rasterized position only (so motion vectors stay unjittered). Shadow tiles never get the jitter.
    if (post_) {
        scene_camera.linear_output = true;
        if (post_->settings().taa) {
            scene_camera.jitter = taa_jitter_ndc(taa_jitter(frame_index_), desc_.width, desc_.height, vulkan);
        }
    }

    // Shadow tile draws: each granted tile's casters, grouped by mesh, in a frame buffer whose camera slot holds the tile's clip matrix.
    struct Draw {
        std::size_t slot = 0;
        std::uint32_t mesh = 0;
        std::uint32_t material = 0;
        std::uint32_t instances = 0;
        std::uint64_t bytes = 0;  // the frame buffer's used size
        std::size_t pipeline = 0;
    };
    std::size_t slot_count = 0;
    const auto upload_draw = [&](std::vector<Draw>& draws, std::size_t slot, const std::vector<const SnapshotItem*>& items, std::uint32_t mesh,
                                 std::uint32_t material, std::size_t pipeline, const FrameCamera& frame_camera) -> RhiStatus {
        const std::uint64_t bytes = frame_size(items, mesh_bones);
        if (const RhiStatus ready = ensure_slot(slot, bytes); !ready.ok()) {
            return ready;
        }
        const std::vector<std::uint8_t> packed = pack_frame(snapshot, items, mesh_bones, frame_camera);
        if (const RhiStatus uploaded = device_.upload_buffer(slots_[slot].upload, 0, bytes, packed.data()); !uploaded.ok()) {
            return uploaded;
        }
        draws.push_back(Draw{slot, mesh, material, static_cast<std::uint32_t>(items.size()), bytes, pipeline});
        return core::success();
    };
    std::vector<std::vector<Draw>> tile_draws(plans.size());
    std::uint32_t shadow_draw_count = 0;
    for (std::size_t j = 0; j < plans.size(); ++j) {
        const ShadowPlan& plan = plans[j];
        if (!plan.granted || !dirty[plan.cascade_atlas ? 0 : 1]) {
            continue;
        }
        const glm::mat4 light_clip = vulkan ? core::coords::to_vulkan_clip(plan.light) : plan.light;
        const std::uint32_t atlas_size = plan.cascade_atlas ? atlas_cascade : atlas_local;
        const glm::mat4 tile_clip = tile_remap(plan.rect, atlas_size, vulkan) * light_clip;
        std::map<std::uint32_t, std::vector<const SnapshotItem*>> by_mesh;
        for (const SnapshotItem* item : plan.casters) {
            by_mesh[item->mesh].push_back(item);
        }
        FrameCamera tile;
        tile.view_projection = tile_clip;
        tile.previous_view_projection = tile_clip;
        tile.light_clip = &light_clip;
        for (const auto& [mesh, items] : by_mesh) {
            const std::size_t pipeline = gpu_meshes_[mesh].terrain ? kTerrainShadowPipeline
                                         : gpu_meshes_[mesh].bones > 0 ? kSkinnedShadowPipeline
                                                                       : kShadowPipeline;
            if (const RhiStatus added = upload_draw(tile_draws[j], slot_count++, items, mesh, kDefaultMaterial, pipeline, tile); !added.ok()) {
                return added;
            }
            ++shadow_draw_count;
        }
    }

    // Visible items drawn by material class: opaque and mask in one batch per mesh and material, blended one by one.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<const SnapshotItem*>> groups;
    struct Blended {
        float distance = 0.0f;
        const SnapshotItem* item = nullptr;
    };
    std::vector<Blended> blended;
    for (const SnapshotItem& item : snapshot.items()) {
        if (item.pick_id == 0) {
            continue;  // shadow-only: not in the view
        }
        if (materials_[item.material].params.blend == BlendMode::AlphaBlend) {
            const glm::vec3 center = glm::vec3(item.model * glm::vec4(bounds_[item.mesh].center, 1.0f));
            blended.push_back(Blended{glm::length(center), &item});
        } else {
            groups[{item.mesh, item.material}].push_back(&item);
        }
    }
    std::sort(blended.begin(), blended.end(), [](const Blended& a, const Blended& b) {
        return a.distance != b.distance ? a.distance > b.distance : a.item->pick_id < b.item->pick_id;
    });

    // A scene pipeline: opaque or blended, one or two-sided, and skinned or static (see build()).
    const auto scene_pipeline = [](bool blended_pass, bool two_sided, bool skinned) -> std::size_t {
        return (blended_pass ? 2 : 0) + (two_sided ? 1 : 0) + (skinned ? kSkinnedPipelineBase : 0);
    };
    // Opaque groups (US-037). A group goes to the indirect path while the limit allows, and when its kind has a pipeline (skinned groups need
    // the skinned indirect entry). The candidates of the indirect groups share one frame buffer, in group order, and each candidate's group
    // goes to the cull buffer. The other groups are drawn indexed, as before.
    struct IndirectGroup {
        std::uint32_t mesh = 0;
        std::uint32_t material = 0;
        std::size_t pipeline = 0;
    };
    std::vector<IndirectGroup> indirect_groups;
    std::vector<const SnapshotItem*> candidates;
    std::vector<std::uint32_t> candidate_groups;
    std::vector<Draw> opaque_draws;
    std::vector<Draw> blended_draws;
    // Meshlet groups (US-038): a static group of a mesh with meshlets takes the mesh shader path while the geometry path selects it. Its frame
    // buffer holds the items and the meshlet header, and the group is one task dispatch of its (item, meshlet) pairs.
    struct MeshletDraw {
        std::size_t slot = 0;
        std::uint32_t mesh = 0;
        std::uint32_t material = 0;
        bool two_sided = false;
        std::uint64_t bytes = 0;
        std::uint32_t task_x = 0;
        std::uint32_t task_y = 0;
        std::uint32_t counts_base = 0;  // first visibility counter of the group's task groups
        std::uint32_t task_groups = 0;
        std::uint32_t candidates = 0;   // (item, meshlet) pairs
    };
    std::vector<MeshletDraw> meshlet_draws;
    std::uint32_t meshlet_task_groups = 0;
    std::uint32_t meshlet_candidates = 0;
    const bool meshlets_selected = geometry_path() == GeometryPath::Meshlets;
    for (const auto& [key, items] : groups) {
        const bool skinned = gpu_meshes_[key.first].bones > 0;
        const bool two_sided = materials_[key.second].params.two_sided;
        if (gpu_meshes_[key.first].terrain) {
            // A terrain tile (US-047) takes the terrain pipeline, drawn directly: the meshlet and indirect paths have no terrain stride.
            if (const RhiStatus added = upload_draw(opaque_draws, slot_count++, items, key.first, key.second, kTerrainOpaquePipeline, scene_camera);
                !added.ok()) {
                return added;
            }
            continue;
        }
        if (meshlets_selected && !skinned && gpu_meshes_[key.first].meshlet_count > 0) {
            const GpuMesh& gpu = gpu_meshes_[key.first];
            const std::uint64_t pairs = static_cast<std::uint64_t>(items.size()) * gpu.meshlet_count;
            const auto task_groups = static_cast<std::uint32_t>((pairs + kTaskThreads - 1) / kTaskThreads);
            if (meshlet_task_groups + task_groups > desc_.meshlets.task_groups) {
                return rhi::fail(RhiErrorCode::OutOfRange, "render: the frame needs more visibility counters than the meshlet pool holds");
            }
            const std::size_t slot = slot_count++;
            const std::uint64_t bytes = frame_size(items, mesh_bones);
            if (const RhiStatus ready = ensure_slot(slot, bytes); !ready.ok()) {
                return ready;
            }
            const MeshletHeader header{gpu.meshlet_base, gpu.meshlet_count, static_cast<std::uint32_t>(items.size()), meshlet_task_groups,
                                       task_groups, !two_sided};
            FrameCamera meshlet_camera = scene_camera;
            meshlet_camera.meshlet = &header;
            const std::vector<std::uint8_t> packed = pack_frame(snapshot, items, mesh_bones, meshlet_camera);
            if (const RhiStatus uploaded = device_.upload_buffer(slots_[slot].upload, 0, bytes, packed.data()); !uploaded.ok()) {
                return uploaded;
            }
            // The dispatch is at most kTaskGroupsX wide, and the flat group index (y * kTaskGroupsX + x) covers every task group.
            const auto task_x = static_cast<std::uint32_t>(std::min<std::uint64_t>(task_groups, kTaskGroupsX));
            const std::uint32_t task_y = (task_groups + task_x - 1) / task_x;
            meshlet_draws.push_back(MeshletDraw{slot, key.first, key.second, two_sided, bytes, task_x, task_y, meshlet_task_groups, task_groups,
                                                static_cast<std::uint32_t>(pairs)});
            meshlet_task_groups += task_groups;
            meshlet_candidates += static_cast<std::uint32_t>(pairs);
            continue;
        }
        if (indirect_ready_ && indirect_groups.size() < desc_.max_draw_groups && (!skinned || skinned_indirect_ready_)) {
            const auto group = static_cast<std::uint32_t>(indirect_groups.size());
            indirect_groups.push_back(IndirectGroup{key.first, key.second, kIndirectPipelineBase + (skinned ? 2 : 0) + (two_sided ? 1 : 0)});
            for (const SnapshotItem* item : items) {
                candidates.push_back(item);
                candidate_groups.push_back(group);
            }
            continue;
        }
        const std::size_t pipeline = scene_pipeline(false, two_sided, skinned);
        if (const RhiStatus added = upload_draw(opaque_draws, slot_count++, items, key.first, key.second, pipeline, scene_camera); !added.ok()) {
            return added;
        }
    }

    // The indirect frame: the candidates' records in candidate order (the item index the cull writes), and the cull buffer.
    const std::size_t indirect_slot = slot_count++;
    const std::uint64_t indirect_bytes = indirect_groups.empty() ? 0 : frame_size(candidates, mesh_bones);
    const std::uint64_t cull_bytes = cull_buffer_bytes(desc_);
    const std::uint64_t count_bytes = count_buffer_bytes(desc_);
    if (!indirect_groups.empty()) {
        if (const RhiStatus ready = ensure_slot(indirect_slot, indirect_bytes); !ready.ok()) {
            return ready;
        }
        const std::vector<std::uint8_t> packed = pack_frame(snapshot, candidates, mesh_bones, scene_camera);
        if (const RhiStatus uploaded = device_.upload_buffer(slots_[indirect_slot].upload, 0, indirect_bytes, packed.data()); !uploaded.ok()) {
            return uploaded;
        }
        std::vector<CullGroup> cull_groups;
        cull_groups.reserve(indirect_groups.size());
        for (const IndirectGroup& group : indirect_groups) {
            cull_groups.push_back(CullGroup{gpu_meshes_[group.mesh].index_count, bounds_[group.mesh]});
        }
        const std::vector<std::uint8_t> cull = pack_cull(make_frustum(snapshot.view_projection()), candidate_groups, cull_groups,
                                                         draw_group_capacity_, desc_.gpu_culling, desc_.max_draw_groups, cull_bytes);
        if (const RhiStatus uploaded = device_.upload_buffer(cull_upload_, 0, cull_bytes, cull.data()); !uploaded.ok()) {
            return uploaded;
        }
    }
    for (const Blended& entry : blended) {
        const bool skinned = gpu_meshes_[entry.item->mesh].bones > 0;
        const std::size_t pipeline = scene_pipeline(true, materials_[entry.item->material].params.two_sided, skinned);
        if (const RhiStatus added = upload_draw(blended_draws, slot_count++, {entry.item}, entry.item->mesh, entry.item->material, pipeline, scene_camera);
            !added.ok()) {
            return added;
        }
    }

    // The ray-traced structures and the frame's tables are built and written before the graph; the pass itself runs inside it.
    if (const RhiStatus prepared = prepare_ray_traced(snapshot); !prepared.ok()) {
        return prepared;
    }

    RenderGraph graph;
    const TextureUsage target_usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    // The depth values feed the reflection pass as well (US-040), so they are sampled when it is on.
    const TextureUsage depth_values_usage =
        target_usage | ((desc_.screen_reflections || fog_) ? TextureUsage::ShaderResource : TextureUsage::None);
    const TextureUsage color_usage = target_usage | post_source_usage();
    const ResourceId color_res = graph.import_texture("color", color_,
        texture_desc(desc_.width, desc_.height, scene_color_format(), color_usage, "scene color"), ResourceState::Common, ResourceState::Common);
    const ResourceId ids_res = graph.import_texture("entity ids", ids_,
        texture_desc(desc_.width, desc_.height, Format::R32Uint, target_usage, "scene entity ids"), ResourceState::Common, ResourceState::Common);
    const ResourceId depth_values_res = graph.import_texture("depth values", depth_values_,
        texture_desc(desc_.width, desc_.height, Format::R32Float, depth_values_usage, "scene depth values"), ResourceState::Common, ResourceState::Common);
    const ResourceId velocity_res = graph.import_texture("velocity", velocity_,
        texture_desc(desc_.width, desc_.height, Format::R32G32Float, color_usage, "scene velocity"), ResourceState::Common, ResourceState::Common);
    const ResourceId surface_res = graph.import_texture("surface", surface_,
        texture_desc(desc_.width, desc_.height, Format::R16G16B16A16Float, color_usage, "scene surface"), ResourceState::Common, ResourceState::Common);
    const ResourceId base_res = graph.import_texture("base", base_,
        texture_desc(desc_.width, desc_.height, Format::R16G16B16A16Float, color_usage, "scene base"), ResourceState::Common, ResourceState::Common);
    const ResourceId depth_res = graph.import_texture("depth", depth_,
        texture_desc(desc_.width, desc_.height, Format::D32Float, TextureUsage::DepthStencil, "scene depth"), ResourceState::Common, ResourceState::Common);
    const BufferUsage readback_usage = BufferUsage::CpuRead | BufferUsage::CopyDestination;
    const ResourceId readback_color_res = graph.import_buffer("color readback", readback_color_,
        buffer_desc(readback_size(desc_.width, desc_.height, Format::R8G8B8A8Unorm), readback_usage, "scene color readback"),
        ResourceState::CopyDestination, ResourceState::CopyDestination);
    const ResourceId readback_ids_res = graph.import_buffer("id readback", readback_ids_,
        buffer_desc(readback_size(desc_.width, desc_.height, Format::R32Uint), readback_usage, "scene id readback"),
        ResourceState::CopyDestination, ResourceState::CopyDestination);
    const ResourceId readback_depth_res = graph.import_buffer("depth readback", readback_depth_,
        buffer_desc(readback_size(desc_.width, desc_.height, Format::R32Float), readback_usage, "scene depth readback"),
        ResourceState::CopyDestination, ResourceState::CopyDestination);
    const ResourceId readback_velocity_res = graph.import_buffer("velocity readback", readback_velocity_,
        buffer_desc(readback_size(desc_.width, desc_.height, Format::R32G32Float), readback_usage, "scene velocity readback"),
        ResourceState::CopyDestination, ResourceState::CopyDestination);

    // Lighting and the atlases stay in ShaderRead between frames, so every frame imports them in that state. The lighting buffer is
    // written by one copy each frame, before any pass reads it.
    const ResourceId lighting_res = graph.import_buffer("lighting", lighting_buffer_,
        buffer_desc(layout.bytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "lighting"), ResourceState::ShaderRead,
        ResourceState::ShaderRead);
    const ResourceId lighting_upload_res = graph.import_buffer("lighting upload", lighting_upload_,
        buffer_desc(layout.bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "lighting upload"), ResourceState::CopySource,
        ResourceState::CopySource);
    graph.add_pass("upload lighting", PassKind::Graphics, {{lighting_upload_res, Access::CopySource}, {lighting_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer(ctx.buffer(lighting_upload_res), 0, ctx.buffer(lighting_res), 0, layout.bytes); });
    const TextureUsage atlas_usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    const ResourceId cascade_color_res = graph.import_texture("cascade atlas", cascade_color_,
        texture_desc(atlas_cascade, atlas_cascade, Format::R32Float, atlas_usage, "cascade atlas"), ResourceState::ShaderRead, ResourceState::ShaderRead);
    const ResourceId local_color_res = graph.import_texture("local atlas", local_color_,
        texture_desc(atlas_local, atlas_local, Format::R32Float, atlas_usage, "local shadow atlas"), ResourceState::ShaderRead, ResourceState::ShaderRead);
    const ResourceId cascade_depth_res = graph.import_texture("cascade atlas depth", cascade_depth_,
        texture_desc(atlas_cascade, atlas_cascade, Format::D32Float, TextureUsage::DepthStencil, "cascade atlas"), ResourceState::Common, ResourceState::Common);
    const ResourceId local_depth_res = graph.import_texture("local atlas depth", local_depth_,
        texture_desc(atlas_local, atlas_local, Format::D32Float, TextureUsage::DepthStencil, "local shadow atlas"), ResourceState::Common, ResourceState::Common);

    // The environment stays in ShaderRead between frames, so every frame imports it in that state.
    std::vector<ResourceAccess> scene_accesses{{color_res, Access::ColorWrite}, {ids_res, Access::ColorWrite},
                                               {depth_values_res, Access::ColorWrite}, {velocity_res, Access::ColorWrite},
                                               {surface_res, Access::ColorWrite}, {base_res, Access::ColorWrite},
                                               {depth_res, Access::DepthWrite}};
    add_access(scene_accesses, lighting_res, Access::ShaderRead);
    add_access(scene_accesses, cascade_color_res, Access::ShaderRead);
    add_access(scene_accesses, local_color_res, Access::ShaderRead);
    const ResourceId environment_buffer_res = graph.import_buffer("environment", environment_buffer_,
        buffer_desc(kEnvironmentBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "environment"), ResourceState::ShaderRead,
        ResourceState::ShaderRead);
    add_access(scene_accesses, environment_buffer_res, Access::ShaderRead);
    // The probe grid (US-040) is bound with the environment, so it is read in the same state.
    const ResourceId gi_res = graph.import_buffer("probe grid", gi_buffer_,
        buffer_desc(gi_bytes_, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "probe grid"), ResourceState::ShaderRead,
        ResourceState::ShaderRead);
    add_access(scene_accesses, gi_res, Access::ShaderRead);
    std::array<ResourceId, kEnvironmentTextures> environment_res{};
    for (std::size_t i = 0; i < kEnvironmentTextures; ++i) {
        environment_res[i] = graph.import_texture("environment " + std::to_string(i), environment_textures_[i], environment_texture_desc(environment_, i),
                                                  ResourceState::ShaderRead, ResourceState::ShaderRead);
        add_access(scene_accesses, environment_res[i], Access::ShaderRead);
    }

    // Each resource is imported once per graph, however many draws use it. The graph allows one resource id per device handle, and it
    // declares each access once per pass. Per-slot frame buffers are imported per draw.
    struct MeshResources {
        ResourceId vertices;
        ResourceId indices;
    };
    std::map<std::uint32_t, MeshResources> mesh_res;
    std::map<std::uint32_t, ResourceId> material_params_res;
    std::map<std::uint32_t, ResourceId> texture_res;
    const auto import_mesh = [&](std::uint32_t mesh, std::vector<ResourceAccess>& accesses) -> MeshResources {
        auto found = mesh_res.find(mesh);
        if (found == mesh_res.end()) {
            const GpuMesh& gpu = gpu_meshes_[mesh];
            const std::string tag = std::to_string(mesh);
            const ResourceId vertices = graph.import_buffer("mesh vertices " + tag, gpu.vertices,
                buffer_desc(static_cast<std::uint64_t>(meshes_[mesh].positions.size()) * gpu.vertex_stride, BufferUsage::Vertex | BufferUsage::CopyDestination, "mesh vertices"),
                ResourceState::Common, ResourceState::Common);
            const ResourceId indices = graph.import_buffer("mesh indices " + tag, gpu.indices,
                buffer_desc(meshes_[mesh].indices.size() * sizeof(std::uint32_t), BufferUsage::Index | BufferUsage::CopyDestination, "mesh indices"),
                ResourceState::Common, ResourceState::Common);
            found = mesh_res.emplace(mesh, MeshResources{vertices, indices}).first;
        }
        add_access(accesses, found->second.vertices, Access::VertexRead);
        add_access(accesses, found->second.indices, Access::IndexRead);
        return found->second;
    };
    const auto import_texture = [&](std::uint32_t texture) -> ResourceId {
        auto found = texture_res.find(texture);
        if (found == texture_res.end()) {
            const GpuTexture& gpu = textures_[texture];
            const ResourceId id = graph.import_texture("texture " + std::to_string(texture), gpu.texture, gpu.desc, ResourceState::ShaderRead,
                                                       ResourceState::ShaderRead);
            found = texture_res.emplace(texture, id).first;
        }
        add_access(scene_accesses, found->second, Access::ShaderRead);
        return found->second;
    };
    const auto import_material = [&](std::uint32_t material) {
        auto found = material_params_res.find(material);
        if (found == material_params_res.end()) {
            const GpuMaterial& gpu = materials_[material];
            const ResourceId params = graph.import_buffer("material " + std::to_string(material), gpu.parameters,
                buffer_desc(kMaterialBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "material parameters"), ResourceState::ShaderRead,
                ResourceState::ShaderRead);
            found = material_params_res.emplace(material, params).first;
        }
        add_access(scene_accesses, found->second, Access::ShaderRead);
        for (const std::uint32_t texture : materials_[material].textures) {
            import_texture(texture);
        }
    };

    // A draw uploads its frame buffer in its own pass, then binds the buffer and its mesh. Shadow draws declare their accesses to the
    // shadow pass; scene draws declare theirs to the scene pass.
    // Uploads the first `used` bytes of a slot's frame buffer in a pass of its own, and returns the frame resource the draws read.
    const auto import_frame = [&](std::size_t slot_index, std::uint64_t used) -> ResourceId {
        const std::string tag = std::to_string(slot_index);
        const FrameSlot& slot = slots_[slot_index];
        const ResourceId upload_res = graph.import_buffer("frame upload " + tag, slot.upload,
            buffer_desc(slot.capacity, BufferUsage::CopySource | BufferUsage::CpuWrite, "scene frame upload"),
            ResourceState::CopySource, ResourceState::CopySource);
        const ResourceId frame_res = graph.import_buffer("frame " + tag, slot.frame,
            buffer_desc(slot.capacity, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "scene frame"),
            ResourceState::Common, ResourceState::Common);
        graph.add_pass("upload frame " + tag, PassKind::Graphics, {{upload_res, Access::CopySource}, {frame_res, Access::CopyDestination}},
                       [=](PassContext& ctx) { return ctx.commands().copy_buffer(ctx.buffer(upload_res), 0, ctx.buffer(frame_res), 0, used); });
        return frame_res;
    };
    const auto bind = [&](const Draw& draw, std::vector<ResourceAccess>& accesses, bool scene) -> BoundDraw {
        const FrameSlot& slot = slots_[draw.slot];
        const ResourceId frame_res = import_frame(draw.slot, draw.bytes);
        add_access(accesses, frame_res, Access::ShaderRead);
        add_access(accesses, lighting_res, Access::ShaderRead);
        const MeshResources mesh = import_mesh(draw.mesh, accesses);
        if (scene) {
            import_material(draw.material);
        }
        const bool shadow = draw.pipeline == kShadowPipeline || draw.pipeline == kSkinnedShadowPipeline || draw.pipeline == kTerrainShadowPipeline;
        BoundDraw bound;
        bound.frame_set = slot.set;
        bound.material_set = materials_[shadow ? kDefaultMaterial : draw.material].set;
        bound.environment_set = environment_set_;
        bound.lighting_set = shadow ? shadow_lighting_set_ : lighting_set_;
        bound.pipeline = pipelines_[draw.pipeline];
        bound.vertices = mesh.vertices;
        bound.indices = mesh.indices;
        bound.index_count = gpu_meshes_[draw.mesh].index_count;
        bound.instances = draw.instances;
        return bound;
    };

    // The shadow pass draws every granted tile of a dirty atlas, in order. Each tile is one render pass that clears its depth. The first
    // tile of a dirty atlas also clears the atlas color to the far depth, so an empty tile reads as lit.
    struct TileBound {
        bool cascade = true;
        bool clear_color = false;
        std::vector<BoundDraw> draws;
    };
    std::vector<TileBound> tiles;
    std::vector<ResourceAccess> shadow_accesses;
    bool cleared[2] = {false, false};
    for (std::size_t j = 0; j < plans.size(); ++j) {
        if (!plans[j].granted || !dirty[plans[j].cascade_atlas ? 0 : 1]) {
            continue;
        }
        TileBound tile;
        tile.cascade = plans[j].cascade_atlas;
        bool& done = cleared[tile.cascade ? 0 : 1];
        tile.clear_color = !done;
        done = true;
        for (const Draw& draw : tile_draws[j]) {
            tile.draws.push_back(bind(draw, shadow_accesses, false));
        }
        tiles.push_back(std::move(tile));
    }
    const bool any_cascade = dirty[0];
    const bool any_local = dirty[1];
    if (any_cascade) {
        add_access(shadow_accesses, cascade_color_res, Access::ColorWrite);
        add_access(shadow_accesses, cascade_depth_res, Access::DepthWrite);
    }
    if (any_local) {
        add_access(shadow_accesses, local_color_res, Access::ColorWrite);
        add_access(shadow_accesses, local_depth_res, Access::DepthWrite);
    }
    if (!tiles.empty()) {
        graph.add_pass("shadow maps", PassKind::Graphics, std::move(shadow_accesses), [=](PassContext& ctx) -> RhiStatus {
            rhi::CommandList& commands = ctx.commands();
            for (const TileBound& tile : tiles) {
                rhi::RenderPassDesc pass;
                pass.color_targets = {ctx.texture(tile.cascade ? cascade_color_res : local_color_res)};
                pass.depth_target = ctx.texture(tile.cascade ? cascade_depth_res : local_depth_res);
                pass.clear_depth = 1.0f;
                if (tile.clear_color) {
                    pass.clear_color = std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
                }
                if (const RhiStatus status = commands.begin_render_pass(pass); !status.ok()) {
                    return status;
                }
                for (const BoundDraw& draw : tile.draws) {
                    if (const RhiStatus status = draw_bound(ctx, draw); !status.ok()) {
                        return status;
                    }
                }
                if (const RhiStatus status = commands.end_render_pass(); !status.ok()) {
                    return status;
                }
            }
            return core::success();
        });
    }

    std::vector<BoundDraw> opaque_bound;
    std::vector<BoundDraw> blended_bound;
    for (const Draw& draw : opaque_draws) {
        opaque_bound.push_back(bind(draw, scene_accesses, true));
    }
    for (const Draw& draw : blended_draws) {
        blended_bound.push_back(bind(draw, scene_accesses, true));
    }

    // Indirect groups (US-037). The cull pass clears the counts, writes each visible candidate into its group's records, and bumps the count.
    // The scene pass then draws each group with its count. The records and counts stay on the GPU; the counts are read back for the stats.
    const bool indirect_active = !indirect_groups.empty();
    ResourceId counts_res;
    ResourceId counts_readback_res;
    if (indirect_active) {
        const std::uint64_t args_bytes = static_cast<std::uint64_t>(desc_.max_draw_groups) * draw_group_capacity_ * rhi::kDrawIndexedIndirectStride;
        const BufferUsage count_usage = BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument | BufferUsage::CopySource | BufferUsage::CopyDestination;
        const ResourceId cull_upload_res = graph.import_buffer("cull upload", cull_upload_,
            buffer_desc(cull_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "cull upload"), ResourceState::CopySource, ResourceState::CopySource);
        const ResourceId cull_data_res = graph.import_buffer("cull data", cull_data_,
            buffer_desc(cull_bytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "cull data"), ResourceState::Common, ResourceState::Common);
        const ResourceId zero_res = graph.import_buffer("indirect counts zero", counts_zero_,
            buffer_desc(count_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "indirect counts zero"), ResourceState::CopySource, ResourceState::CopySource);
        counts_res = graph.import_buffer("indirect counts", cull_counts_, buffer_desc(count_bytes, count_usage, "indirect counts"),
            ResourceState::Common, ResourceState::Common);
        const ResourceId args_res = graph.import_buffer("indirect arguments", cull_args_,
            buffer_desc(args_bytes, BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument, "indirect arguments"), ResourceState::Common,
            ResourceState::Common);
        counts_readback_res = graph.import_buffer("indirect counts readback", counts_readback_,
            buffer_desc(count_bytes, BufferUsage::CpuRead | BufferUsage::CopyDestination, "indirect counts readback"), ResourceState::CopyDestination,
            ResourceState::CopyDestination);
        const ResourceId instance_res = graph.import_buffer("instance ids", instance_ids_,
            buffer_desc(4ull * desc_.max_items, BufferUsage::Vertex | BufferUsage::CopyDestination, "instance ids"), ResourceState::Common,
            ResourceState::Common);
        graph.add_pass("upload cull", PassKind::Graphics, {{cull_upload_res, Access::CopySource}, {cull_data_res, Access::CopyDestination}},
                       [=](PassContext& ctx) { return ctx.commands().copy_buffer(ctx.buffer(cull_upload_res), 0, ctx.buffer(cull_data_res), 0, cull_bytes); });
        const ResourceId frame_res = import_frame(indirect_slot, indirect_bytes);
        const rhi::DescriptorSetHandle frame_set = slots_[indirect_slot].set;
        const rhi::PipelineHandle cull_pipeline = cull_pipeline_;
        const rhi::DescriptorSetHandle cull_set = cull_set_;
        const auto dispatches = static_cast<std::uint32_t>((candidates.size() + kCullThreads - 1) / kCullThreads);
        // The graph allows one writer per resource, so the counts are zeroed inside the cull pass: the counts move to CopyDestination for the
        // copy and back to UnorderedAccess before the dispatch, and the transitions order the copy before the atomics.
        graph.add_pass("indirect cull", PassKind::Compute,
                       {{frame_res, Access::ShaderRead}, {cull_data_res, Access::ShaderRead}, {zero_res, Access::CopySource},
                        {args_res, Access::StorageWrite}, {counts_res, Access::StorageWrite}},
                       [=](PassContext& ctx) -> RhiStatus {
                           rhi::CommandList& commands = ctx.commands();
                           const rhi::BufferHandle counts = ctx.buffer(counts_res);
                           if (const RhiStatus status = commands.transition(counts, ResourceState::UnorderedAccess, ResourceState::CopyDestination); !status.ok()) {
                               return status;
                           }
                           if (const RhiStatus status = commands.copy_buffer(ctx.buffer(zero_res), 0, counts, 0, count_bytes); !status.ok()) {
                               return status;
                           }
                           if (const RhiStatus status = commands.transition(counts, ResourceState::CopyDestination, ResourceState::UnorderedAccess); !status.ok()) {
                               return status;
                           }
                           if (const RhiStatus status = commands.set_pipeline(cull_pipeline); !status.ok()) {
                               return status;
                           }
                           if (const RhiStatus status = commands.set_descriptor_set(0, frame_set); !status.ok()) {
                               return status;
                           }
                           if (const RhiStatus status = commands.set_descriptor_set(1, cull_set); !status.ok()) {
                               return status;
                           }
                           return commands.dispatch(dispatches, 1, 1);
                       });
        add_access(scene_accesses, frame_res, Access::ShaderRead);
        add_access(scene_accesses, instance_res, Access::VertexRead);
        add_access(scene_accesses, args_res, Access::IndirectRead);
        add_access(scene_accesses, counts_res, Access::IndirectRead);
        for (std::size_t g = 0; g < indirect_groups.size(); ++g) {
            const IndirectGroup& group = indirect_groups[g];
            const MeshResources mesh = import_mesh(group.mesh, scene_accesses);
            import_material(group.material);
            BoundDraw draw;
            draw.frame_set = frame_set;
            draw.material_set = materials_[group.material].set;
            draw.environment_set = environment_set_;
            draw.lighting_set = lighting_set_;
            draw.pipeline = pipelines_[group.pipeline];
            draw.vertices = mesh.vertices;
            draw.indices = mesh.indices;
            draw.index_count = gpu_meshes_[group.mesh].index_count;
            draw.indirect = true;
            draw.instance_ids = instance_res;
            draw.args = args_res;
            draw.counts = counts_res;
            draw.args_offset = static_cast<std::uint64_t>(g) * draw_group_capacity_ * rhi::kDrawIndexedIndirectStride;
            draw.count_offset = static_cast<std::uint64_t>(g) * rhi::kIndirectCountSize;
            draw.max_draws = draw_group_capacity_;
            opaque_bound.push_back(draw);
        }
    }

    // Meshlet draws (US-038). The task and mesh stages read each group's frame buffer and the pools, and the task stage writes one survivor
    // count per task group. No pass zeroes the counts: each task group writes its own entry, and the readback reads only the entries this
    // frame wrote, so the counts need no reset.
    const bool meshlets_active = !meshlet_draws.empty();
    ResourceId meshlet_counts_res;
    ResourceId meshlet_counts_readback_res;
    if (meshlets_active) {
        const MeshletPoolSettings& pool = desc_.meshlets;
        const BufferUsage gpu_buffer = BufferUsage::ShaderResource | BufferUsage::CopyDestination;
        const std::uint64_t counts_bytes = static_cast<std::uint64_t>(pool.task_groups) * 4;
        const ResourceId records_res = graph.import_buffer("meshlet records", meshlet_records_,
            buffer_desc(static_cast<std::uint64_t>(pool.meshlets) * kMeshletRecordBytes, gpu_buffer, "meshlet records"), ResourceState::ShaderRead,
            ResourceState::ShaderRead);
        const ResourceId vertices_res = graph.import_buffer("meshlet vertices", meshlet_vertices_,
            buffer_desc(static_cast<std::uint64_t>(pool.vertices) * 4, gpu_buffer, "meshlet vertices"), ResourceState::ShaderRead,
            ResourceState::ShaderRead);
        const ResourceId triangles_res = graph.import_buffer("meshlet triangles", meshlet_triangles_,
            buffer_desc(pool.triangle_bytes, gpu_buffer, "meshlet triangles"), ResourceState::ShaderRead, ResourceState::ShaderRead);
        const ResourceId mesh_vertices_res = graph.import_buffer("mesh vertex pool", mesh_vertex_pool_,
            buffer_desc(static_cast<std::uint64_t>(pool.mesh_vertices) * kVertexStride, gpu_buffer, "mesh vertex pool"), ResourceState::ShaderRead,
            ResourceState::ShaderRead);
        add_access(scene_accesses, records_res, Access::ShaderRead);
        add_access(scene_accesses, vertices_res, Access::ShaderRead);
        add_access(scene_accesses, triangles_res, Access::ShaderRead);
        add_access(scene_accesses, mesh_vertices_res, Access::ShaderRead);
        meshlet_counts_res = graph.import_buffer("meshlet counts", meshlet_counts_,
            buffer_desc(counts_bytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource | BufferUsage::CopyDestination, "meshlet counts"),
            ResourceState::Common, ResourceState::Common);
        add_access(scene_accesses, meshlet_counts_res, Access::StorageWrite);
        meshlet_counts_readback_res = graph.import_buffer("meshlet counts readback", meshlet_counts_readback_,
            buffer_desc(counts_bytes, BufferUsage::CpuRead | BufferUsage::CopyDestination, "meshlet counts readback"), ResourceState::CopyDestination,
            ResourceState::CopyDestination);
        for (const MeshletDraw& meshlet : meshlet_draws) {
            const ResourceId frame_res = import_frame(meshlet.slot, meshlet.bytes);
            add_access(scene_accesses, frame_res, Access::ShaderRead);
            import_material(meshlet.material);
            BoundDraw draw;
            draw.frame_set = slots_[meshlet.slot].meshlet_set;
            draw.material_set = materials_[meshlet.material].set;
            draw.environment_set = environment_set_;
            draw.lighting_set = lighting_set_;
            draw.pipeline = pipelines_[kMeshletPipelineBase + (meshlet.two_sided ? 1 : 0)];
            draw.meshlet = true;
            draw.task_x = meshlet.task_x;
            draw.task_y = meshlet.task_y;
            opaque_bound.push_back(draw);
        }
    }

    // The ray-traced pass (US-043) builds the top-level structure and traces the shadow and reflection targets. It runs before the scene pass,
    // which reads them at the same pixel. It declares no accesses: the pass keeps its targets' states itself (RtEffects::record), and both
    // targets end in ShaderRead for the scene.
    if (ray_traced_) {
        RtEffects* effects = rt_effects_.get();
        DynamicGi* dynamic = dynamic_gi_.get();
        const rhi::AccelerationStructureHandle top_level = top_level_;
        graph.add_pass("ray-traced effects", PassKind::Compute, {}, [=](PassContext& ctx) -> RhiStatus {
            rhi::CommandList& commands = ctx.commands();
            if (const RhiStatus status = commands.build_top_level(top_level, rhi::BuildMode::Build); !status.ok()) {
                return status;
            }
            if (const RhiStatus status = effects->record(commands); !status.ok()) {
                return status;
            }
            // The dynamic grid traces the same top-level structure, and its history ends in ShaderRead for the scene (US-044).
            return dynamic ? dynamic->record(commands) : core::success();
        });
        // The denoisers read the pass's targets and the primary geometry, all in ShaderRead, and leave their outputs in ShaderRead for the scene.
        // Each is its own pass, so the graph times it apart from the others (US-045).
        if (denoise_) {
            TemporalDenoiser* shadow = denoise_shadow_.get();
            TemporalDenoiser* reflection = denoise_reflection_.get();
            graph.add_pass("denoise shadow", PassKind::Compute, {}, [=](PassContext& ctx) -> RhiStatus {
                return shadow->record(ctx.commands());
            });
            graph.add_pass("denoise reflection", PassKind::Compute, {}, [=](PassContext& ctx) -> RhiStatus {
                return reflection->record(ctx.commands());
            });
        }
        // The fog's inject and integrate read the traced scene's top-level structure, which the pass above built (US-045).
        if (fog_) {
            VolumetricFog* fog = volumetric_fog_.get();
            graph.add_pass("fog inject", PassKind::Compute, {}, [=](PassContext& ctx) -> RhiStatus {
                return fog->inject(ctx.commands());
            });
            graph.add_pass("fog integrate", PassKind::Compute, {}, [=](PassContext& ctx) -> RhiStatus {
                return fog->integrate(ctx.commands());
            });
        }
    }

    // One scene pass with two render passes. The opaque pass clears and writes all four targets. The blended pass keeps them, writes color
    // only, and sorts by the order built above.
    graph.add_pass("scene", PassKind::Graphics, std::move(scene_accesses), [=](PassContext& ctx) -> RhiStatus {
        rhi::CommandList& commands = ctx.commands();
        rhi::RenderPassDesc opaque;
        opaque.color_targets = {ctx.texture(color_res), ctx.texture(ids_res), ctx.texture(depth_values_res), ctx.texture(velocity_res),
                                ctx.texture(surface_res), ctx.texture(base_res)};
        opaque.depth_target = ctx.texture(depth_res);
        opaque.clear_color = kBackground;
        opaque.clear_depth = 1.0f;
        if (const RhiStatus status = commands.begin_render_pass(opaque); !status.ok()) {
            return status;
        }
        for (const BoundDraw& draw : opaque_bound) {
            if (const RhiStatus status = draw_bound(ctx, draw); !status.ok()) {
                return status;
            }
        }
        if (const RhiStatus status = commands.end_render_pass(); !status.ok()) {
            return status;
        }
        if (blended_bound.empty()) {
            return core::success();
        }
        rhi::RenderPassDesc blended_pass;
        blended_pass.color_targets = {ctx.texture(color_res)};
        blended_pass.depth_target = ctx.texture(depth_res);
        if (const RhiStatus status = commands.begin_render_pass(blended_pass); !status.ok()) {
            return status;
        }
        for (const BoundDraw& draw : blended_bound) {
            if (const RhiStatus status = draw_bound(ctx, draw); !status.ok()) {
                return status;
            }
        }
        return commands.end_render_pass();
    });

    // The fog (US-045) adds to the lit color after the opaque pass, into its own target. The reflection and post passes read the fogged color.
    ResourceId lit_res = color_res;
    if (fog_) {
        const ResourceId fogged_res = graph.import_texture("fogged color", fogged_,
            texture_desc(desc_.width, desc_.height, scene_color_format(), TextureUsage::ShaderResource | TextureUsage::UnorderedAccess, "fogged color"),
            ResourceState::Common, ResourceState::Common);
        std::vector<ResourceAccess> fog_accesses{{color_res, Access::ShaderRead}, {depth_values_res, Access::ShaderRead},
                                                 {fogged_res, Access::StorageWrite}};
        VolumetricFog* fog = volumetric_fog_.get();
        graph.add_pass("fog apply", PassKind::Compute, std::move(fog_accesses), [=](PassContext& ctx) -> RhiStatus {
            return fog->apply(ctx.commands());
        });
        lit_res = fogged_res;
    }

    // With the post pass, the displayed image is copied to the color readback by the post pass, after it runs (below). The HDR color
    // target itself is not 8-bit, so this graph does not copy it.
    const bool post = post_ != nullptr;
    // The reflection pass (US-040) reads the lit color and the surface targets of the opaque pass, and writes the corrected color into its
    // own target. Reading the lit color and writing a separate target keeps one writer per resource, and the post pass reads the result.
    if (desc_.screen_reflections) {
        const ResourceId reflected_res = graph.import_texture("reflected color", reflected_,
            texture_desc(desc_.width, desc_.height, scene_color_format(), TextureUsage::ShaderResource | TextureUsage::UnorderedAccess,
                         "reflected color"),
            ResourceState::Common, ResourceState::Common);
        const ResourceId params_res = graph.import_buffer("reflection parameters", reflection_params_,
            buffer_desc(kReflectionBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "reflection parameters"),
            ResourceState::ShaderRead, ResourceState::ShaderRead);
        std::vector<ResourceAccess> reflection_accesses{{lit_res, Access::ShaderRead}, {depth_values_res, Access::ShaderRead},
                                                        {surface_res, Access::ShaderRead}, {base_res, Access::ShaderRead},
                                                        {params_res, Access::ShaderRead}, {reflected_res, Access::StorageWrite},
                                                        {environment_buffer_res, Access::ShaderRead}, {gi_res, Access::ShaderRead}};
        for (const ResourceId environment : environment_res) {
            reflection_accesses.push_back(ResourceAccess{environment, Access::ShaderRead});
        }
        const rhi::PipelineHandle reflection_pipeline = reflection_pipeline_;
        const rhi::DescriptorSetHandle reflection_set = reflection_set_;
        const rhi::DescriptorSetHandle environment_set = environment_set_;
        const std::uint32_t groups_x = (desc_.width + 7) / 8;
        const std::uint32_t groups_y = (desc_.height + 7) / 8;
        graph.add_pass("screen reflections", PassKind::Compute, std::move(reflection_accesses), [=](PassContext& ctx) -> RhiStatus {
            rhi::CommandList& commands = ctx.commands();
            if (const RhiStatus status = commands.set_pipeline(reflection_pipeline); !status.ok()) {
                return status;
            }
            if (const RhiStatus status = commands.set_descriptor_set(0, reflection_set); !status.ok()) {
                return status;
            }
            if (const RhiStatus status = commands.set_descriptor_set(1, environment_set); !status.ok()) {
                return status;
            }
            return commands.dispatch(groups_x, groups_y, 1);
        });
    }

    std::vector<ResourceAccess> readback_accesses{{ids_res, Access::CopySource}, {depth_values_res, Access::CopySource},
                                                  {velocity_res, Access::CopySource}, {readback_ids_res, Access::CopyDestination},
                                                  {readback_depth_res, Access::CopyDestination}, {readback_velocity_res, Access::CopyDestination}};
    if (!post) {
        readback_accesses.push_back({color_res, Access::CopySource});
        readback_accesses.push_back({readback_color_res, Access::CopyDestination});
    }
    if (indirect_active) {
        readback_accesses.push_back({counts_res, Access::CopySource});
        readback_accesses.push_back({counts_readback_res, Access::CopyDestination});
    }
    if (meshlets_active) {
        readback_accesses.push_back({meshlet_counts_res, Access::CopySource});
        readback_accesses.push_back({meshlet_counts_readback_res, Access::CopyDestination});
    }
    graph.add_pass("read back", PassKind::Graphics, std::move(readback_accesses), [=](PassContext& ctx) -> RhiStatus {
        rhi::CommandList& commands = ctx.commands();
        if (!post) {
            if (const RhiStatus status = commands.copy_texture_to_buffer(ctx.texture(color_res), ctx.buffer(readback_color_res), 0); !status.ok()) {
                return status;
            }
        }
        if (const RhiStatus status = commands.copy_texture_to_buffer(ctx.texture(ids_res), ctx.buffer(readback_ids_res), 0); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = commands.copy_texture_to_buffer(ctx.texture(velocity_res), ctx.buffer(readback_velocity_res), 0); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = commands.copy_texture_to_buffer(ctx.texture(depth_values_res), ctx.buffer(readback_depth_res), 0); !status.ok()) {
            return status;
        }
        if (meshlets_active) {
            const std::uint64_t bytes = static_cast<std::uint64_t>(meshlet_task_groups) * 4;
            if (const RhiStatus status = commands.copy_buffer(ctx.buffer(meshlet_counts_res), 0, ctx.buffer(meshlet_counts_readback_res), 0, bytes);
                !status.ok()) {
                return status;
            }
        }
        if (!indirect_active) {
            return core::success();
        }
        return commands.copy_buffer(ctx.buffer(counts_res), 0, ctx.buffer(counts_readback_res), 0, count_bytes);
    });

    // The reflection parameters are the frame's own, so they are uploaded before the graph reads them.
    if (desc_.screen_reflections) {
        const std::vector<std::uint8_t> params = pack_reflection_params(view_projection, scene_camera.velocity_scale, desc_.width, desc_.height,
                                                                        static_cast<float>(camera.near_plane), static_cast<float>(camera.far_plane),
                                                                        desc_.reflections);
        // The buffer is Common until the first graph reads it, and ShaderRead after that (the graph's import states).
        const ResourceState before = reflection_params_ready_ ? ResourceState::ShaderRead : ResourceState::Common;
        if (const RhiStatus uploaded = upload_buffer_bytes(reflection_params_, kReflectionBytes, params,
                                                           BufferUsage::ShaderResource | BufferUsage::CopyDestination, ResourceState::ShaderRead, before);
            !uploaded.ok()) {
            return uploaded;
        }
        reflection_params_ready_ = true;
    }
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        return rhi::fail(RhiErrorCode::InvalidState, "render: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    if (!executed.ok()) {
        return core::failure(executed.error());
    }
    frame_passes_ = executed.value().passes;

    // The post pass resolves the HDR frame into the displayed image, and copies it into the color readback. It waits for its own submission.
    if (post) {
        PostInputs post_inputs;
        post_inputs.color = desc_.screen_reflections ? reflected_ : (fog_ ? fogged_ : color_);
        post_inputs.velocity = velocity_;
        post_inputs.readback = readback_color_;
        post_inputs.camera_position = camera.position;
        post_inputs.camera_cut = camera_cut_;
        if (const RhiStatus ran = post_->run(post_inputs); !ran.ok()) {
            return ran;
        }
        camera_cut_ = false;
    }

    // The graph waited for the frame, so the readback buffers hold this frame's pixels.
    if (readback_enabled_) {
        const std::uint32_t w = desc_.width;
        const std::uint32_t h = desc_.height;
        const std::uint64_t color_pitch = rhi::copy_row_pitch(w, Format::R8G8B8A8Unorm);
        const std::uint64_t id_pitch = rhi::copy_row_pitch(w, Format::R32Uint);
        const std::uint64_t depth_pitch = rhi::copy_row_pitch(w, Format::R32Float);
        std::vector<std::uint8_t> color_bytes(readback_size(w, h, Format::R8G8B8A8Unorm));
        std::vector<std::uint8_t> id_bytes(readback_size(w, h, Format::R32Uint));
        std::vector<std::uint8_t> depth_bytes(readback_size(w, h, Format::R32Float));
        std::vector<std::uint8_t> velocity_bytes(readback_size(w, h, Format::R32G32Float));
        const std::uint64_t velocity_pitch = rhi::copy_row_pitch(w, Format::R32G32Float);
        if (const RhiStatus read = device_.read_buffer(readback_velocity_, 0, velocity_bytes.size(), velocity_bytes.data()); !read.ok()) {
            return read;
        }
        if (const RhiStatus read = device_.read_buffer(readback_color_, 0, color_bytes.size(), color_bytes.data()); !read.ok()) {
            return read;
        }
        if (const RhiStatus read = device_.read_buffer(readback_ids_, 0, id_bytes.size(), id_bytes.data()); !read.ok()) {
            return read;
        }
        if (const RhiStatus read = device_.read_buffer(readback_depth_, 0, depth_bytes.size(), depth_bytes.data()); !read.ok()) {
            return read;
        }

        image_.width = w;
        image_.height = h;
        image_.color.resize(static_cast<std::size_t>(w) * h);
        image_.pick_ids.resize(static_cast<std::size_t>(w) * h);
        image_.depth.resize(static_cast<std::size_t>(w) * h);
        image_.velocity.resize(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t pixel = image_.index(x, y);
                const std::uint8_t* color = color_bytes.data() + y * color_pitch + x * 4;
                image_.color[pixel] = {color[0], color[1], color[2], color[3]};
                std::memcpy(&image_.pick_ids[pixel], id_bytes.data() + y * id_pitch + x * 4, sizeof(std::uint32_t));
                std::memcpy(&image_.depth[pixel], depth_bytes.data() + y * depth_pitch + x * 4, sizeof(float));
                std::memcpy(&image_.velocity[pixel], velocity_bytes.data() + y * velocity_pitch + x * 8, sizeof(glm::vec2));
            }
        }
    }
    IndirectStats stats;
    stats.indexed_groups = static_cast<std::uint32_t>(groups.size() - indirect_groups.size() - meshlet_draws.size());
    if (indirect_active) {
        std::vector<std::uint32_t> counts(desc_.max_draw_groups);
        if (const RhiStatus read = device_.read_buffer(counts_readback_, 0, count_bytes, counts.data()); !read.ok()) {
            return read;
        }
        stats.active = true;
        stats.groups = static_cast<std::uint32_t>(indirect_groups.size());
        stats.candidates = static_cast<std::uint32_t>(candidates.size());
        for (std::size_t g = 0; g < indirect_groups.size(); ++g) {
            const std::uint32_t visible = counts[g];
            const std::uint32_t drawn = std::min(visible, draw_group_capacity_);
            stats.visible += visible;
            stats.drawn += drawn;
            stats.overflow += visible - drawn;
        }
    }
    indirect_stats_ = stats;

    MeshletStats meshlet_stats;
    if (meshlets_active) {
        std::vector<std::uint32_t> counts(meshlet_task_groups);
        if (const RhiStatus read = device_.read_buffer(meshlet_counts_readback_, 0, 4ull * meshlet_task_groups, counts.data()); !read.ok()) {
            return read;
        }
        meshlet_stats.active = true;
        meshlet_stats.groups = static_cast<std::uint32_t>(meshlet_draws.size());
        meshlet_stats.candidates = meshlet_candidates;
        for (const MeshletDraw& meshlet : meshlet_draws) {
            for (std::uint32_t g = 0; g < meshlet.task_groups; ++g) {
                meshlet_stats.visible += counts[meshlet.counts_base + g];
            }
        }
    }
    meshlet_stats_ = meshlet_stats;

    history_ = MotionHistory{snapshot.view_projection(), camera.position};
    has_history_ = true;
    frame_history_ = true;
    previous_camera_position_ = camera.position;
    previous_camera_forward_ = glm::vec3(camera.forward());
    previous_camera_up_ = glm::vec3(camera.orientation * glm::dvec3(0.0, 1.0, 0.0));
    previous_sun_direction_ = sun_toward(snapshot);
    previous_local_lights_ = point_light_positions(snapshot);
    ++frame_index_;

    std::uint32_t maps_granted = 0;
    std::uint32_t maps_rendered = 0;
    std::uint32_t maps_dropped = plans_dropped;
    for (const ShadowPlan& plan : plans) {
        maps_granted += plan.granted ? 1 : 0;
        maps_rendered += plan.render ? 1 : 0;
        maps_dropped += plan.granted ? 0 : 1;
    }
    lighting_stats_.lights = static_cast<std::uint32_t>(kept);
    lighting_stats_.dropped_lights = static_cast<std::uint32_t>(lights.size() - kept);
    lighting_stats_.shadow_maps = maps_granted;
    lighting_stats_.shadow_maps_rendered = maps_rendered;
    lighting_stats_.shadow_maps_dropped = maps_dropped;
    lighting_stats_.shadow_draws = shadow_draw_count;
    lighting_stats_.overflow_clusters = assignment.overflow_clusters;
    lighting_stats_.dropped_entries = assignment.dropped_entries;
    return core::success();
}

RhiStatus SceneRenderer::build_reflections(const SceneShaders& shaders) {
    if (!desc_.post.enabled) {
        return rhi::fail(RhiErrorCode::InvalidState, "screen reflections need the post pass: the reflection pass works on the HDR color");
    }
    if (shaders.reflections.bytecode.empty()) {
        return rhi::fail(RhiErrorCode::InvalidState, "screen reflections need SceneShaders::reflections (CSReflections)");
    }
    if (desc_.reflections.max_steps == 0 || !(desc_.reflections.max_distance > 0.0f)) {
        return rhi::fail(RhiErrorCode::OutOfRange, "screen reflections need at least one step and a positive max_distance");
    }
    // t0 parameters, t1 lit color, t2 depth values, t3 surface, t4 base, and u5 the reflected color: the registers reflections.hlsl reads.
    rhi::DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
    for (std::uint32_t binding = 1; binding <= 4; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
    }
    layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{5, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device_.create_descriptor_set_layout(layout_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    reflection_layout_ = std::move(layout).value();

    rhi::PipelineDesc pipeline;
    pipeline.kind = rhi::PipelineKind::Compute;
    pipeline.shaders = {shaders.reflections};
    pipeline.set_layouts = {reflection_layout_, environment_layout_};
    pipeline.debug_name = "screen reflections";
    RhiResult<rhi::PipelineHandle> created = device_.create_pipeline(pipeline);
    if (!created.ok()) {
        return core::failure(created.error());
    }
    reflection_pipeline_ = std::move(created).value();

    RhiResult<BufferHandle> buffer = device_.create_buffer(buffer_desc(kReflectionBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "reflection parameters"));
    if (!buffer.ok()) {
        return core::failure(buffer.error());
    }
    reflection_params_ = std::move(buffer).value();

    RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(reflection_layout_);
    if (!set.ok()) {
        return core::failure(set.error());
    }
    reflection_set_ = std::move(set).value();
    return write_reflection_set();
}

// Points the reflection set at the current targets. Called after build_targets() and again after every resize, since the targets are new.
RhiStatus SceneRenderer::write_reflection_set() {
    RhiStatus status = device_.write_buffer(reflection_set_, 0, 0, reflection_params_);
    if (status.ok()) {
        status = device_.write_texture(reflection_set_, 1, 0, fog_ ? fogged_ : color_);
    }
    if (status.ok()) {
        status = device_.write_texture(reflection_set_, 2, 0, depth_values_);
    }
    if (status.ok()) {
        status = device_.write_texture(reflection_set_, 3, 0, surface_);
    }
    if (status.ok()) {
        status = device_.write_texture(reflection_set_, 4, 0, base_);
    }
    if (status.ok()) {
        status = device_.write_texture(reflection_set_, 5, 0, reflected_);
    }
    return status;
}

RhiStatus SceneRenderer::upload_gi(const std::vector<std::uint8_t>& bytes, bool active) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_gi_volume: the renderer was released");
    }
    if (!environment_set_.valid()) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_gi_volume: the environment is not set");
    }
    // The new buffer is complete before the set points at it, and the old one is destroyed after that, as set_environment does.
    RhiResult<BufferHandle> created = device_.create_buffer(buffer_desc(bytes.size(), BufferUsage::ShaderResource | BufferUsage::CopyDestination, "probe grid"));
    if (!created.ok()) {
        return core::failure(created.error());
    }
    BufferHandle buffer = std::move(created).value();
    RhiStatus status = upload_buffer_bytes(buffer, bytes.size(), bytes);
    if (status.ok()) {
        status = device_.write_buffer(environment_set_, kGiBinding, 0, buffer);
    }
    if (!status.ok()) {
        RhiStatus ignored = core::success();
        destroy_into(device_, buffer, ignored);
        return status;
    }
    RhiStatus retired = core::success();
    destroy_into(device_, gi_buffer_, retired);
    gi_buffer_ = buffer;
    gi_bytes_ = bytes.size();
    gi_active_ = active;
    return retired;
}

RhiStatus SceneRenderer::set_gi_volume(const gi::GiVolume& volume) {
    if (volume.format_version != gi::kFormatVersion) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "set_gi_volume: the volume's format version is not " + std::to_string(gi::kFormatVersion));
    }
    const std::uint64_t count = static_cast<std::uint64_t>(volume.grid.dims.x) * volume.grid.dims.y * volume.grid.dims.z;
    if (count == 0 || count != volume.probes.size()) {
        return rhi::fail(RhiErrorCode::OutOfRange, "set_gi_volume: the probe count does not match the grid");
    }
    if (!(volume.grid.spacing > 0.0f)) {
        return rhi::fail(RhiErrorCode::OutOfRange, "set_gi_volume: the grid spacing must be positive");
    }
    return upload_gi(pack_gi_volume(&volume), true);
}

RhiStatus SceneRenderer::clear_gi_volume() {
    return upload_gi(pack_gi_volume(nullptr), false);
}

RhiStatus SceneRenderer::set_post_settings(const PostProcessSettings& settings) {
    if (!post_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_post_settings: the renderer has no post pass (SceneRendererDesc::post)");
    }
    post_->set_settings(settings);
    return core::success();
}

RhiStatus SceneRenderer::set_overlay(const ImageRgba8& overlay) {
    if (!post_) {
        return rhi::fail(RhiErrorCode::InvalidState, "set_overlay: the renderer has no post pass (SceneRendererDesc::post)");
    }
    return post_->set_overlay(overlay);
}

RhiStatus SceneRenderer::resize(std::uint32_t width, std::uint32_t height) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "resize: the renderer was released");
    }
    if (width == 0 || height == 0) {
        return rhi::fail(RhiErrorCode::OutOfRange, "resize: the size must be non-zero");
    }
    // Every target and readback is sized by desc_, so they are destroyed and built again at the new size. The pipelines and the frame
    // buffers do not depend on the size.
    RhiStatus first = core::success();
    destroy_into(device_, readback_velocity_, first);
    destroy_into(device_, readback_depth_, first);
    destroy_into(device_, readback_ids_, first);
    destroy_into(device_, readback_color_, first);
    destroy_into(device_, depth_, first);
    destroy_into(device_, velocity_, first);
    destroy_into(device_, depth_values_, first);
    destroy_into(device_, ids_, first);
    destroy_into(device_, color_, first);
    destroy_into(device_, surface_, first);
    destroy_into(device_, base_, first);
    destroy_into(device_, reflected_, first);
    destroy_into(device_, fogged_, first);
    if (!first.ok()) {
        return first;
    }
    desc_.width = width;
    desc_.height = height;
    if (ray_traced_) {
        // The pass's targets are sized to the frame, so the pass is built again at the new size. Its structures do not depend on the size, and
        // they stay.
        if (rt_effects_) {
            if (const RhiStatus released = rt_effects_->release(); !released.ok()) {
                return released;
            }
            rt_effects_.reset();
        }
        RhiResult<std::unique_ptr<RtEffects>> effects = RtEffects::create(device_, width, height, ray_traced_shader_);
        if (!effects.ok()) {
            return core::failure(effects.error());
        }
        rt_effects_ = std::move(effects).value();
        if (denoise_) {
            if (const RhiStatus built = build_denoisers(); !built.ok()) {
                return built;
            }
        }
        if (const RhiStatus bound = bind_ray_traced_outputs(); !bound.ok()) {
            return bound;
        }
    }
    if (const RhiStatus built = build_targets(); !built.ok()) {
        return built;
    }
    if (fog_) {
        if (const RhiStatus built = build_fog(); !built.ok()) {
            return built;
        }
    }
    if (reflection_set_.valid()) {
        if (const RhiStatus written = write_reflection_set(); !written.ok()) {
            return written;
        }
    }
    if (post_) {
        if (const RhiStatus resized = post_->resize(width, height); !resized.ok()) {
            return resized;
        }
    }
    // No frame before this one: the next frame has no motion, and the post pass resets its history. The denoiser and the fog restart too.
    has_history_ = false;
    frame_history_ = false;
    return core::success();
}

RhiStatus SceneRenderer::release() {
    if (released_) {
        return core::success();
    }
    released_ = true;
    RhiStatus first = core::success();
    if (post_) {
        // The post pass owns device objects of its own, so it is released before the device objects of the scene.
        if (const RhiStatus released = post_->release(); !released.ok() && first.ok()) {
            first = released;
        }
        post_.reset();
    }
    for (GpuMesh& mesh : gpu_meshes_) {
        destroy_into(device_, mesh.vertices, first);
        destroy_into(device_, mesh.indices, first);
    }
    gpu_meshes_.clear();
    for (FrameSlot& slot : slots_) {
        destroy_slot(slot, first);
    }
    slots_.clear();
    for (GpuMaterial& material : materials_) {
        destroy_into(device_, material.set, first);
        destroy_into(device_, material.parameters, first);
    }
    materials_.clear();
    destroy_into(device_, environment_set_, first);
    destroy_into(device_, environment_buffer_, first);
    destroy_into(device_, gi_buffer_, first);
    destroy_into(device_, reflection_set_, first);
    destroy_into(device_, reflection_pipeline_, first);
    destroy_into(device_, reflection_params_, first);
    destroy_into(device_, reflected_, first);
    destroy_into(device_, base_, first);
    destroy_into(device_, surface_, first);
    for (rhi::TextureHandle& texture : environment_textures_) {
        destroy_into(device_, texture, first);
    }
    for (GpuTexture& texture : textures_) {
        destroy_into(device_, texture.texture, first);
    }
    textures_.clear();
    destroy_into(device_, readback_velocity_, first);
    destroy_into(device_, readback_depth_, first);
    destroy_into(device_, readback_ids_, first);
    destroy_into(device_, readback_color_, first);
    destroy_into(device_, depth_, first);
    destroy_into(device_, velocity_, first);
    destroy_into(device_, depth_values_, first);
    destroy_into(device_, ids_, first);
    destroy_into(device_, color_, first);
    for (rhi::PipelineHandle& pipeline : pipelines_) {
        destroy_into(device_, pipeline, first);
    }
    destroy_into(device_, meshlet_counts_readback_, first);
    destroy_into(device_, meshlet_counts_, first);
    destroy_into(device_, mesh_vertex_pool_, first);
    destroy_into(device_, meshlet_triangles_, first);
    destroy_into(device_, meshlet_vertices_, first);
    destroy_into(device_, meshlet_records_, first);
    destroy_into(device_, meshlet_layout_, first);
    destroy_into(device_, cull_set_, first);
    destroy_into(device_, cull_pipeline_, first);
    destroy_into(device_, instance_ids_, first);
    destroy_into(device_, counts_readback_, first);
    destroy_into(device_, counts_zero_, first);
    destroy_into(device_, cull_counts_, first);
    destroy_into(device_, cull_args_, first);
    destroy_into(device_, cull_data_, first);
    destroy_into(device_, cull_upload_, first);
    destroy_into(device_, cull_layout_, first);
    destroy_into(device_, shadow_lighting_set_, first);
    destroy_into(device_, shadow_dummy_, first);
    destroy_into(device_, lighting_set_, first);
    // The lighting sets reference the pass's targets, so the pass goes after them.
    destroy_ray_traced(first);
    destroy_into(device_, lighting_upload_, first);
    destroy_into(device_, lighting_buffer_, first);
    destroy_into(device_, local_depth_, first);
    destroy_into(device_, local_color_, first);
    destroy_into(device_, cascade_depth_, first);
    destroy_into(device_, cascade_color_, first);
    destroy_into(device_, lighting_layout_, first);
    destroy_into(device_, environment_layout_, first);
    destroy_into(device_, reflection_layout_, first);
    destroy_into(device_, material_layout_, first);
    destroy_into(device_, frame_layout_, first);
    return first;
}

}  // namespace engine::render
