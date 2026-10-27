#include "engine/render/postprocess.hpp"

#include "engine/render/shadow_atlas.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {

namespace {

// Radical inverse of `index` in `base`, in [0, 1). The Halton sequence is these inverses for bases 2 and 3.
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

float max_component(glm::vec3 v) noexcept {
    return std::max(v.x, std::max(v.y, v.z));
}

// The ACES fit for one channel. Input is non-negative and finite.
float aces_channel(float x) noexcept {
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

}  // namespace

JitterOffset taa_jitter(std::uint64_t frame_index) noexcept {
    // Index 0 is skipped: Halton(0) is 0, which would put every phase's first sample on the pixel corner.
    const auto index = static_cast<std::uint32_t>(frame_index % kJitterPhases) + 1;
    return JitterOffset{halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

glm::vec2 taa_jitter_ndc(JitterOffset jitter, std::uint32_t width, std::uint32_t height, bool vulkan_clip) noexcept {
    if (width == 0 || height == 0) {
        return glm::vec2(0.0f);
    }
    // A shift of `jitter.y` rows moves NDC y by 2 * jitter.y / height times the row sign (tile_row_sign maps NDC y to a row).
    const float row_sign = tile_row_sign(vulkan_clip);
    return glm::vec2(2.0f * jitter.x / static_cast<float>(width), 2.0f * jitter.y * row_sign / static_cast<float>(height));
}

float exposure_from_ev100(float ev100) noexcept {
    if (!std::isfinite(ev100)) {
        return 1.0f;
    }
    return 1.0f / (1.2f * std::exp2(ev100));
}

float ev100_from_luminance(float average_luminance, const ExposureSettings& settings) noexcept {
    const float low = settings.min_ev100;
    const float high = settings.max_ev100;
    // EV100 = log2(L * S / K) with S = 100 (ISO) and K = 12.5, so log2(8 L).
    if (!std::isfinite(average_luminance) || average_luminance <= 0.0f) {
        return low + settings.ev_compensation;
    }
    const float ev = std::log2(8.0f * average_luminance);
    return std::clamp(ev, low, high) + settings.ev_compensation;
}

glm::vec3 sanitize_color(glm::vec3 color) noexcept {
    const auto clean = [](float c) { return std::isfinite(c) ? std::max(c, 0.0f) : 0.0f; };
    return glm::vec3(clean(color.x), clean(color.y), clean(color.z));
}

glm::vec3 bloom_prefilter(glm::vec3 color, const BloomSettings& settings) noexcept {
    const glm::vec3 clean = sanitize_color(color);
    const float brightness = max_component(clean);
    const float threshold = std::max(settings.threshold, 0.0f);
    const float knee = std::max(settings.knee, 0.0f);
    // The soft part is a quadratic ramp across [threshold - knee, threshold + knee]; above that the full excess passes.
    float soft = std::clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-4f);
    const float contribution = std::max(soft, brightness - threshold) / std::max(brightness, 1e-4f);
    return clean * std::max(contribution, 0.0f);
}

std::vector<float> bloom_blur_weights(std::uint32_t radius) {
    if (radius == 0) {
        return {1.0f};
    }
    const float sigma = static_cast<float>(radius) / 2.0f;
    std::vector<float> weights(2 * static_cast<std::size_t>(radius) + 1);
    float sum = 0.0f;
    for (std::uint32_t i = 0; i <= radius; ++i) {
        const float x = static_cast<float>(i);
        const float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        weights[radius + i] = w;
        weights[radius - i] = w;
    }
    for (const float w : weights) {
        sum += w;
    }
    for (float& w : weights) {
        w /= sum;
    }
    return weights;
}

glm::vec3 tonemap_aces(glm::vec3 exposed_color) noexcept {
    const glm::vec3 clean = sanitize_color(exposed_color);
    return glm::vec3(aces_channel(clean.x), aces_channel(clean.y), aces_channel(clean.z));
}

glm::vec3 srgb_encode(glm::vec3 linear) noexcept {
    const auto encode = [](float c) {
        const float x = std::clamp(c, 0.0f, 1.0f);
        return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
    };
    const glm::vec3 clean = sanitize_color(linear);
    return glm::vec3(encode(clean.x), encode(clean.y), encode(clean.z));
}

glm::vec3 postprocess_pixel(glm::vec3 scene_color, glm::vec3 bloom, float exposure, float bloom_intensity) noexcept {
    const float safe_exposure = std::isfinite(exposure) ? std::max(exposure, 0.0f) : 1.0f;
    const float safe_intensity = std::isfinite(bloom_intensity) ? std::max(bloom_intensity, 0.0f) : 0.0f;
    // Bloom is added in linear light, before exposure, so it scales with the scene as the scene does.
    const glm::vec3 combined = sanitize_color(scene_color) + sanitize_color(bloom) * safe_intensity;
    return tonemap_aces(combined * safe_exposure);
}

glm::vec3 composite_overlay(glm::vec3 display_color, glm::vec4 overlay) noexcept {
    const float alpha = std::isfinite(overlay.a) ? std::clamp(overlay.a, 0.0f, 1.0f) : 0.0f;
    return sanitize_color(display_color) * (1.0f - alpha) + sanitize_color(glm::vec3(overlay)) * alpha;
}

bool is_teleport(glm::vec3 previous_camera, glm::vec3 current_camera) noexcept {
    const float distance = glm::length(current_camera - previous_camera);
    return !std::isfinite(distance) || distance > kTeleportDistance;
}

HistoryReset taa_history_reset(bool has_history, std::uint32_t previous_width, std::uint32_t previous_height,
                               const TaaFrameInput& input) noexcept {
    if (!has_history) {
        return HistoryReset::FirstFrame;
    }
    if (input.width != previous_width || input.height != previous_height) {
        return HistoryReset::Resize;
    }
    if (input.camera_cut) {
        return HistoryReset::CameraCut;
    }
    if (input.teleport) {
        return HistoryReset::Teleport;
    }
    return HistoryReset::None;
}

HistoryReset TaaHistory::begin_frame(const TaaFrameInput& input) {
    const HistoryReset reason = taa_history_reset(has_history_, width_, height_, input);
    if (reason != HistoryReset::None) {
        // The next resolve starts a fresh history from its current color.
        has_history_ = false;
        history_.clear();
    }
    width_ = input.width;
    height_ = input.height;
    pending_reset_ = reason;
    return reason;
}

std::vector<glm::vec3> TaaHistory::resolve(const std::vector<glm::vec3>& current, const std::vector<glm::vec2>& velocity,
                                           const TaaSettings& settings) {
    const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    std::vector<glm::vec3> clean(current.size());
    std::transform(current.begin(), current.end(), clean.begin(), [](glm::vec3 c) { return sanitize_color(c); });
    if (count == 0 || current.size() != count || velocity.size() != count) {
        // Nothing to accumulate: the frame keeps no history.
        has_history_ = false;
        history_.clear();
        pending_reset_ = HistoryReset::None;
        return clean;
    }
    if (!has_history_ || pending_reset_ != HistoryReset::None) {
        history_ = clean;
        has_history_ = true;
        pending_reset_ = HistoryReset::None;
        return clean;
    }

    const float weight = std::isfinite(settings.current_weight) ? std::clamp(settings.current_weight, 0.0f, 1.0f) : 1.0f;
    const auto w = static_cast<int>(width_);
    const auto h = static_cast<int>(height_);
    std::vector<glm::vec3> out(count);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x);
            const glm::vec3 now = clean[i];

            // Color box of the current 3x3 neighborhood. The history is clamped into it, so content that is no longer there fades out.
            glm::vec3 box_min = now;
            glm::vec3 box_max = now;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = std::clamp(x + dx, 0, w - 1);
                    const int ny = std::clamp(y + dy, 0, h - 1);
                    const glm::vec3 c = clean[static_cast<std::size_t>(ny) * width_ + static_cast<std::size_t>(nx)];
                    box_min = glm::min(box_min, c);
                    box_max = glm::max(box_max, c);
                }
            }

            // The previous position of this pixel is the current one minus its UV motion, in pixels.
            const glm::vec2 motion = velocity[i] * glm::vec2(static_cast<float>(w), static_cast<float>(h));
            const bool finite = std::isfinite(motion.x) && std::isfinite(motion.y);
            const glm::vec2 previous = glm::vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) - motion;
            const int px = finite ? static_cast<int>(std::floor(previous.x)) : -1;
            const int py = finite ? static_cast<int>(std::floor(previous.y)) : -1;
            if (px < 0 || py < 0 || px >= w || py >= h) {
                out[i] = now;  // off screen: no history for this pixel
                continue;
            }
            const glm::vec3 past = history_[static_cast<std::size_t>(py) * width_ + static_cast<std::size_t>(px)];
            const glm::vec3 clamped = glm::clamp(past, box_min, box_max);
            out[i] = glm::mix(clamped, now, weight);
        }
    }
    history_ = out;
    return out;
}

}  // namespace engine::render
