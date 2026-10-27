#pragma once

// In-house temporal and spatial denoiser for the ray-traced signals (US-045). TemporalDenoiser filters one target at a time (the RT shadow
// or the RT reflection of rt_effects.hpp). Each pixel first gets a depth- and normal-aware box prefilter over spatial_radius. Then it
// reprojects into the previous frame with the motion vector and keeps the history only where the previous sample's linear depth and
// normal agree with the current pixel's (disocclusion and history rejection). An accepted sample blends toward the filtered value with a
// weight that is never below 1 / (age + 1), where age counts the accepted frames up to max_age. So a stale history loses its weight every
// frame and cannot persist. A rejected pixel restarts from its filtered value. A reset (a camera cut, or a light that moved; see
// camera_cut and light_changed) discards the whole history.
//
// Inputs, per pixel, all in the frame's own resolution: the noisy value (rgb, a), the linear view depth in meters (positive ahead), the world
// normal (unit), and the motion vector (the UV offset from this frame to the previous one: previous uv = uv + motion).
//
// The CPU reference (denoise_reference) runs the same steps on the same values, so the GPU suite compares them.

#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {

inline constexpr std::uint32_t kDenoiseParameterBytes = 32;
// The largest spatial radius; the prefilter reads (2 * radius + 1)^2 pixels.
inline constexpr std::uint32_t kDenoiseMaxRadius = 2;

struct DenoiseSettings {
    float history_weight = 0.9f;      // the largest share of the history in a settled pixel, in [0, 1)
    std::uint32_t max_age = 15;       // the most accepted frames the history counts; its share never falls below 1 / (max_age + 1) per frame
    float depth_tolerance = 0.05f;    // the relative linear-depth difference a history sample may have
    float normal_threshold = 0.9f;    // the smallest dot product of the current and the history normal
    std::uint32_t spatial_radius = 1; // the prefilter's half-width in pixels, 0 to kDenoiseMaxRadius
};

// Checks the settings' ranges. Returns the first problem, or nothing when they are usable.
[[nodiscard]] std::optional<std::string> validate_denoise_settings(const DenoiseSettings& settings);

// A camera's pose. A cut is a jump the history cannot follow.
struct CameraPose {
    glm::vec3 eye{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};  // unit
};
// Whether the camera moved more than max_translation meters or turned more than max_angle_degrees between the two poses.
[[nodiscard]] bool camera_cut(const CameraPose& previous, const CameraPose& current, float max_translation, float max_angle_degrees);
// Whether a light's direction (unit, toward the light) turned more than max_angle_degrees. The light-dependent history resets then.
[[nodiscard]] bool light_changed(const glm::vec3& previous_direction, const glm::vec3& direction, float max_angle_degrees);

// The per-pixel geometry the denoiser reads and keeps in the history, with the age of the value it belongs to.
struct DenoiseGeometry {
    float depth = 0.0f;     // linear view depth, meters
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float age = 0.0f;       // accepted frames of the value, 1 for a restart, up to max_age
};

// One frame of the noisy signal, on the CPU. Each vector holds width * height entries, row 0 first.
struct DenoiseTarget {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<glm::vec4> value;
    std::vector<float> depth;
    std::vector<glm::vec3> normal;
    std::vector<glm::vec2> motion;
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept { return static_cast<std::size_t>(y) * width + x; }
};

// The history from the previous frame: the accumulated value (rgba, as the signal) and the geometry it was accepted with, whose age
// counts the accepted frames.
struct DenoiseHistory {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<glm::vec4> value;
    std::vector<DenoiseGeometry> geometry;
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept { return static_cast<std::size_t>(y) * width + x; }
};

struct DenoiseReference {
    std::vector<glm::vec4> output;          // the denoised value (rgba)
    DenoiseHistory history;                 // the history for the next frame
    std::vector<std::uint8_t> accepted;     // 1 where the pixel used its history, 0 where it restarted
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept { return static_cast<std::size_t>(y) * history.width + x; }
};

// The CPU reference of one frame. `previous` is the last frame's history; an invalid or differently sized history restarts. `reset`
// discards it for this frame as well.
[[nodiscard]] DenoiseReference denoise_reference(const DenoiseTarget& target, const DenoiseHistory& previous, const DenoiseSettings& settings,
                                                 bool reset);

// Packs the frame parameters the shader reads (layout in shaders/denoise.hlsl).
[[nodiscard]] std::vector<std::uint8_t> pack_denoise_parameters(const DenoiseSettings& settings, std::uint32_t width, std::uint32_t height,
                                                                bool reset, bool history_valid);

// The inputs the GPU pass reads. Each texture is in ShaderResource usage and in ShaderRead when record() runs.
struct DenoiseInputs {
    rhi::TextureHandle value;    // R16G16B16A16Float: the noisy signal
    rhi::TextureHandle depth;    // R32Float: linear view depth
    rhi::TextureHandle normal;   // R16G16B16A16Float: world normal in xyz
    rhi::TextureHandle motion;   // R16G16B16A16Float (xy): the UV offset to the previous frame
};

// The GPU pass on a device. The pass owns two ping-ponged sets of history textures (value, depth, normal) and its parameter buffer. Each
// record() reads the previous history and writes the next one, so the output is the history it wrote.
class TemporalDenoiser {
public:
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<TemporalDenoiser>> create(rhi::Device& device, std::uint32_t width, std::uint32_t height,
                                                                                  rhi::ShaderBinary shader);
    TemporalDenoiser(const TemporalDenoiser&) = delete;
    TemporalDenoiser& operator=(const TemporalDenoiser&) = delete;
    ~TemporalDenoiser() = default;

    // Binds the inputs. Replaces an earlier binding; the inputs are read at record() time.
    [[nodiscard]] rhi::RhiStatus set_inputs(const DenoiseInputs& inputs);
    // Uploads this frame's settings. `reset` discards the history for this frame. Needs no earlier call.
    [[nodiscard]] rhi::RhiStatus update_frame(const DenoiseSettings& settings, bool reset);
    // Records the dispatch and advances the history. Needs inputs and a frame. Leaves the output in ShaderRead.
    [[nodiscard]] rhi::RhiStatus record(rhi::CommandList& list);

    // The denoised value of the last record() (rgba). Invalid before the first record.
    [[nodiscard]] rhi::TextureHandle output() const noexcept;
    // The texture the next record() writes. A caller that reads the output in the same frame binds it before the record runs.
    [[nodiscard]] rhi::TextureHandle next_output() const noexcept { return value_[1 - parity_]; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    // Destroys every handle the pass owns. The RHI retires the storage behind its fence.
    [[nodiscard]] rhi::RhiStatus release();

private:
    TemporalDenoiser(rhi::Device& device, std::uint32_t width, std::uint32_t height) noexcept;

    rhi::Device& device_;
    std::uint32_t width_;
    std::uint32_t height_;
    rhi::DescriptorSetLayoutHandle layout_;
    rhi::PipelineHandle pipeline_;
    rhi::DescriptorSetHandle sets_[2];   // sets_[p] reads history p and writes history 1 - p
    rhi::BufferHandle parameters_;
    rhi::TextureHandle value_[2];
    rhi::TextureHandle depth_[2];
    rhi::TextureHandle normal_[2];
    rhi::ResourceState value_state_[2] = {rhi::ResourceState::Common, rhi::ResourceState::Common};
    rhi::ResourceState depth_state_[2] = {rhi::ResourceState::Common, rhi::ResourceState::Common};
    rhi::ResourceState normal_state_[2] = {rhi::ResourceState::Common, rhi::ResourceState::Common};
    std::uint32_t parity_ = 0;   // the history the last record wrote
    bool history_valid_ = false;
    bool inputs_set_ = false;
    bool frame_set_ = false;
    bool parameters_uploaded_ = false;
};

}  // namespace engine::render
