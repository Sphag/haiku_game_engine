// Screen-space reflections (US-040), on the CPU. A wall fills the frame at 10 m, so a reflected ray that runs straight into it hits, a ray that
// leaves the frame misses, and a rough surface skips the trace. Every miss must return the environment exactly, never black.

#include "engine/gi/screen_reflections.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <limits>

using engine::gi::reflection_radiance;
using engine::gi::ReflectionView;
using engine::gi::ScreenBuffers;
using engine::gi::ScreenHit;
using engine::gi::ScreenTraceSettings;
using engine::gi::trace_screen_reflection;

namespace {

constexpr std::uint32_t kSide = 64;
const glm::vec3 kWallColor(0.2f, 0.4f, 0.6f);
const glm::vec3 kEnvironment(0.9f, 0.5f, 0.1f);

// A 10 m wall that faces the eye and covers the frame.
ScreenBuffers wall_buffers() {
    ScreenBuffers buffers;
    buffers.width = kSide;
    buffers.height = kSide;
    buffers.view_depth.assign(static_cast<std::size_t>(kSide) * kSide, 10.0f);
    buffers.radiance.assign(static_cast<std::size_t>(kSide) * kSide, kWallColor);
    return buffers;
}

ReflectionView view() {
    ReflectionView reflection;
    reflection.projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    return reflection;
}

// Fine steps (0.04 m) so the thickness test cannot step over the wall, and a 20 m reach that is past the wall.
ScreenTraceSettings fine_settings() {
    ScreenTraceSettings settings;
    settings.max_steps = 512;
    settings.max_distance = 20.0f;
    settings.thickness = 0.5f;
    return settings;
}

}  // namespace

TEST_CASE("A reflected ray that runs into a visible surface returns its color", "[unit]") {
    const ScreenHit hit = trace_screen_reflection(wall_buffers(), view(), glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.1f,
                                                  fine_settings());
    REQUIRE(hit.hit);
    CHECK(hit.radiance.x == Catch::Approx(kWallColor.x));
    CHECK(hit.radiance.y == Catch::Approx(kWallColor.y));
    CHECK(hit.radiance.z == Catch::Approx(kWallColor.z));
    CHECK(hit.confidence == Catch::Approx(1.0f));
    CHECK(reflection_radiance(hit, kEnvironment) == kWallColor);
}

TEST_CASE("Off-screen, behind-the-eye, and rough reflections miss and return the environment exactly", "[unit]") {
    const ScreenBuffers buffers = wall_buffers();
    const ReflectionView reflection = view();
    const ScreenTraceSettings settings = fine_settings();

    // Heading sideways out of the frame: the ray leaves the screen before it reaches the wall.
    const glm::vec3 sideways = glm::normalize(glm::vec3(1.0f, 0.0f, -0.2f));
    const ScreenHit off_screen = trace_screen_reflection(buffers, reflection, glm::vec3(0.0f, 0.0f, -2.0f), sideways, 0.1f, settings);
    CHECK_FALSE(off_screen.hit);
    CHECK(off_screen.confidence == 0.0f);
    CHECK(reflection_radiance(off_screen, kEnvironment) == kEnvironment);

    // Pointing back at the eye: the ray never reaches the wall.
    const ScreenHit behind = trace_screen_reflection(buffers, reflection, glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, 1.0f), 0.1f, settings);
    CHECK_FALSE(behind.hit);
    CHECK(reflection_radiance(behind, kEnvironment) == kEnvironment);

    // A rough surface skips the trace, even when the same ray would hit.
    const ScreenHit rough = trace_screen_reflection(buffers, reflection, glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.9f, settings);
    CHECK_FALSE(rough.hit);
    CHECK(reflection_radiance(rough, kEnvironment) == kEnvironment);
}

TEST_CASE("A reflection with no usable screen data misses instead of reading past the buffers", "[unit]") {
    ScreenBuffers buffers = wall_buffers();
    buffers.view_depth.pop_back();  // one entry short of width * height
    const ScreenHit mismatched = trace_screen_reflection(buffers, view(), glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.1f,
                                                         fine_settings());
    CHECK_FALSE(mismatched.hit);

    const ScreenHit empty = trace_screen_reflection(ScreenBuffers{}, view(), glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.1f,
                                                    fine_settings());
    CHECK_FALSE(empty.hit);
}

TEST_CASE("Geometry behind the visible surface is not reflected, and an empty pixel is not a surface", "[unit]") {
    // The buffer holds nothing (infinite depth) everywhere, so the ray passes through and misses.
    ScreenBuffers empty = wall_buffers();
    empty.view_depth.assign(empty.view_depth.size(), std::numeric_limits<float>::infinity());
    const ScreenHit through = trace_screen_reflection(empty, view(), glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.1f,
                                                      fine_settings());
    CHECK_FALSE(through.hit);

    // The wall is 10 m away and the ray starts 2 m away, so the ray must reach the wall's depth from in front. A ray that starts behind the
    // wall never reads it.
    const ScreenHit behind_wall = trace_screen_reflection(wall_buffers(), view(), glm::vec3(0.0f, 0.0f, -12.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                                                          0.1f, fine_settings());
    CHECK_FALSE(behind_wall.hit);
}
