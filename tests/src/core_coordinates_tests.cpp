#include "engine/core/coordinates.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <glm/ext/matrix_transform.hpp>

namespace coords = engine::core::coords;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Clip-space depth (NDC z) of a view-space point, after the perspective divide.
float ndc_depth(const glm::mat4& projection, const glm::vec3& view_point) {
    return coords::to_ndc(projection * glm::vec4(view_point, 1.0f)).z;
}

}  // namespace

TEST_CASE("Axes form a right-handed Y-up system with the camera looking down -Z", "[unit]") {
    CHECK(glm::cross(coords::kRight, coords::kUp) == coords::kBack);
    CHECK(glm::cross(coords::kUp, coords::kBack) == coords::kRight);
    CHECK(coords::kForward == glm::vec3(0.0f, 0.0f, -1.0f));
}

TEST_CASE("Matrices are column-major and act on column vectors", "[unit]") {
    const auto translation = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));

    CHECK(translation[3] == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
    CHECK(translation * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
}

TEST_CASE("Quaternions rotate by the right-hand rule", "[unit]") {
    // A +90 degree turn about +Y takes the forward axis (-Z) to -X.
    const auto turn = glm::angleAxis(kPi / 2.0f, coords::kUp);
    const glm::vec3 rotated = turn * coords::kForward;

    CHECK(rotated.x == Catch::Approx(-1.0f).margin(1e-5f));
    CHECK(rotated.y == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(rotated.z == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("Engine perspective maps near to depth 0 and far to depth 1", "[unit]") {
    const auto projection = coords::perspective(kPi / 3.0f, 16.0f / 9.0f, 0.5f, 1000.0f);

    CHECK(ndc_depth(projection, glm::vec3(0.0f, 0.0f, -0.5f)) == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(ndc_depth(projection, glm::vec3(0.0f, 0.0f, -1000.0f)) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("Engine clip space has Y pointing up, and Vulkan clip flips it", "[unit]") {
    const auto projection = coords::perspective(kPi / 3.0f, 1.0f, 0.1f, 100.0f);
    const glm::vec3 above_centre(0.0f, 1.0f, -2.0f);

    const auto engine_ndc = coords::to_ndc(projection * glm::vec4(above_centre, 1.0f));
    const auto vulkan_ndc = coords::to_ndc(coords::to_vulkan_clip(projection) * glm::vec4(above_centre, 1.0f));

    CHECK(engine_ndc.y > 0.0f);
    CHECK(vulkan_ndc.y == Catch::Approx(-engine_ndc.y).margin(1e-6f));
    CHECK(vulkan_ndc.x == Catch::Approx(engine_ndc.x).margin(1e-6f));
    CHECK(vulkan_ndc.z == Catch::Approx(engine_ndc.z).margin(1e-6f));
}

TEST_CASE("Orthographic clip space uses the same depth range as perspective", "[unit]") {
    const auto projection = coords::orthographic(-10.0f, 10.0f, -5.0f, 5.0f, 1.0f, 201.0f);

    CHECK(ndc_depth(projection, glm::vec3(0.0f, 0.0f, -1.0f)) == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(ndc_depth(projection, glm::vec3(0.0f, 0.0f, -201.0f)) == Catch::Approx(1.0f).margin(1e-5f));
}
