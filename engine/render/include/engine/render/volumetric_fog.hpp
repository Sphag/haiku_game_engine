#pragma once

// Froxel volumetric fog (US-045). The view frustum is cut into a grid of froxels: width x height columns of pixels, and depth slices whose
// far edges grow exponentially from near to far. Each froxel holds the in-scattered radiance per unit length at its centre, and the grid is
// integrated front to back along each column into an accumulated radiance and a transmittance.
//
// Lighting per froxel: a directional sun and up to kFogMaxLights local point lights. Each light term is scaled by a Henyey-Greenstein phase
// of the scattering angle and by a visibility: 1 when the segment from the froxel to the light is clear, 0 when it is blocked. The GPU
// traces the visibility with inline ray queries against the top-level structure, so a shadowed light needs the RayTracing feature. The
// density falls off exponentially with height; the extinction equals the density.
//
// Temporal stabilisation: each froxel reprojects its centre into the previous frame's grid, and keeps the previous value where that sample
// is inside the grid and the reset flag is off. The blend weight is never below 1 / (age + 1), where age counts the accepted frames up to
// max_age, so a stale value loses its weight every frame. A camera cut or a reset discards the history.
//
// The pass runs as three compute passes, each registered with the render graph by the caller: an inject pass (lighting and temporal blend),
// an integrate pass (the column accumulation), and an apply pass that adds the fog to a lit frame (apply_fog). The froxel grid is a buffer,
// because the RHI has no volume textures; a froxel occupies 16 bytes at ((z * height + y) * width + x) * 16.
// The apply pass maps each pixel to its grid column and reads the scene's NDC depth (0 where nothing was drawn, so the fog reaches the far
// plane there). The camera's depth range (FogFrame::camera_near, camera_far) turns that depth into view depth.
//
// The CPU reference (inject_reference, integrate_reference) runs the same steps on the same values, with a caller-supplied visibility.

#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {

// The PRD's full-profile grid (Volumetric fog: 160 x 90 x 64).
inline constexpr std::uint32_t kFogFullWidth = 160;
inline constexpr std::uint32_t kFogFullHeight = 90;
inline constexpr std::uint32_t kFogFullDepth = 64;
inline constexpr std::uint32_t kFogMaxLights = 16;
inline constexpr std::uint32_t kFogFroxelBytes = 16;
inline constexpr std::uint32_t kFogParameterBytes = 768;

// The distance a sun shadow ray travels: the sun is at infinity.
inline constexpr float kFogSunDistance = 1.0e5f;

// A local point light. The radiance is linear; the light falls off as 1 / (1 + d^2) inside its range, with a window that reaches zero at
// the range.
struct FogLight {
    glm::vec3 position{0.0f};
    glm::vec3 radiance{0.0f};
    float range = 10.0f;     // meters
    bool shadowed = false;   // traces a visibility ray (GPU) or asks the visibility function (CPU)
};

struct FogFrame {
    // The camera. forward is a unit vector; right and up are the view's half extents at unit depth: right = unit_right * tan(fov/2) * aspect,
    // up = unit_up * tan(fov/2). A froxel's centre is at eye + d * (forward + x * right + y * up) for view depth d and NDC (x, y).
    glm::vec3 eye{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    // The previous frame's camera, in the same convention, for the reprojection.
    glm::vec3 previous_eye{0.0f};
    glm::vec3 previous_forward{0.0f, 0.0f, -1.0f};
    glm::vec3 previous_right{1.0f, 0.0f, 0.0f};
    glm::vec3 previous_up{0.0f, 1.0f, 0.0f};
    // The sun. The direction is a unit vector toward the sun; the radiance is linear and zero for no sun.
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};
    glm::vec3 sun_radiance{0.0f};
    // The medium. albedo is the scattering fraction (the single-scattering albedo, 0 to 1 per channel); density is the extinction at
    // height zero in 1/m, falling off as exp(-height_falloff * y).
    glm::vec3 albedo{1.0f};
    float density = 0.02f;
    float height_falloff = 0.0f;
    float anisotropy = 0.5f;   // Henyey-Greenstein g, in (-1, 1)
    float near_plane = 0.5f;   // meters; the first slice starts here
    float far_plane = 200.0f;  // meters; the last slice ends here
    // The camera's depth range, which the apply pass uses to turn the scene's NDC depth into view depth (meters).
    float camera_near = 0.1f;
    float camera_far = 1000.0f;
    // Temporal stabilisation.
    bool shadows = true;
    bool reset = false;
    float history_weight = 0.9f;
    std::uint32_t max_age = 15;
    std::vector<FogLight> lights;
};

// The visibility of the segment from `from` to `to`: 1 when clear, 0 when blocked. The CPU reference asks it for the sun (to = from +
// sun_direction * kFogSunDistance) and for each shadowed light.
using FogVisibility = std::function<float(const glm::vec3& from, const glm::vec3& to)>;

// A grid on the CPU: the radiance per unit length and the age (rgb, a) when injected, the accumulated radiance and transmittance when
// integrated. Row-major by slice, then row, then column.
struct FogVolume {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::vector<glm::vec4> cells;
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept {
        return (static_cast<std::size_t>(z) * height + y) * width + x;
    }
};

// Checks the grid and the frame: a non-zero grid, a finite camera, a valid medium, a near plane below the far plane, a history weight in
// [0, 1), and at most kFogMaxLights lights with finite values and positive ranges. Returns the first problem, or nothing.
[[nodiscard]] std::optional<std::string> validate_fog(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth);

// The view depth of the boundary of slice `z` (0 to depth): near * (far / near)^(z / depth).
[[nodiscard]] float fog_slice_edge(const FogFrame& frame, std::uint32_t depth, float slice) noexcept;
// The density at a world height (the extinction per meter).
[[nodiscard]] float fog_density(const FogFrame& frame, float height) noexcept;
// Henyey-Greenstein phase of the cosine between the view ray's direction and the direction toward the light. Forward scattering peaks at 1.
[[nodiscard]] float fog_phase(float cosine, float anisotropy) noexcept;
// The world position of a froxel's centre.
[[nodiscard]] glm::vec3 fog_froxel_center(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t x,
                                          std::uint32_t y, std::uint32_t z) noexcept;

// The byte image the inject pass reads (layout in shaders/volumetric_fog.hlsl).
[[nodiscard]] std::vector<std::uint8_t> pack_fog_parameters(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                                            bool history_valid);

// The CPU reference of the inject pass. `previous` is last frame's injected grid; an empty or differently sized grid restarts.
[[nodiscard]] FogVolume inject_reference(const FogFrame& frame, const FogVolume& previous, std::uint32_t width, std::uint32_t height,
                                         std::uint32_t depth, const FogVisibility& visibility);
// The CPU reference of the integrate pass: the accumulated radiance and the transmittance at each slice's far edge.
[[nodiscard]] FogVolume integrate_reference(const FogFrame& frame, const FogVolume& injected);
// Applies the fog to a pixel's colour: the accumulated radiance is added to the attenuated colour. view_depth is the pixel's depth in meters.
[[nodiscard]] glm::vec3 apply_fog(const glm::vec3& color, const FogVolume& integrated, const FogFrame& frame, std::uint32_t x, std::uint32_t y,
                                  float view_depth) noexcept;
// The view depth (meters) of a scene's NDC depth for the camera's range, as the apply pass computes it. NDC 0 is the cleared background
// and maps to the far plane.
[[nodiscard]] float fog_scene_view_depth(float ndc_depth, float camera_near, float camera_far) noexcept;

// The targets of the apply pass: the lit colour (read), the scene's NDC depth (read), and the output (written), all of one size. The
// caller declares their states in its graph (the colour and depth in ShaderRead, the output in StorageWrite).
struct FogApplyTargets {
    rhi::TextureHandle color;   // R16G16B16A16Float: the lit frame
    rhi::TextureHandle depth;   // R32Float: the scene's NDC depth, 0 where nothing was drawn
    rhi::TextureHandle output;  // R16G16B16A16Float: the frame with the fog
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The GPU passes on a device. Needs the RayTracing feature, because the inject pass traces the shadows of the sun and the shadowed lights.
// The pass owns its parameter buffer, two injected grids that are ping-ponged, and the integrated grid.
class VolumetricFog {
public:
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<VolumetricFog>> create(rhi::Device& device, std::uint32_t width, std::uint32_t height,
                                                                               std::uint32_t depth, rhi::ShaderBinary inject_shader,
                                                                               rhi::ShaderBinary integrate_shader, rhi::ShaderBinary apply_shader);
    VolumetricFog(const VolumetricFog&) = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;
    ~VolumetricFog() = default;

    // Binds the top-level structure that the shadow rays trace. It must be built when inject() records. Replaces an earlier binding.
    [[nodiscard]] rhi::RhiStatus set_scene(rhi::AccelerationStructureHandle top_level);
    // Uploads this frame's parameters. Needs no earlier call.
    [[nodiscard]] rhi::RhiStatus update_frame(const FogFrame& frame);
    // Records the inject pass: the lighting, the shadows, and the temporal blend. Needs a scene and a frame. Leaves the injected grid in
    // ShaderRead.
    [[nodiscard]] rhi::RhiStatus inject(rhi::CommandList& list);
    // Records the integrate pass over the grid that inject() wrote last. Needs inject() first. Leaves the integrated grid in ShaderRead.
    [[nodiscard]] rhi::RhiStatus integrate(rhi::CommandList& list);
    // Binds the apply pass's targets. Call it when they change (a resize); the descriptors are written now.
    [[nodiscard]] rhi::RhiStatus set_apply_targets(const FogApplyTargets& targets);
    // Records the apply pass: the fog of the integrated grid is added to each pixel of the colour target. Needs integrate() and the targets.
    [[nodiscard]] rhi::RhiStatus apply(rhi::CommandList& list);

    // The integrated grid (rgb = accumulated radiance, a = transmittance) of the last integrate(). Invalid before the first one.
    [[nodiscard]] rhi::BufferHandle integrated() const noexcept { return integrated_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }

    // Destroys every handle the pass owns. The RHI retires the storage behind its fence.
    [[nodiscard]] rhi::RhiStatus release();

private:
    VolumetricFog(rhi::Device& device, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept;

    rhi::Device& device_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t depth_;
    rhi::DescriptorSetLayoutHandle inject_layout_;
    rhi::DescriptorSetLayoutHandle integrate_layout_;
    rhi::DescriptorSetLayoutHandle apply_layout_;
    rhi::PipelineHandle inject_pipeline_;
    rhi::PipelineHandle integrate_pipeline_;
    rhi::PipelineHandle apply_pipeline_;
    rhi::DescriptorSetHandle inject_sets_[2];     // inject_sets_[p] reads grid p and writes grid 1 - p
    rhi::DescriptorSetHandle integrate_sets_[2];  // integrate_sets_[p] reads grid p
    rhi::DescriptorSetHandle apply_set_;
    std::uint32_t apply_width_ = 0;
    std::uint32_t apply_height_ = 0;
    bool apply_bound_ = false;
    rhi::BufferHandle parameters_;
    rhi::BufferHandle grids_[2];
    rhi::BufferHandle integrated_;
    rhi::ResourceState grid_state_[2] = {rhi::ResourceState::Common, rhi::ResourceState::Common};
    rhi::ResourceState integrated_state_ = rhi::ResourceState::Common;
    rhi::AccelerationStructureHandle top_level_;
    std::uint32_t parity_ = 0;   // the grid inject() wrote last
    bool history_valid_ = false;
    bool scene_set_ = false;
    bool frame_set_ = false;
    bool parameters_uploaded_ = false;
    bool injected_ = false;
    bool integrated_once_ = false;
};

}  // namespace engine::render
