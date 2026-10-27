#pragma once

// HDR postprocessing (US-039): the sub-pixel jitter sequence for TAA, exposure from EV100, bloom prefilter and blur weights, the ACES
// filmic tone curve, the reference overlay composite, and the TAA history with its reset rules. These functions are the CPU reference of
// the scene's post pass; the shader implements the same formulas. docs/postprocess.md describes the contracts.
//
// Conventions: scene color is linear HDR (RGB, unbounded). The velocity target is in UV units (x right, y down), as scene.hlsl writes it.
// Jitter is in pixels, with y down, so both backends shift the image by the same amount on screen (see taa_jitter_ndc).

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {

// ---- Jitter -----------------------------------------------------------------------------------------------------------------------------

// Sub-pixel offset in pixels, each component in [-0.5, 0.5], y down.
struct JitterOffset {
    float x = 0.0f;
    float y = 0.0f;
};

// Number of phases in the jitter sequence. The sequence repeats every this many frames.
inline constexpr std::uint32_t kJitterPhases = 8;

// Halton(2, 3) sample for the frame, centred on the pixel. The same offset is returned for the same frame index on every backend.
[[nodiscard]] JitterOffset taa_jitter(std::uint64_t frame_index) noexcept;
// The jitter as an NDC offset to add to a projection's clip x and y terms (before the perspective divide adds it to the image). x is
// the same on both backends. y depends on the backend's NDC row order: tile_row_sign (shadow_atlas.hpp) gives the sign that maps NDC y
// to a row, and the offset follows it, so a pixel shift moves the image the same way on DX12 and Vulkan.
[[nodiscard]] glm::vec2 taa_jitter_ndc(JitterOffset jitter, std::uint32_t width, std::uint32_t height, bool vulkan_clip) noexcept;

// ---- Exposure ---------------------------------------------------------------------------------------------------------------------------

struct ExposureSettings {
    float min_ev100 = -4.0f;  // darkest exposure the auto mode may choose, in EV100
    float max_ev100 = 16.0f;  // brightest, in EV100
    float ev_compensation = 0.0f;  // artist offset added to the chosen EV100
};

// Exposure multiplier for an EV100: 1 / (1.2 * 2^EV100), the saturation-based standard. Non-finite input gives 1.
[[nodiscard]] float exposure_from_ev100(float ev100) noexcept;
// EV100 that exposes a scene with average luminance `average_luminance` (cd/m^2 scale of the linear input). The EV is clamped to the
// settings' range, then the artist compensation is added. A zero, negative, or non-finite luminance gives the darkest allowed value
// (an empty or NaN frame must not blow up exposure).
[[nodiscard]] float ev100_from_luminance(float average_luminance, const ExposureSettings& settings) noexcept;

// ---- Color hygiene ---------------------------------------------------------------------------------------------------------------------

// Replaces NaN and Inf components with 0 and clamps negatives to 0. Applied to every scene sample before it is filtered, so one bad
// pixel cannot spread through bloom or temporal history.
[[nodiscard]] glm::vec3 sanitize_color(glm::vec3 color) noexcept;

// ---- Bloom ------------------------------------------------------------------------------------------------------------------------------

struct BloomSettings {
    float threshold = 1.0f;  // luminance where bloom starts
    float knee = 0.5f;       // width of the soft transition around the threshold, >= 0
    float intensity = 0.1f;  // weight of the bloom added back in the composite
};

// Soft-threshold prefilter (the Karis knee): the color's contribution is zero below threshold - knee, ramps up across the knee, and is
// the full color above threshold + knee. Returns color unchanged when the threshold is 0 and knee is 0.
[[nodiscard]] glm::vec3 bloom_prefilter(glm::vec3 color, const BloomSettings& settings) noexcept;
// Normalized one-dimensional Gaussian weights for taps from -radius to +radius, centred. The sum of the weights is 1. radius is >= 0;
// sigma is taken as radius / 2 (the tap at the edge is near 2 sigma).
[[nodiscard]] std::vector<float> bloom_blur_weights(std::uint32_t radius);

// ---- Tone mapping and overlay -----------------------------------------------------------------------------------------------------------

// ACES filmic curve (Narkowicz fit), applied to linear color that is already multiplied by exposure. The result is clamped to [0, 1]
// for display. Input is sanitized first.
[[nodiscard]] glm::vec3 tonemap_aces(glm::vec3 exposed_color) noexcept;
// Linear [0, 1] to sRGB encoding, the same curve the scene shader uses for the 8-bit target. Input is sanitized and clamped first.
[[nodiscard]] glm::vec3 srgb_encode(glm::vec3 linear) noexcept;
// Scene post pass for one pixel: exposure, bloom added in linear light, then tone mapping. The reference overlay is composited after
// this, so it never sees the scene exposure.
[[nodiscard]] glm::vec3 postprocess_pixel(glm::vec3 scene_color, glm::vec3 bloom, float exposure, float bloom_intensity) noexcept;
// Source-over blend of a UI or reference overlay (straight alpha, already in display space) onto a tone-mapped pixel.
[[nodiscard]] glm::vec3 composite_overlay(glm::vec3 display_color, glm::vec4 overlay) noexcept;

// ---- TAA history ------------------------------------------------------------------------------------------------------------------------

// Why the temporal history was discarded this frame. None means the history was kept.
enum class HistoryReset : std::uint8_t {
    None,
    FirstFrame,   // there was no history yet
    Resize,       // the render target size changed
    CameraCut,    // the caller flagged a cut (the camera jumped to a new shot)
    Teleport,     // the caller flagged a teleport (the camera moved beyond the teleport distance in one frame)
};

struct TaaFrameInput {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool camera_cut = false;
    bool teleport = false;
};

struct TaaSettings {
    // Weight of the current sample in the blend, in (0, 1]. Lower values keep more history (smoother, slower to react).
    float current_weight = 0.1f;
};

// The reset rule of TaaHistory::begin_frame, for a history kept elsewhere (the GPU post pass keeps its own). `has_history` says whether a
// history exists, and previous_width/height are the size it was resolved at. Returns the reason the history is discarded, or None.
[[nodiscard]] HistoryReset taa_history_reset(bool has_history, std::uint32_t previous_width, std::uint32_t previous_height,
                                             const TaaFrameInput& input) noexcept;

class TaaHistory {
public:
    // Starts a frame. Resets the history when there is none, when the size changed, or when a cut or teleport was flagged, and returns
    // the reason (or None). A reset frame resolves to its current color unchanged.
    [[nodiscard]] HistoryReset begin_frame(const TaaFrameInput& input);

    // Resolves the frame's sanitized current color with the history. `velocity` holds the UV motion of each pixel (x right, y down, the
    // current position minus the previous one). Per pixel: the history is sampled at the previous position, clamped to the 3x3
    // neighborhood's color box of the current frame (which removes ghosts of disoccluded or moved content), and blended with the current
    // sample. A history sample that falls outside the target is rejected. The result becomes the history for the next frame.
    // Both vectors must hold width * height entries; a mismatched size returns the sanitized current color and keeps no history.
    [[nodiscard]] std::vector<glm::vec3> resolve(const std::vector<glm::vec3>& current, const std::vector<glm::vec2>& velocity,
                                                 const TaaSettings& settings);

    [[nodiscard]] bool has_history() const noexcept {
        return has_history_;
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool has_history_ = false;
    std::vector<glm::vec3> history_;
    HistoryReset pending_reset_ = HistoryReset::None;
};

// Camera motion larger than kTeleportDistance world units between two frames counts as a teleport.
inline constexpr float kTeleportDistance = 50.0f;
[[nodiscard]] bool is_teleport(glm::vec3 previous_camera, glm::vec3 current_camera) noexcept;

}  // namespace engine::render
