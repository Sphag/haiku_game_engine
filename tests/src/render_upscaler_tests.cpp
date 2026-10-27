// The temporal upscaling contract (US-046): the two image profiles' fixed resolutions, FSR 2's render size and jitter phase count, the jitter
// values, the benchmark policy, and the upscaler's history reset. The GPU upscale is checked by the FSR 2 integration's GPU tests once
// they exist (docs/upscaling.md); this file covers the CPU contract only.
#include "engine/render/upscaler.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <utility>

using engine::render::ImageProfile;
using engine::render::UpscaleHistory;
using engine::render::UpscalePolicy;
using engine::render::benchmark_policy_allowed;
using engine::render::fsr_jitter;
using engine::render::fsr_jitter_phase_count;
using engine::render::fsr_render_extent;
using engine::render::HistoryReset;
using engine::render::kFsrQualityRatio;
using engine::render::profile_resolution;
using engine::render::TaaFrameInput;

TEST_CASE("The native profile renders at the output size without upscaling", "[unit]") {
    const auto native = profile_resolution(ImageProfile::Native1080);
    CHECK(native.output_width == 1920);
    CHECK(native.output_height == 1080);
    CHECK(native.internal_width == 1920);
    CHECK(native.internal_height == 1080);
    CHECK_FALSE(native.upscale);
}

TEST_CASE("The full profile renders at a fixed 1280x720 and upscales to the output size with FSR 2 Quality", "[unit]") {
    CHECK(kFsrQualityRatio == 1.5f);
    const auto full = profile_resolution(ImageProfile::Full1080);
    CHECK(full.output_width == 1920);
    CHECK(full.output_height == 1080);
    CHECK(full.internal_width == 1280);
    CHECK(full.internal_height == 720);
    CHECK(full.upscale);
}

TEST_CASE("FSR 2 derives the render size by truncating the display size divided by the ratio", "[unit]") {
    CHECK(fsr_render_extent(1920, 1.5f) == 1280);
    CHECK(fsr_render_extent(1080, 1.5f) == 720);
    // 1000 / 1.5 = 666.67, which truncates to 666 as FSR 2 does.
    CHECK(fsr_render_extent(1000, 1.5f) == 666);
    CHECK(fsr_render_extent(1920, 0.0f) == 0);
}

TEST_CASE("The jitter phase count follows FSR 2's 8 * (display / render)^2 rule", "[unit]") {
    CHECK(fsr_jitter_phase_count(1280, 1920) == 18);
    CHECK(fsr_jitter_phase_count(1920, 1920) == 8);
    CHECK(fsr_jitter_phase_count(0, 1920) == 0);
}

TEST_CASE("The FSR 2 jitter is the Halton sample of the frame's phase, centred on the pixel", "[unit]") {
    // Phase 0 is Halton index 1: x = 1/2 - 1/2 = 0 and y = 1/3 - 1/2 = -1/6.
    const auto first = fsr_jitter(0, 18);
    CHECK(first.x == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(first.y == Catch::Approx(-1.0f / 6.0f).margin(1e-6f));

    // Phase 1 is Halton index 2: x = 1/4 - 1/2 = -1/4 and y = 2/3 - 1/2 = 1/6.
    const auto second = fsr_jitter(1, 18);
    CHECK(second.x == Catch::Approx(-0.25f).margin(1e-6f));
    CHECK(second.y == Catch::Approx(1.0f / 6.0f).margin(1e-6f));
}

TEST_CASE("The FSR 2 jitter repeats every phase count and stays in the half-pixel range", "[unit]") {
    const std::uint32_t phases = 18;
    std::set<std::pair<float, float>> distinct;
    for (std::uint64_t frame = 0; frame < 2 * phases; ++frame) {
        const auto jitter = fsr_jitter(frame, phases);
        CHECK(jitter.x >= -0.5f);
        CHECK(jitter.x <= 0.5f);
        CHECK(jitter.y >= -0.5f);
        CHECK(jitter.y <= 0.5f);
        if (frame < phases) {
            distinct.insert({jitter.x, jitter.y});
        }
        const auto repeated = fsr_jitter(frame + phases, phases);
        CHECK(repeated.x == jitter.x);
        CHECK(repeated.y == jitter.y);
    }
    CHECK(distinct.size() == phases);
}

TEST_CASE("A zero phase count gives no jitter instead of dividing by zero", "[unit]") {
    const auto none = fsr_jitter(7, 0);
    CHECK(none.x == 0.0f);
    CHECK(none.y == 0.0f);
}

TEST_CASE("The benchmark policy forbids frame generation and dynamic resolution", "[unit]") {
    CHECK(benchmark_policy_allowed(UpscalePolicy{}));
    CHECK_FALSE(benchmark_policy_allowed(UpscalePolicy{.frame_generation = true}));
    CHECK_FALSE(benchmark_policy_allowed(UpscalePolicy{.dynamic_resolution = true}));
    CHECK_FALSE(benchmark_policy_allowed(UpscalePolicy{.frame_generation = true, .dynamic_resolution = true}));
}

TEST_CASE("The upscaler keeps its history across frames and resets it on a size change, cut, or teleport", "[unit]") {
    UpscaleHistory history;
    const TaaFrameInput steady{1280, 720, false, false};

    // A frame that was never completed does not count as history.
    CHECK(history.begin_frame(steady) == HistoryReset::FirstFrame);
    CHECK(history.begin_frame(steady) == HistoryReset::FirstFrame);
    history.frame_done();
    CHECK(history.has_history());

    CHECK(history.begin_frame(steady) == HistoryReset::None);
    history.frame_done();

    CHECK(history.begin_frame(TaaFrameInput{1920, 1080, false, false}) == HistoryReset::Resize);
    history.frame_done();

    CHECK(history.begin_frame(TaaFrameInput{1920, 1080, true, false}) == HistoryReset::CameraCut);
    history.frame_done();

    CHECK(history.begin_frame(TaaFrameInput{1920, 1080, false, true}) == HistoryReset::Teleport);
    history.frame_done();
    CHECK(history.begin_frame(TaaFrameInput{1920, 1080, false, false}) == HistoryReset::None);
}
