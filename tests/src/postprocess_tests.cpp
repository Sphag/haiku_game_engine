// HDR postprocessing (US-039), on the CPU. These cases need no GPU: they check the jitter sequence and its NDC form on both backends,
// exposure from EV100 and its clamps, the color hygiene that stops NaN and Inf from spreading, the bloom prefilter and blur weights, the
// tone curve, the overlay composite (independent of scene exposure), and the TAA history: its reset reasons, convergence on static
// content, ghost rejection on a moving pixel, and finite output for bad input.

#include "engine/render/postprocess.hpp"
#include "engine/render/shadow_atlas.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

using engine::render::bloom_blur_weights;
using engine::render::bloom_prefilter;
using engine::render::BloomSettings;
using engine::render::composite_overlay;
using engine::render::exposure_from_ev100;
using engine::render::ev100_from_luminance;
using engine::render::ExposureSettings;
using engine::render::HistoryReset;
using engine::render::is_teleport;
using engine::render::JitterOffset;
using engine::render::kJitterPhases;
using engine::render::kTeleportDistance;
using engine::render::postprocess_pixel;
using engine::render::sanitize_color;
using engine::render::TaaFrameInput;
using engine::render::TaaHistory;
using engine::render::TaaSettings;
using engine::render::taa_jitter;
using engine::render::srgb_encode;
using engine::render::taa_history_reset;
using engine::render::taa_jitter_ndc;
using engine::render::tile_row_sign;
using engine::render::tonemap_aces;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

bool finite(glm::vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The row a backend draws an NDC y into, with the same formula the shader uses (docs/postprocess.md).
float row_of(float ndc_y, std::uint32_t height, bool vulkan_clip) {
    return (0.5f + 0.5f * tile_row_sign(vulkan_clip) * ndc_y) * static_cast<float>(height);
}

}  // namespace

TEST_CASE("The jitter sequence is centred on the pixel, repeats, and visits distinct phases", "[unit]") {
    std::set<std::pair<float, float>> phases;
    for (std::uint64_t frame = 0; frame < kJitterPhases; ++frame) {
        const JitterOffset j = taa_jitter(frame);
        CHECK(j.x >= -0.5f);
        CHECK(j.x <= 0.5f);
        CHECK(j.y >= -0.5f);
        CHECK(j.y <= 0.5f);
        phases.insert({j.x, j.y});
        const JitterOffset again = taa_jitter(frame + kJitterPhases);
        CHECK(again.x == j.x);
        CHECK(again.y == j.y);
    }
    CHECK(phases.size() == kJitterPhases);

    // Frame 0 is Halton index 1: x = 1/2 - 1/2 = 0, y = 1/3 - 1/2.
    const JitterOffset first = taa_jitter(0);
    CHECK(first.x == Catch::Approx(0.0f));
    CHECK(first.y == Catch::Approx(1.0f / 3.0f - 0.5f));
}

TEST_CASE("The jitter's NDC form moves the image by the same pixel amount on DX12 and Vulkan", "[unit]") {
    const std::uint32_t width = 1920;
    const std::uint32_t height = 1080;
    for (std::uint64_t frame = 0; frame < kJitterPhases; ++frame) {
        const JitterOffset j = taa_jitter(frame);
        const glm::vec2 dx = taa_jitter_ndc(j, width, height, false);
        const glm::vec2 vk = taa_jitter_ndc(j, width, height, true);
        // x is the same on both backends; y has the opposite NDC sign, because the Vulkan NDC y runs the other way.
        CHECK(dx.x == Catch::Approx(vk.x));
        CHECK(dx.y == Catch::Approx(-vk.y));
        // Both shift the same row by the same jitter: a row at y = 0 moves by j.y pixels on either backend.
        const float base_dx = row_of(0.0f, height, false);
        const float base_vk = row_of(0.0f, height, true);
        CHECK(row_of(dx.y, height, false) - base_dx == Catch::Approx(j.y).margin(1e-4f));
        CHECK(row_of(vk.y, height, true) - base_vk == Catch::Approx(j.y).margin(1e-4f));
        CHECK(dx.x * static_cast<float>(width) * 0.5f == Catch::Approx(j.x).margin(1e-4f));
    }
    CHECK(taa_jitter_ndc(JitterOffset{0.25f, 0.25f}, 0, 0, false) == glm::vec2(0.0f));
}

TEST_CASE("Exposure follows EV100, rises as the scene darkens, and clamps unusable input", "[unit]") {
    CHECK(exposure_from_ev100(0.0f) == Catch::Approx(1.0f / 1.2f));
    CHECK(exposure_from_ev100(1.0f) == Catch::Approx(0.5f / 1.2f));
    CHECK(std::isfinite(exposure_from_ev100(kNaN)));
    CHECK(exposure_from_ev100(kNaN) == 1.0f);

    const ExposureSettings settings{};
    // Average luminance 1/8 is EV 0; doubling the luminance adds one stop.
    CHECK(ev100_from_luminance(1.0f / 8.0f, settings) == Catch::Approx(0.0f));
    CHECK(ev100_from_luminance(2.0f / 8.0f, settings) == Catch::Approx(1.0f));
    // A darker scene gets more exposure (a larger exposure multiplier).
    CHECK(exposure_from_ev100(ev100_from_luminance(0.01f, settings)) > exposure_from_ev100(ev100_from_luminance(10.0f, settings)));

    // The range clamps the chosen EV, and the compensation is added after the clamp.
    CHECK(ev100_from_luminance(1.0e12f, settings) == Catch::Approx(settings.max_ev100));
    ExposureSettings compensated = settings;
    compensated.ev_compensation = 0.5f;
    CHECK(ev100_from_luminance(1.0e12f, compensated) == Catch::Approx(settings.max_ev100 + 0.5f));

    // Empty, negative, and NaN frames choose the darkest exposure instead of an infinite one.
    CHECK(ev100_from_luminance(0.0f, settings) == Catch::Approx(settings.min_ev100));
    CHECK(ev100_from_luminance(-3.0f, settings) == Catch::Approx(settings.min_ev100));
    CHECK(ev100_from_luminance(kNaN, settings) == Catch::Approx(settings.min_ev100));
    CHECK(std::isfinite(exposure_from_ev100(ev100_from_luminance(kInf, settings))));
}

TEST_CASE("Color hygiene replaces NaN and Inf and clamps negatives, so bad samples cannot spread", "[unit]") {
    CHECK(sanitize_color(glm::vec3(kNaN, kInf, -kInf)) == glm::vec3(0.0f));
    CHECK(sanitize_color(glm::vec3(-2.0f, 0.5f, 3.0f)) == glm::vec3(0.0f, 0.5f, 3.0f));
    CHECK(finite(tonemap_aces(glm::vec3(kNaN, kInf, -4.0f))));
    CHECK(finite(postprocess_pixel(glm::vec3(kNaN), glm::vec3(kInf), kNaN, kNaN)));
}

TEST_CASE("The bloom prefilter removes what is below the knee and passes the excess above it", "[unit]") {
    const BloomSettings settings{1.0f, 0.5f, 0.1f};
    // Well below threshold - knee: nothing.
    CHECK(glm::length(bloom_prefilter(glm::vec3(0.2f), settings)) == Catch::Approx(0.0f));
    // Above threshold + knee: only the excess over the threshold remains in the brightest channel.
    const glm::vec3 bright = bloom_prefilter(glm::vec3(3.0f, 1.0f, 0.0f), settings);
    CHECK(std::max(bright.x, std::max(bright.y, bright.z)) == Catch::Approx(3.0f - 1.0f));
    // Inside the knee the color is partly passed: it is positive and never brighter than the input.
    const glm::vec3 knee = bloom_prefilter(glm::vec3(1.2f), settings);
    CHECK(knee.x > 0.0f);
    CHECK(knee.x < 1.2f);
    // No threshold and no knee: the color passes unchanged.
    const BloomSettings open{0.0f, 0.0f, 1.0f};
    CHECK(bloom_prefilter(glm::vec3(2.0f, 0.5f, 1.0f), open) == glm::vec3(2.0f, 0.5f, 1.0f));
    CHECK(finite(bloom_prefilter(glm::vec3(kNaN), settings)));
}

TEST_CASE("Bloom blur weights are normalized, symmetric, and peak at the centre", "[unit]") {
    CHECK(bloom_blur_weights(0) == std::vector<float>{1.0f});
    for (const std::uint32_t radius : {1u, 4u, 9u}) {
        const std::vector<float> w = bloom_blur_weights(radius);
        REQUIRE(w.size() == 2 * radius + 1);
        float sum = 0.0f;
        for (const float v : w) {
            sum += v;
        }
        CHECK(sum == Catch::Approx(1.0f));
        for (std::uint32_t i = 0; i < radius; ++i) {
            CHECK(w[i] == Catch::Approx(w[w.size() - 1 - i]));
            CHECK(w[radius] > w[i]);
        }
    }
}

TEST_CASE("The ACES curve is monotonic, stays in [0, 1], and maps black to black", "[unit]") {
    CHECK(tonemap_aces(glm::vec3(0.0f)) == glm::vec3(0.0f));
    float previous = -1.0f;
    for (float x = 0.0f; x <= 16.0f; x += 0.25f) {
        const float y = tonemap_aces(glm::vec3(x)).x;
        CHECK(y >= 0.0f);
        CHECK(y <= 1.0f);
        CHECK(y >= previous);
        previous = y;
    }
    CHECK(tonemap_aces(glm::vec3(1.0e6f)).x == Catch::Approx(1.0f));
}

TEST_CASE("Exposure scales the scene, bloom adds in linear light, and a zero exposure is black", "[unit]") {
    const glm::vec3 scene(0.4f, 0.2f, 0.1f);
    const glm::vec3 dim = postprocess_pixel(scene, glm::vec3(0.0f), 0.5f, 0.0f);
    const glm::vec3 bright = postprocess_pixel(scene, glm::vec3(0.0f), 2.0f, 0.0f);
    CHECK(bright.x > dim.x);
    CHECK(postprocess_pixel(scene, glm::vec3(0.0f), 0.0f, 0.0f) == glm::vec3(0.0f));
    // Bloom raises a dark pixel that the scene alone would leave dark.
    CHECK(postprocess_pixel(glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 0.5f).x > 0.0f);
    CHECK(postprocess_pixel(glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 0.0f).x == 0.0f);
    // A NaN exposure falls back to 1 rather than producing a NaN pixel.
    CHECK(postprocess_pixel(scene, glm::vec3(0.0f), kNaN, 0.0f) == postprocess_pixel(scene, glm::vec3(0.0f), 1.0f, 0.0f));
}

TEST_CASE("A reference overlay is composited after tone mapping and ignores the scene exposure", "[unit]") {
    const glm::vec3 scene(0.6f, 0.6f, 0.6f);
    const glm::vec4 overlay(1.0f, 0.0f, 0.0f, 1.0f);
    // A fully opaque overlay shows its own color whatever the scene exposure was.
    const glm::vec3 under_low = composite_overlay(postprocess_pixel(scene, glm::vec3(0.0f), 0.1f, 0.0f), overlay);
    const glm::vec3 under_high = composite_overlay(postprocess_pixel(scene, glm::vec3(0.0f), 8.0f, 0.0f), overlay);
    CHECK(under_low == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(under_high == glm::vec3(1.0f, 0.0f, 0.0f));
    // A transparent overlay leaves the tone-mapped scene untouched.
    const glm::vec3 display = postprocess_pixel(scene, glm::vec3(0.0f), 1.0f, 0.0f);
    CHECK(composite_overlay(display, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)) == display);
    CHECK(composite_overlay(display, glm::vec4(kNaN, 0.0f, 0.0f, kNaN)) == display);
}

TEST_CASE("The TAA history resets on the first frame, a size change, a cut, and a teleport, and on no other frame", "[unit]") {
    TaaHistory history;
    CHECK(history.begin_frame({8, 4, false, false}) == HistoryReset::FirstFrame);
    const std::vector<glm::vec3> color(32, glm::vec3(0.5f));
    const std::vector<glm::vec2> still(32, glm::vec2(0.0f));
    (void)history.resolve(color, still, TaaSettings{});
    CHECK(history.has_history());

    CHECK(history.begin_frame({8, 4, false, false}) == HistoryReset::None);
    (void)history.resolve(color, still, TaaSettings{});
    CHECK(history.begin_frame({8, 4, false, true}) == HistoryReset::Teleport);
    (void)history.resolve(color, still, TaaSettings{});
    CHECK(history.begin_frame({8, 4, true, false}) == HistoryReset::CameraCut);
    (void)history.resolve(color, still, TaaSettings{});
    CHECK(history.begin_frame({16, 4, false, false}) == HistoryReset::Resize);
}

TEST_CASE("A reset frame resolves to its current color, with no blend against stale history", "[unit]") {
    TaaHistory history;
    const std::vector<glm::vec2> still(8, glm::vec2(0.0f));
    (void)history.begin_frame({8, 1, false, false});
    (void)history.resolve(std::vector<glm::vec3>(8, glm::vec3(1.0f)), still, TaaSettings{});

    // The camera cuts to a dark scene: the first frame of the new shot is exactly the dark color.
    CHECK(history.begin_frame({8, 1, true, false}) == HistoryReset::CameraCut);
    const std::vector<glm::vec3> dark = history.resolve(std::vector<glm::vec3>(8, glm::vec3(0.1f)), still, TaaSettings{});
    CHECK(dark[3] == glm::vec3(0.1f));
}

TEST_CASE("TAA converges on static content and stays finite when the input is not", "[unit]") {
    TaaHistory history;
    const std::vector<glm::vec2> still(16, glm::vec2(0.0f));
    TaaSettings settings;
    settings.current_weight = 0.25f;
    (void)history.begin_frame({4, 4, false, false});
    (void)history.resolve(std::vector<glm::vec3>(16, glm::vec3(0.0f)), still, settings);
    std::vector<glm::vec3> out;
    for (int frame = 0; frame < 40; ++frame) {
        (void)history.begin_frame({4, 4, false, false});
        out = history.resolve(std::vector<glm::vec3>(16, glm::vec3(0.8f)), still, settings);
    }
    CHECK(out[5].x == Catch::Approx(0.8f).margin(1e-3f));

    // NaN and Inf samples come out as finite colors, and a NaN motion vector falls back to the current sample.
    std::vector<glm::vec3> bad(16, glm::vec3(0.8f));
    bad[0] = glm::vec3(kNaN, 1.0f, kInf);
    std::vector<glm::vec2> motion(16, glm::vec2(0.0f));
    motion[7] = glm::vec2(kNaN, 0.0f);
    (void)history.begin_frame({4, 4, false, false});
    const std::vector<glm::vec3> resolved = history.resolve(bad, motion, settings);
    for (const glm::vec3& c : resolved) {
        CHECK(finite(c));
    }
    CHECK(resolved[7] == glm::vec3(0.8f));
}

TEST_CASE("TAA history follows a moving pixel and rejects it when the motion is wrong", "[unit]") {
    // One row of eight pixels. A bright object is at x = 2 in frame 0 and x = 3 in frame 1, moving one pixel to the right per frame.
    const std::uint32_t width = 8;
    const std::uint32_t height = 1;
    const std::vector<glm::vec3> frame0 = [] {
        std::vector<glm::vec3> f(8, glm::vec3(0.0f));
        f[2] = glm::vec3(1.0f);
        return f;
    }();
    const std::vector<glm::vec3> frame1 = [] {
        std::vector<glm::vec3> f(8, glm::vec3(0.0f));
        f[3] = glm::vec3(1.0f);
        return f;
    }();
    // The UV motion of the object is its current minus its previous position: +1 pixel in x.
    const std::vector<glm::vec2> moving(8, glm::vec2(1.0f / static_cast<float>(width), 0.0f));
    const std::vector<glm::vec2> still(8, glm::vec2(0.0f));
    TaaSettings settings;
    settings.current_weight = 0.1f;

    TaaHistory with_motion;
    (void)with_motion.begin_frame({width, height, false, false});
    (void)with_motion.resolve(frame0, still, settings);
    (void)with_motion.begin_frame({width, height, false, false});
    const std::vector<glm::vec3> followed = with_motion.resolve(frame1, moving, settings);
    // The object's history came from x = 2 and is kept at x = 3, so the pixel stays close to 1.
    CHECK(followed[3].x > 0.9f);

    TaaHistory without_motion;
    (void)without_motion.begin_frame({width, height, false, false});
    (void)without_motion.resolve(frame0, still, settings);
    (void)without_motion.begin_frame({width, height, false, false});
    const std::vector<glm::vec3> ghosted = without_motion.resolve(frame1, still, settings);
    // With no motion the history at x = 3 is the old background, so the object is faded in only by the current weight.
    CHECK(ghosted[3].x == Catch::Approx(0.1f).margin(1e-4f));
    CHECK(followed[3].x > ghosted[3].x);

    // A motion that points off the target keeps the current sample alone.
    TaaHistory edge;
    (void)edge.begin_frame({width, height, false, false});
    (void)edge.resolve(frame0, still, settings);
    (void)edge.begin_frame({width, height, false, false});
    const std::vector<glm::vec2> off(8, glm::vec2(10.0f, 0.0f));
    const std::vector<glm::vec3> out = edge.resolve(frame1, off, settings);
    CHECK(out[3] == frame1[3]);
}

TEST_CASE("A mismatched frame size keeps no history and returns the sanitized current color", "[unit]") {
    TaaHistory history;
    (void)history.begin_frame({4, 2, false, false});
    const std::vector<glm::vec3> out = history.resolve(std::vector<glm::vec3>(3, glm::vec3(kNaN)), std::vector<glm::vec2>(8), TaaSettings{});
    CHECK(out.size() == 3);
    CHECK(finite(out[0]));
    CHECK_FALSE(history.has_history());
}

TEST_CASE("A camera move beyond the teleport distance is a teleport, and a NaN position is too", "[unit]") {
    CHECK_FALSE(is_teleport(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK_FALSE(is_teleport(glm::vec3(0.0f), glm::vec3(kTeleportDistance - 1.0f, 0.0f, 0.0f)));
    CHECK(is_teleport(glm::vec3(0.0f), glm::vec3(kTeleportDistance + 1.0f, 0.0f, 0.0f)));
    CHECK(is_teleport(glm::vec3(0.0f), glm::vec3(kNaN, 0.0f, 0.0f)));
}

TEST_CASE("The sRGB encode matches the scene shader's curve at the ends and the linear toe, and clamps input", "[unit]") {
    CHECK(srgb_encode(glm::vec3(0.0f)) == glm::vec3(0.0f));
    CHECK(srgb_encode(glm::vec3(1.0f)).x == Catch::Approx(1.0f).margin(1e-6f));
    // The linear toe below 0.0031308, and the power curve above it, at a mid-grey point.
    CHECK(srgb_encode(glm::vec3(0.001f)).x == Catch::Approx(12.92f * 0.001f).margin(1e-7f));
    CHECK(srgb_encode(glm::vec3(0.5f)).x == Catch::Approx(0.735357f).margin(1e-4f));
    // Out of range input is clamped, and NaN is treated as zero.
    CHECK(srgb_encode(glm::vec3(2.0f)).x == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(srgb_encode(glm::vec3(-1.0f, std::nanf(""), 0.0f)) == glm::vec3(0.0f));
}

TEST_CASE("The GPU history reset rule gives the same reason as the CPU history, in the same order", "[unit]") {
    TaaFrameInput frame;
    frame.width = 8;
    frame.height = 4;
    CHECK(taa_history_reset(false, 0, 0, frame) == HistoryReset::FirstFrame);
    CHECK(taa_history_reset(true, 8, 4, frame) == HistoryReset::None);
    CHECK(taa_history_reset(true, 16, 4, frame) == HistoryReset::Resize);
    frame.camera_cut = true;
    frame.teleport = true;
    CHECK(taa_history_reset(true, 8, 4, frame) == HistoryReset::CameraCut);
    frame.camera_cut = false;
    CHECK(taa_history_reset(true, 8, 4, frame) == HistoryReset::Teleport);
    // A missing history wins over every other reason.
    CHECK(taa_history_reset(false, 16, 4, frame) == HistoryReset::FirstFrame);
}

