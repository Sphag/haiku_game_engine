#pragma once

// Ray-traced shadows and single-bounce reflections (US-043). RtEffects runs one compute pass (shaders/rt_effects.hlsl) that traces the
// camera's primary rays against a top-level acceleration structure (US-041 on DX12, US-042 on Vulkan) with inline ray queries. It writes
// two targets, both R16G16B16A16 float, that the caller combines with its own lighting:
//   shadow      x = 1 where the primary surface sees the sun, 0 where a shadow ray hits geometry within shadow_distance. A primary ray
//               that hits nothing writes 1.
//   reflection  rgb = the reflected surface's direct sun and ambient, weighted by Fresnel; a = 1 when the reflection ray hits geometry
//               within reflection_distance, 0 when it does not. A 0 leaves the caller's environment term in place.
// The primary hit also gives the geometry the denoiser needs (US-045), so the denoiser can run before the scene pass that reads the shadow
// and reflection. A ray that hits nothing writes the far depth, an up normal, and no motion.
//   depth       R32 float: the linear view depth of the primary hit, in meters (along the view direction)
//   normal      rgb = the world normal of the primary hit, toward the camera
//   motion      the UV offset from this frame's pixel to the same point's pixel in the previous frame (previous uv = uv + motion). It
//               follows the camera alone: the traced instances are static in the frame they are traced in.
// Only surfaces with roughness at or below roughness_cutoff are reflected. The pass does not shade the primary surface and does not
// trace past one bounce. Rays are not limited to the screen: a reflection of geometry outside the view is traced like any other.
//
// Alpha-mask triangles are tested inside the traversal. A non-opaque candidate is committed only where the mask's alpha in the atlas is at
// least the instance's cutoff, so a masked hole lets the ray through. Geometry classes the pass does not trace are listed by
// rt_effects_geometry_support() and docs/ray-tracing-effects.md.
//
// The CPU reference (trace_reference) runs the same steps on the same records, so the GPU suite compares them pixel by pixel.

#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {

// Sizes of the GPU records the pass reads. The byte layout is documented in shaders/rt_effects.hlsl.
inline constexpr std::uint32_t kRtParameterBytes = 208;
// The depth a primary ray that hits nothing writes (the far distance of the view, in meters).
inline constexpr float kRtMissDepth = 1.0e6f;
inline constexpr std::uint32_t kRtInstanceStride = 64;
inline constexpr std::uint32_t kRtTriangleStride = 64;
// Distance of a primary ray: the camera sees the world at any range, so the camera ray is not limited.
inline constexpr float kRtPrimaryDistance = 1.0e6f;

// One material per TopLevelInstance::instance_id. The material is in world terms only through albedo, metallic, and roughness; the
// geometry comes from the triangle records.
struct RtInstanceRecord {
    glm::mat4 transform{1.0f};   // object to world; affine (the last row is 0 0 0 1)
    glm::vec3 albedo{1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    std::uint32_t triangle_base = 0;   // first triangle of this instance in RtScene::triangles
    std::uint32_t triangle_count = 0;  // triangles of this instance; they follow the bottom-level structure's index order
    bool masked = false;               // alpha-mask: non-opaque candidates are tested against the atlas
    glm::vec4 mask_rect{0.0f, 0.0f, 1.0f, 1.0f};  // the instance's rectangle in the atlas: offset (xy) and size (zw), in UV units
    float mask_cutoff = 0.5f;          // the alpha at or above which a masked texel is opaque
};

// One triangle in object space, with its texture coordinates. Its vertices are the bottom-level structure's vertices at the index
// order of the instance's mesh.
struct RtTriangleRecord {
    std::array<glm::vec3, 3> positions{};
    std::array<glm::vec2, 3> uvs{};
};

// A single-channel alpha image for the CPU reference (row-major, row 0 first). The GPU atlas must hold the same texels.
struct RtMaskImage {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::vector<float> alpha{1.0f};
};

// The scene the pass traces. The caller builds the top-level structure from the instances (make_top_level_instance) and the bottom-level
// structures from the triangle records' mesh, and keeps them built while the pass runs.
struct RtScene {
    rhi::AccelerationStructureHandle top_level;   // built, with one instance per RtInstanceRecord; instance_id is the record index
    std::vector<RtInstanceRecord> instances;
    std::vector<RtTriangleRecord> triangles;
    rhi::TextureHandle mask_atlas;                // ShaderResource usage, in ShaderRead; RGBA8 with the alpha in .a
};

// One frame's view and lighting. forward and up are unit vectors and perpendicular. The sun direction points from the surface to the
// sun. The radiances are linear, in the units the caller's lighting uses.
struct RtFrame {
    glm::vec3 eye{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float fov_y_radians = 1.0471975511965976f;
    float aspect = 1.0f;
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};
    glm::vec3 sun_radiance{1.0f};
    glm::vec3 ambient{0.0f};
    bool shadows = true;
    bool reflections = true;
    float roughness_cutoff = 0.4f;       // surfaces above this roughness are not reflected
    float reflection_distance = 100.0f;  // meters; the reflection ray's length
    float shadow_distance = 1000.0f;     // meters; the shadow ray's length
    float bias = 0.01f;                  // meters; the origin moves this far along the surface normal before each ray
    // The previous frame's camera, in the same space as eye (the traced scene is camera-relative, so this is the previous world position
    // minus the current one). The field of view and the aspect ratio are the frame's own. A frame with no previous camera copies the
    // current one into these, which gives no motion.
    glm::vec3 previous_eye{0.0f};
    glm::vec3 previous_forward{0.0f, 0.0f, -1.0f};
    glm::vec3 previous_up{0.0f, 1.0f, 0.0f};
};

// The camera for a look-at target, with the vertical field of view and the aspect ratio. The up vector is the world's +Y, made
// perpendicular to the view direction.
[[nodiscard]] RtFrame look_at_frame(const glm::vec3& eye, const glm::vec3& target, float fov_y_radians, float aspect);

// Checks a frame's values (finite, unit and perpendicular vectors, a valid field of view, non-negative distances, cutoff in [0, 1]).
// Returns the first problem, or nothing when the frame is usable.
[[nodiscard]] std::optional<std::string> validate_frame(const RtFrame& frame);

// Whether a surface of this roughness is reflected: at or below the cutoff.
[[nodiscard]] bool reflection_applies(float roughness, float cutoff) noexcept;
// Whether a masked texel's alpha keeps the candidate: at or above the cutoff.
[[nodiscard]] bool alpha_passes(float alpha, float cutoff) noexcept;

// The byte images the pass uploads. Each is the layout that shaders/rt_effects.hlsl reads.
[[nodiscard]] std::vector<std::uint8_t> pack_frame_parameters(const RtFrame& frame, std::uint32_t width, std::uint32_t height);
[[nodiscard]] std::vector<std::uint8_t> pack_instance_table(const std::vector<RtInstanceRecord>& instances);
[[nodiscard]] std::vector<std::uint8_t> pack_triangle_table(const std::vector<RtTriangleRecord>& triangles);

// The top-level instance for one record: its transform (row-major 3x4), its instance_id (the record index), and its bottom-level structure.
[[nodiscard]] rhi::TopLevelInstance make_top_level_instance(const RtInstanceRecord& record, std::uint32_t index,
                                                            rhi::AccelerationStructureHandle bottom_level);

// Checks a scene's records: every instance's triangle range lies inside the triangle table, and the masks have a usable cutoff.
[[nodiscard]] std::optional<std::string> validate_scene_records(const std::vector<RtInstanceRecord>& instances,
                                                                const std::vector<RtTriangleRecord>& triangles);

// CPU reference: the pass's steps, run on the records. The shadow and reflection values match the shader's outputs. primary_instance is
// the instance a primary ray hits (-1 for none), so a test can show that a reflected object is outside the view.
struct RtReference {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> shadow;                  // x of the shadow target
    std::vector<glm::vec4> reflection;          // the reflection target (rgb, a)
    std::vector<std::int32_t> primary_instance; // -1 where the primary ray hits nothing
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept { return static_cast<std::size_t>(y) * width + x; }
};
[[nodiscard]] RtReference trace_reference(const std::vector<RtInstanceRecord>& instances, const std::vector<RtTriangleRecord>& triangles,
                                          const RtMaskImage& mask, const RtFrame& frame, std::uint32_t width, std::uint32_t height);

// The geometry classes the pass handles, and the ones it does not. The capability report lists them; an unsupported class is not traced.
struct GeometrySupport {
    std::string_view geometry;
    bool supported = false;
    std::string_view note;
};
[[nodiscard]] const std::vector<GeometrySupport>& rt_effects_geometry_support();
// Whether the report lists the class and marks it supported.
[[nodiscard]] bool geometry_supported(std::string_view geometry);

// The pass on a device. create() needs the RayTracing feature and fails with FeatureDisabled otherwise. The pass owns its descriptor
// set, parameter buffer, instance and triangle buffers, and its two output targets. Scene and frame uploads are synchronous (they go
// through the render graph's execute, which waits), so call them between frames.
class RtEffects {
public:
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<RtEffects>> create(rhi::Device& device, std::uint32_t width, std::uint32_t height,
                                                                          rhi::ShaderBinary shader);
    RtEffects(const RtEffects&) = delete;
    RtEffects& operator=(const RtEffects&) = delete;
    ~RtEffects() = default;

    // Binds the scene: uploads the instance and triangle tables and writes the descriptors. Replaces an earlier scene.
    [[nodiscard]] rhi::RhiStatus set_scene(const RtScene& scene);
    // Uploads the parameters of one frame. Needs a scene.
    [[nodiscard]] rhi::RhiStatus update_frame(const RtFrame& frame);
    // Records the dispatch. Both outputs start in Common and end in ShaderRead. Needs a scene and a frame, and a top-level structure that
    // the list built before this call.
    [[nodiscard]] rhi::RhiStatus record(rhi::CommandList& list);

    [[nodiscard]] rhi::TextureHandle shadow() const noexcept { return shadow_; }
    [[nodiscard]] rhi::TextureHandle reflection() const noexcept { return reflection_; }
    [[nodiscard]] rhi::TextureHandle depth() const noexcept { return depth_; }
    [[nodiscard]] rhi::TextureHandle normal() const noexcept { return normal_; }
    [[nodiscard]] rhi::TextureHandle motion() const noexcept { return motion_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    // Destroys every handle the pass owns. Safe to call once; the RHI retires the storage behind its fence.
    [[nodiscard]] rhi::RhiStatus release();

private:
    RtEffects(rhi::Device& device, std::uint32_t width, std::uint32_t height) noexcept;

    rhi::Device& device_;
    std::uint32_t width_;
    std::uint32_t height_;
    rhi::DescriptorSetLayoutHandle layout_;
    rhi::DescriptorSetHandle set_;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle parameters_;
    rhi::BufferHandle instances_;
    rhi::BufferHandle triangles_;
    rhi::TextureHandle shadow_;
    rhi::TextureHandle reflection_;
    rhi::TextureHandle depth_;
    rhi::TextureHandle normal_;
    rhi::TextureHandle motion_;
    rhi::ResourceState outputs_state_ = rhi::ResourceState::Common;
    bool scene_set_ = false;
    bool frame_set_ = false;
    bool parameters_uploaded_ = false;
};

}  // namespace engine::render
