#include "engine/render/upscaler.hpp"

namespace engine::render {

namespace {

// Radical inverse of `index` in `base`, in [0, 1). The same form as the Halton helper of postprocess.cpp and FSR 2's own.
float halton(std::uint32_t index, std::uint32_t base) noexcept {
    float fraction = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

}  // namespace

ProfileResolution profile_resolution(ImageProfile profile) noexcept {
    ProfileResolution resolution;
    resolution.output_width = kOutputWidth;
    resolution.output_height = kOutputHeight;
    switch (profile) {
        case ImageProfile::Native1080:
            resolution.internal_width = kOutputWidth;
            resolution.internal_height = kOutputHeight;
            resolution.upscale = false;
            break;
        case ImageProfile::Full1080:
            resolution.internal_width = fsr_render_extent(kOutputWidth, kFsrQualityRatio);
            resolution.internal_height = fsr_render_extent(kOutputHeight, kFsrQualityRatio);
            resolution.upscale = true;
            break;
    }
    return resolution;
}

std::uint32_t fsr_render_extent(std::uint32_t display_extent, float ratio) noexcept {
    if (ratio <= 0.0f) {
        return 0;
    }
    return static_cast<std::uint32_t>(static_cast<float>(display_extent) / ratio);
}

std::uint32_t fsr_jitter_phase_count(std::uint32_t render_width, std::uint32_t display_width) noexcept {
    if (render_width == 0) {
        return 0;
    }
    const float ratio = static_cast<float>(display_width) / static_cast<float>(render_width);
    return static_cast<std::uint32_t>(8.0f * ratio * ratio);
}

JitterOffset fsr_jitter(std::uint64_t frame_index, std::uint32_t phase_count) noexcept {
    if (phase_count == 0) {
        return JitterOffset{};
    }
    const auto index = static_cast<std::uint32_t>(frame_index % phase_count) + 1;
    return JitterOffset{halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

bool benchmark_policy_allowed(const UpscalePolicy& policy) noexcept {
    return !policy.frame_generation && !policy.dynamic_resolution;
}

HistoryReset UpscaleHistory::begin_frame(const TaaFrameInput& input) {
    const HistoryReset reason = taa_history_reset(has_history_, width_, height_, input);
    if (reason != HistoryReset::None) {
        has_history_ = false;
    }
    width_ = input.width;
    height_ = input.height;
    return reason;
}

}  // namespace engine::render
