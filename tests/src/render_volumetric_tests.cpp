// The froxel fog's CPU reference and its contracts (US-045): the frame's validation, the slice and density math, the phase function, the
// homogeneous transmittance, the sun and local-light visibility, the temporal blend and its reset, and the application to a pixel. The GPU
// passes are checked against this reference in render_volumetric_gpu_tests.cpp.
#include "engine/render/volumetric_fog.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

using engine::render::FogFrame;
using engine::render::FogLight;
using engine::render::FogVolume;
using engine::render::apply_fog;
using engine::render::fog_density;
using engine::render::fog_froxel_center;
using engine::render::fog_phase;
using engine::render::fog_scene_view_depth;
using engine::render::fog_slice_edge;
using engine::render::integrate_reference;
using engine::render::inject_reference;
using engine::render::kFogFroxelBytes;
using engine::render::kFogMaxLights;
using engine::render::kFogParameterBytes;
using engine::render::pack_fog_parameters;
using engine::render::validate_fog;

namespace {

// A camera at the origin looking down -Z with a 90 degree vertical field of view on a square image.
FogFrame camera_frame() {
    FogFrame frame;
    frame.eye = glm::vec3(0.0f);
    frame.forward = glm::vec3(0.0f, 0.0f, -1.0f);
    frame.right = glm::vec3(1.0f, 0.0f, 0.0f);
    frame.up = glm::vec3(0.0f, 1.0f, 0.0f);
    frame.previous_eye = frame.eye;
    frame.previous_forward = frame.forward;
    frame.previous_right = frame.right;
    frame.previous_up = frame.up;
    frame.near_plane = 1.0f;
    frame.far_plane = 100.0f;
    frame.density = 0.05f;
    frame.albedo = glm::vec3(1.0f);
    frame.anisotropy = 0.0f;
    frame.sun_radiance = glm::vec3(0.0f);
    return frame;
}

constexpr std::uint32_t kW = 4;
constexpr std::uint32_t kH = 3;
constexpr std::uint32_t kD = 8;

// A visibility function that never blocks, and counts its calls. Pass it with std::ref, because the reference copies it.
struct CountingVisibility {
    int calls = 0;
    float operator()(const glm::vec3&, const glm::vec3&) {
        ++calls;
        return 1.0f;
    }
};

}  // namespace

TEST_CASE("The fog's frame is checked before any pass runs", "[unit]") {
    FogFrame frame = camera_frame();
    CHECK_FALSE(validate_fog(frame, kW, kH, kD).has_value());

    CHECK(validate_fog(frame, 0, kH, kD).has_value());

    FogFrame bad_slices = frame;
    bad_slices.far_plane = 0.5f;
    CHECK(validate_fog(bad_slices, kW, kH, kD).has_value());

    FogFrame bad_anisotropy = frame;
    bad_anisotropy.anisotropy = 1.0f;
    CHECK(validate_fog(bad_anisotropy, kW, kH, kD).has_value());

    FogFrame bad_albedo = frame;
    bad_albedo.albedo = glm::vec3(1.5f, 0.0f, 0.0f);
    CHECK(validate_fog(bad_albedo, kW, kH, kD).has_value());

    FogFrame too_many = frame;
    too_many.lights.assign(kFogMaxLights + 1, FogLight{});
    CHECK(validate_fog(too_many, kW, kH, kD).has_value());

    FogFrame bad_light = frame;
    bad_light.lights.push_back(FogLight{glm::vec3(0.0f), glm::vec3(1.0f), 0.0f, false});
    CHECK(validate_fog(bad_light, kW, kH, kD).has_value());
}

TEST_CASE("The slices grow exponentially from near to far, and the centres sit inside them", "[unit]") {
    const FogFrame frame = camera_frame();
    CHECK(fog_slice_edge(frame, kD, 0.0f) == Catch::Approx(1.0f));
    CHECK(fog_slice_edge(frame, kD, static_cast<float>(kD)) == Catch::Approx(100.0f));
    float previous = 0.0f;
    for (std::uint32_t z = 0; z <= kD; ++z) {
        const float edge = fog_slice_edge(frame, kD, static_cast<float>(z));
        CHECK(edge > previous);
        previous = edge;
    }
    // The centre of a slice lies on the view ray through its column's centre, at the slice's geometric middle.
    const glm::vec3 centre = fog_froxel_center(frame, kW, kH, kD, kW / 2, kH / 2, 0);
    CHECK(centre.z < 0.0f);
    CHECK(std::fabs(centre.x) < 0.5f);
    CHECK(-centre.z == Catch::Approx(fog_slice_edge(frame, kD, 0.5f)));
}

TEST_CASE("The density falls off with height and phase favours forward scattering", "[unit]") {
    FogFrame frame = camera_frame();
    frame.height_falloff = 0.1f;
    CHECK(fog_density(frame, 0.0f) == Catch::Approx(frame.density));
    CHECK(fog_density(frame, 10.0f) < fog_density(frame, 0.0f));

    frame.anisotropy = 0.6f;
    CHECK(fog_phase(1.0f, frame.anisotropy) > fog_phase(-1.0f, frame.anisotropy));
    CHECK(fog_phase(0.0f, 0.0f) == Catch::Approx(1.0f / (4.0f * 3.14159265f)));
}

TEST_CASE("A homogeneous medium with no light transmits exp(-density * depth) and adds no radiance", "[unit]") {
    const FogFrame frame = camera_frame();
    const FogVolume injected = inject_reference(frame, FogVolume{}, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 1.0f; });
    const FogVolume integrated = integrate_reference(frame, injected);
    const float expected = std::exp(-frame.density * (frame.far_plane - frame.near_plane));
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const glm::vec4 last = integrated.cells[integrated.index(x, y, kD - 1)];
            CHECK(last.a == Catch::Approx(expected).epsilon(1.0e-4));
            CHECK(last.r == Catch::Approx(0.0f));
        }
    }
}

TEST_CASE("The sun's visibility decides whether the fog is lit, and the shadow is asked along the sun direction", "[unit]") {
    FogFrame frame = camera_frame();
    frame.sun_direction = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    frame.sun_radiance = glm::vec3(2.0f);
    frame.shadows = true;

    int asked = 0;
    const glm::vec3 sun = frame.sun_direction;
    const FogVolume blocked = inject_reference(frame, FogVolume{}, kW, kH, kD, [&asked, sun](const glm::vec3& from, const glm::vec3& to) {
        ++asked;
        CHECK(glm::length(glm::normalize(to - from) - sun) < 1.0e-4f);
        return 0.0f;
    });
    CHECK(asked == static_cast<int>(kW * kH * kD));
    for (const glm::vec4& cell : blocked.cells) {
        CHECK(cell.r == 0.0f);
    }

    const FogVolume lit = inject_reference(frame, FogVolume{}, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 1.0f; });
    CHECK(lit.cells[lit.index(1, 1, 3)].r > 0.0f);

    // Shadows off: the visibility function is not asked, and the sun lights everything.
    frame.shadows = false;
    CountingVisibility never;
    const FogVolume unshadowed = inject_reference(frame, FogVolume{}, kW, kH, kD, std::ref(never));
    CHECK(never.calls == 0);
    CHECK(unshadowed.cells[unshadowed.index(1, 1, 3)].r == Catch::Approx(lit.cells[lit.index(1, 1, 3)].r));
}

TEST_CASE("A local light lights the fog near it, falls off with distance, and is shadowed only when asked", "[unit]") {
    FogFrame frame = camera_frame();
    frame.lights.push_back(FogLight{glm::vec3(0.0f, 0.0f, -10.0f), glm::vec3(5.0f), 30.0f, true});

    CountingVisibility counting;
    const FogVolume lit = inject_reference(frame, FogVolume{}, kW, kH, kD, std::ref(counting));
    CHECK(counting.calls == static_cast<int>(kW * kH * kD));
    // The centre slice is nearer to the light than the first slice's far neighbours, so it is brighter.
    CHECK(lit.cells[lit.index(kW / 2, kH / 2, kD / 2)].r > lit.cells[lit.index(0, 0, 0)].r);

    frame.lights[0].shadowed = false;
    CountingVisibility unused;
    static_cast<void>(inject_reference(frame, FogVolume{}, kW, kH, kD, std::ref(unused)));
    CHECK(unused.calls == 0);

    frame.lights[0].shadowed = true;
    const FogVolume occluded = inject_reference(frame, FogVolume{}, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 0.0f; });
    for (const glm::vec4& cell : occluded.cells) {
        CHECK(cell.r == 0.0f);
    }
}

TEST_CASE("The temporal blend keeps the previous value, decays it, and a reset discards it", "[unit]") {
    FogFrame lit = camera_frame();
    lit.sun_direction = glm::vec3(0.0f, 1.0f, 0.0f);
    lit.sun_radiance = glm::vec3(4.0f);
    lit.history_weight = 0.9f;
    lit.max_age = 15;
    const FogVolume previous = inject_reference(lit, FogVolume{}, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 1.0f; });
    REQUIRE(previous.cells[previous.index(1, 1, 3)].a == 1.0f);

    // The light goes out. The previous grid still carries its value, the blend keeps part of it, and the age grows.
    FogFrame dark = camera_frame();
    dark.history_weight = 0.9f;
    dark.max_age = 15;
    const FogVolume blended = inject_reference(dark, previous, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 1.0f; });
    const glm::vec4 cell = blended.cells[blended.index(1, 1, 3)];
    CHECK(cell.r > 0.0f);
    CHECK(cell.r < previous.cells[previous.index(1, 1, 3)].r);
    CHECK(cell.a == Catch::Approx(2.0f));

    // A reset ignores the history: the dark medium has no light at all.
    dark.reset = true;
    const FogVolume reset = inject_reference(dark, previous, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 1.0f; });
    CHECK(reset.cells[reset.index(1, 1, 3)].r == 0.0f);
    CHECK(reset.cells[reset.index(1, 1, 3)].a == 1.0f);
}

TEST_CASE("A stale history stops contributing, so the grid converges to the current light", "[unit]") {
    FogFrame frame = camera_frame();
    frame.history_weight = 0.9f;
    frame.max_age = 15;
    const auto visible = [](const glm::vec3&, const glm::vec3&) { return 1.0f; };
    FogFrame bright = frame;
    bright.sun_direction = glm::vec3(0.0f, 1.0f, 0.0f);
    bright.sun_radiance = glm::vec3(4.0f);
    FogVolume grid = inject_reference(bright, FogVolume{}, kW, kH, kD, visible);
    const FogVolume settled = inject_reference(frame, FogVolume{}, kW, kH, kD, visible);
    for (int i = 0; i < 200; ++i) {
        grid = inject_reference(frame, grid, kW, kH, kD, visible);
    }
    CHECK(grid.cells[grid.index(1, 1, 3)].r == Catch::Approx(settled.cells[settled.index(1, 1, 3)].r).margin(1.0e-4));
}

TEST_CASE("Applying the fog attenuates the colour by the transmittance and adds the accumulated radiance", "[unit]") {
    const FogFrame frame = camera_frame();
    const FogVolume integrated = integrate_reference(frame, inject_reference(frame, FogVolume{}, kW, kH, kD, [](const glm::vec3&, const glm::vec3&) { return 1.0f; }));
    const glm::vec3 color(0.5f, 0.25f, 1.0f);
    const glm::vec3 fogged = apply_fog(color, integrated, frame, 1, 1, 50.0f);
    CHECK(fogged.r < color.r);
    CHECK(fogged.g < color.g);
    CHECK(fogged.b < color.b);
    CHECK(fogged.r > 0.0f);
}

TEST_CASE("The fog's parameter block holds the frame, its lights, and the flags the shader reads", "[unit]") {
    FogFrame frame = camera_frame();
    frame.lights.push_back(FogLight{glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(4.0f, 5.0f, 6.0f), 7.0f, true});
    frame.reset = false;
    frame.shadows = true;
    const std::vector<std::uint8_t> bytes = pack_fog_parameters(frame, kW, kH, kD, true);
    REQUIRE(bytes.size() == kFogParameterBytes);
    std::uint32_t words[8];
    std::memcpy(words, bytes.data() + 192, sizeof(words));
    CHECK(words[0] == kW);
    CHECK(words[1] == kH);
    CHECK(words[2] == kD);
    CHECK(words[3] == (2u | 4u));  // history valid and shadows, not reset
    float light[8];
    std::memcpy(light, bytes.data() + 224, sizeof(light));
    CHECK(light[0] == 1.0f);
    CHECK(light[3] == 7.0f);       // range
    CHECK(light[4] == 4.0f);       // radiance
    std::uint32_t shadowed = 0;
    std::memcpy(&shadowed, bytes.data() + 224 + 28, sizeof(shadowed));
    CHECK(shadowed == 1u);
    CHECK(kFogFroxelBytes == 16u);
}

TEST_CASE("The scene's NDC depth converts to view depth on the camera's range, and the cleared background is the far plane", "[unit]") {
    const float near_plane = 1.0f;
    const float far_plane = 100.0f;
    // The perspective maps view depth d to NDC f / (f - n) * (1 - n / d), so a round trip returns d.
    for (const float d : {1.5f, 10.0f, 42.0f, 99.0f}) {
        const float ndc = far_plane / (far_plane - near_plane) * (1.0f - near_plane / d);
        CHECK(fog_scene_view_depth(ndc, near_plane, far_plane) == Catch::Approx(d).epsilon(1.0e-4));
    }
    // NDC 0 is the cleared background (no geometry), and NDC 1 is the far plane.
    CHECK(fog_scene_view_depth(0.0f, near_plane, far_plane) == far_plane);
    CHECK(fog_scene_view_depth(1.0f, near_plane, far_plane) == Catch::Approx(far_plane));
    CHECK(fog_scene_view_depth(1.5f, near_plane, far_plane) == Catch::Approx(far_plane));
}

TEST_CASE("The fog's camera range is checked, and it reaches the parameter block after the lights", "[unit]") {
    FogFrame frame = camera_frame();
    frame.camera_near = 0.5f;
    frame.camera_far = 250.0f;
    CHECK_FALSE(validate_fog(frame, kW, kH, kD).has_value());

    frame.camera_near = 0.0f;
    CHECK(validate_fog(frame, kW, kH, kD).has_value());
    frame.camera_near = 300.0f;
    CHECK(validate_fog(frame, kW, kH, kD).has_value());

    frame.camera_near = 0.5f;
    const std::vector<std::uint8_t> bytes = pack_fog_parameters(frame, kW, kH, kD, false);
    float range[2];
    std::memcpy(range, bytes.data() + 736, sizeof(range));
    CHECK(range[0] == 0.5f);
    CHECK(range[1] == 250.0f);
}
