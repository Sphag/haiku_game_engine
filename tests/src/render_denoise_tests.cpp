// The denoiser's CPU reference and its contracts (US-045): the settings' ranges, the camera-cut and light-change tests, the spatial prefilter,
// the reprojection with its disocclusion rejection, the reset, the age floor that stops stale history from persisting, and the handling of
// non-finite samples. The GPU pass is checked against this reference in render_denoise_gpu_tests.cpp.
#include "engine/render/denoiser.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using engine::render::CameraPose;
using engine::render::DenoiseHistory;
using engine::render::DenoiseReference;
using engine::render::DenoiseSettings;
using engine::render::DenoiseTarget;
using engine::render::camera_cut;
using engine::render::denoise_reference;
using engine::render::kDenoiseParameterBytes;
using engine::render::light_changed;
using engine::render::pack_denoise_parameters;
using engine::render::validate_denoise_settings;

namespace {

// A flat wall of `width` x `height` pixels, all at `depth` and facing the camera. Its values are `value` everywhere.
DenoiseTarget flat_target(std::uint32_t width, std::uint32_t height, float depth, glm::vec4 value) {
    DenoiseTarget target;
    target.width = width;
    target.height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    target.value.assign(count, value);
    target.depth.assign(count, depth);
    target.normal.assign(count, glm::vec3(0.0f, 0.0f, 1.0f));
    target.motion.assign(count, glm::vec2(0.0f));
    return target;
}

}  // namespace

TEST_CASE("The denoiser's settings reject values outside their ranges", "[unit]") {
    DenoiseSettings settings;
    CHECK_FALSE(validate_denoise_settings(settings).has_value());

    DenoiseSettings weight = settings;
    weight.history_weight = 1.0f;
    CHECK(validate_denoise_settings(weight).has_value());

    DenoiseSettings age = settings;
    age.max_age = 0;
    CHECK(validate_denoise_settings(age).has_value());

    DenoiseSettings tolerance = settings;
    tolerance.depth_tolerance = -0.1f;
    CHECK(validate_denoise_settings(tolerance).has_value());

    DenoiseSettings normal = settings;
    normal.normal_threshold = 1.5f;
    CHECK(validate_denoise_settings(normal).has_value());

    DenoiseSettings radius = settings;
    radius.spatial_radius = 3;
    CHECK(validate_denoise_settings(radius).has_value());
}

TEST_CASE("A camera cut is a jump in position or a turn the history cannot follow", "[unit]") {
    const CameraPose start{glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    CHECK_FALSE(camera_cut(start, CameraPose{glm::vec3(0.05f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)}, 1.0f, 5.0f));
    CHECK(camera_cut(start, CameraPose{glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)}, 1.0f, 5.0f));
    // A 30 degree turn about the vertical axis.
    const glm::vec3 turned(std::sin(0.5235988f), 0.0f, -std::cos(0.5235988f));
    CHECK(camera_cut(start, CameraPose{glm::vec3(0.0f), turned}, 1.0f, 5.0f));
    CHECK_FALSE(camera_cut(start, CameraPose{glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f)}, 1.0f, 5.0f));
}

TEST_CASE("A light that turns past the threshold resets the light-dependent history", "[unit]") {
    const glm::vec3 sun(0.0f, 1.0f, 0.0f);
    CHECK_FALSE(light_changed(sun, glm::normalize(glm::vec3(0.01f, 1.0f, 0.0f)), 1.0f));
    CHECK(light_changed(sun, glm::normalize(glm::vec3(0.1f, 1.0f, 0.0f)), 1.0f));
}

TEST_CASE("The spatial prefilter averages the neighbours that share the pixel's geometry", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 1;
    DenoiseTarget target = flat_target(8, 8, 5.0f, glm::vec4(0.0f));
    // A bright pixel in a flat wall: its filtered value is shared among itself and its 8 neighbours.
    target.value[target.index(4, 4)] = glm::vec4(9.0f);
    const DenoiseReference result = denoise_reference(target, DenoiseHistory{}, settings, false);
    CHECK(result.output[result.index(4, 4)].r == Catch::Approx(1.0f));
    CHECK(result.output[result.index(3, 3)].r == Catch::Approx(1.0f));
    CHECK(result.output[result.index(0, 0)].r == Catch::Approx(0.0f));
}

TEST_CASE("The prefilter does not average across a depth edge", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 1;
    DenoiseTarget target = flat_target(8, 8, 5.0f, glm::vec4(0.0f));
    // The right half of the image is a wall 50 m behind the left half, with a bright value that must not leak across the edge.
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 4; x < 8; ++x) {
            target.depth[target.index(x, y)] = 50.0f;
            target.value[target.index(x, y)] = glm::vec4(4.0f);
        }
    }
    const DenoiseReference result = denoise_reference(target, DenoiseHistory{}, settings, false);
    CHECK(result.output[result.index(3, 3)].r == Catch::Approx(0.0f));
    CHECK(result.output[result.index(4, 3)].r == Catch::Approx(4.0f));
}

TEST_CASE("A history sample is rejected where its depth or normal disagrees, and accepted where they agree", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 0;
    const DenoiseTarget first = flat_target(4, 4, 10.0f, glm::vec4(1.0f));
    const DenoiseReference frame_one = denoise_reference(first, DenoiseHistory{}, settings, false);
    CHECK(frame_one.history.valid);

    // The second frame keeps the wall, except that the left column is now a surface 5 m closer (a disocclusion).
    DenoiseTarget second = flat_target(4, 4, 10.0f, glm::vec4(3.0f));
    for (std::uint32_t y = 0; y < 4; ++y) {
        second.depth[second.index(0, y)] = 5.0f;
    }
    const DenoiseReference frame_two = denoise_reference(second, frame_one.history, settings, false);
    CHECK(frame_two.accepted[frame_two.index(0, 0)] == 0);
    CHECK(frame_two.accepted[frame_two.index(2, 2)] == 1);
    // A rejected pixel restarts from its own value; an accepted one blends toward it.
    CHECK(frame_two.output[frame_two.index(0, 0)].r == Catch::Approx(3.0f));
    CHECK(frame_two.output[frame_two.index(2, 2)].r > 1.0f);
    CHECK(frame_two.output[frame_two.index(2, 2)].r < 3.0f);

    // A normal that turns away rejects the history too.
    DenoiseTarget third = flat_target(4, 4, 10.0f, glm::vec4(3.0f));
    third.normal[third.index(3, 3)] = glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f));
    const DenoiseReference frame_three = denoise_reference(third, frame_two.history, settings, false);
    CHECK(frame_three.accepted[frame_three.index(3, 3)] == 0);
}

TEST_CASE("A reset discards the history for that frame", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 0;
    const DenoiseReference frame_one = denoise_reference(flat_target(4, 4, 10.0f, glm::vec4(1.0f)), DenoiseHistory{}, settings, false);
    const DenoiseReference reset = denoise_reference(flat_target(4, 4, 10.0f, glm::vec4(5.0f)), frame_one.history, settings, true);
    for (std::size_t i = 0; i < reset.accepted.size(); ++i) {
        CHECK(reset.accepted[i] == 0);
    }
    CHECK(reset.output[0].r == Catch::Approx(5.0f));
    CHECK(reset.history.geometry[0].age == Catch::Approx(1.0f));
}

TEST_CASE("A history from another size restarts instead of reading out of bounds", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 0;
    const DenoiseReference small = denoise_reference(flat_target(2, 2, 10.0f, glm::vec4(1.0f)), DenoiseHistory{}, settings, false);
    const DenoiseReference large = denoise_reference(flat_target(4, 4, 10.0f, glm::vec4(2.0f)), small.history, settings, false);
    for (std::size_t i = 0; i < large.accepted.size(); ++i) {
        CHECK(large.accepted[i] == 0);
    }
}

TEST_CASE("Motion reprojects the history, and a motion that leaves the screen rejects it", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 0;
    // Frame one has a bright column at x = 1; frame two moves the wall one pixel to the right, so each pixel reads its left neighbour.
    DenoiseTarget first = flat_target(4, 1, 10.0f, glm::vec4(0.0f));
    first.value[first.index(1, 0)] = glm::vec4(8.0f);
    const DenoiseReference frame_one = denoise_reference(first, DenoiseHistory{}, settings, false);

    DenoiseTarget second = flat_target(4, 1, 10.0f, glm::vec4(0.0f));
    for (std::uint32_t x = 0; x < 4; ++x) {
        second.motion[second.index(x, 0)] = glm::vec2(-1.0f / 4.0f, 0.0f);
    }
    const DenoiseReference frame_two = denoise_reference(second, frame_one.history, settings, false);
    // Pixel 2 reads the history at pixel 1, which is the bright column: accepted, so it carries the bright value.
    CHECK(frame_two.accepted[frame_two.index(2, 0)] == 1);
    CHECK(frame_two.output[frame_two.index(2, 0)].r > 0.0f);
    // Pixel 0 reads x = -1, off the screen: rejected.
    CHECK(frame_two.accepted[frame_two.index(0, 0)] == 0);
}

TEST_CASE("A stale history loses its weight, so a changed signal converges and nothing persists", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 0;
    settings.history_weight = 0.9f;
    settings.max_age = 15;
    DenoiseHistory history;
    for (int frame = 0; frame < 60; ++frame) {
        history = denoise_reference(flat_target(2, 2, 10.0f, glm::vec4(0.0f)), history, settings, false).history;
    }
    // The signal jumps to 1 and stays. The old zero history keeps some weight for a few frames, then the age floor takes over.
    DenoiseReference result;
    for (int frame = 0; frame < 200; ++frame) {
        result = denoise_reference(flat_target(2, 2, 10.0f, glm::vec4(1.0f)), history, settings, false);
        history = result.history;
    }
    CHECK(result.output[0].r == Catch::Approx(1.0f).margin(1.0e-3f));
    CHECK(result.history.geometry[0].age == Catch::Approx(15.0f));
}

TEST_CASE("A non-finite sample contributes nothing, and the output stays finite", "[unit]") {
    DenoiseSettings settings;
    settings.spatial_radius = 1;
    DenoiseTarget target = flat_target(4, 4, 10.0f, glm::vec4(2.0f));
    target.value[target.index(1, 1)] = glm::vec4(std::numeric_limits<float>::quiet_NaN());
    target.value[target.index(2, 2)] = glm::vec4(std::numeric_limits<float>::infinity());
    const DenoiseReference frame_one = denoise_reference(target, DenoiseHistory{}, settings, false);
    const DenoiseReference frame_two = denoise_reference(flat_target(4, 4, 10.0f, glm::vec4(2.0f)), frame_one.history, settings, false);
    for (const glm::vec4& value : frame_two.output) {
        CHECK(std::isfinite(value.r));
        CHECK(std::isfinite(value.a));
    }
    // The two bad samples count as zero in the prefilter, so the 3 x 3 box around (1, 1) averages seven 2s and two 0s.
    CHECK(frame_one.output[frame_one.index(1, 1)].r == Catch::Approx(14.0f / 9.0f));
}

TEST_CASE("The parameter block has the layout the shader reads", "[unit]") {
    DenoiseSettings settings;
    settings.max_age = 7;
    settings.spatial_radius = 2;
    const std::vector<std::uint8_t> bytes = pack_denoise_parameters(settings, 320, 180, true, false);
    REQUIRE(bytes.size() == kDenoiseParameterBytes);
    std::uint32_t words[8];
    std::memcpy(words, bytes.data(), sizeof(words));
    CHECK(words[0] == 320u);
    CHECK(words[1] == 180u);
    CHECK(words[2] == 1u);  // reset, and no valid history
    CHECK(words[3] == 7u);
    CHECK(words[7] == 2u);
}
