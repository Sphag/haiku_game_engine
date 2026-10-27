#pragma once

// Temporal upscaling contract (US-046): the two image profiles' fixed resolutions, the FSR 2 Quality ratio that derives the full profile's
// internal size, the FSR 2 jitter sequence (its phase count depends on that ratio, so it is not the 8-phase TAA sequence of postprocess.hpp),
// the history reset rule for the upscaler's own history, and the benchmark policy that forbids frame generation and dynamic resolution.
// The GPU upscaler (FSR 2 on DX12 and Vulkan) takes its sizes, jitter, and reset decisions from these functions. docs/upscaling.md
// describes the contract and what is not yet wired.
#include "engine/render/postprocess.hpp"

#include <cstdint>

namespace engine::render {

// The two image profiles of the PRD. Both output 1920x1080.
enum class ImageProfile : std::uint8_t {
    Native1080,  // native_1080: internal 1920x1080, TAA, no upscaling
    Full1080,    // full_1080: internal 1280x720 fixed, FSR 2 Quality
};

inline constexpr std::uint32_t kOutputWidth = 1920;
inline constexpr std::uint32_t kOutputHeight = 1080;

// FSR 2's per-dimension ratio for FFX_FSR2_QUALITY_MODE_QUALITY. Balanced, Performance, and Ultra Performance are not used by any profile.
inline constexpr float kFsrQualityRatio = 1.5f;

struct ProfileResolution {
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t internal_width = 0;   // the size the scene is rendered at, before any upscaling
    std::uint32_t internal_height = 0;
    bool upscale = false;               // true when FSR 2 runs between the scene and the UI
};

// The fixed internal size of a profile. The full profile's size is the output divided by kFsrQualityRatio, truncated as FSR 2 does it.
[[nodiscard]] ProfileResolution profile_resolution(ImageProfile profile) noexcept;

// FSR 2's render size for a display size at `ratio` per dimension (ffxFsr2GetRenderResolutionFromQualityMode uses the same truncation).
[[nodiscard]] std::uint32_t fsr_render_extent(std::uint32_t display_extent, float ratio) noexcept;

// Jitter phases for an internal width and output width (ffxFsr2GetJitterPhaseCount): int(8 * (display / render)^2). The full profile's
// 1280-to-1920 upscale gives 18 phases.
[[nodiscard]] std::uint32_t fsr_jitter_phase_count(std::uint32_t render_width, std::uint32_t display_width) noexcept;

// The FSR 2 jitter for a frame, in internal pixels, each component in [-0.5, 0.5] (ffxFsr2GetJitterOffset). Index 0 is skipped as in
// taa_jitter. A phase count of 0 returns no jitter.
[[nodiscard]] JitterOffset fsr_jitter(std::uint64_t frame_index, std::uint32_t phase_count) noexcept;

// The benchmark policy (docs/upscaling.md): frame generation and dynamic resolution are disabled, so a benchmark run measures the profile's
// fixed resolution and never changes quality during the run.
struct UpscalePolicy {
    bool frame_generation = false;
    bool dynamic_resolution = false;
};

[[nodiscard]] bool benchmark_policy_allowed(const UpscalePolicy& policy) noexcept;

// The upscaler's own history (FSR 2 keeps its reconstructed history on the GPU). The reset rule is the TAA rule of taa_history_reset: the
// first frame, a size change, and a flagged camera cut or teleport each discard it. The GPU upscaler calls this once per frame before it
// dispatches.
class UpscaleHistory {
public:
    [[nodiscard]] HistoryReset begin_frame(const TaaFrameInput& input);

    [[nodiscard]] bool has_history() const noexcept {
        return has_history_;
    }

    // Called after a frame is dispatched, so the next frame can keep this one's history.
    void frame_done() noexcept {
        has_history_ = true;
    }

private:
    bool has_history_ = false;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

}  // namespace engine::render
