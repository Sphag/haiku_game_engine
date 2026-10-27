// Transform math (US-016): TRS composition, exact decomposition, and rejection of shear. CPU-only; these run on every OS.

#include "engine/scene/transform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>

using engine::scene::Transform;
using engine::scene::decompose_exact;

namespace {

Transform posed(glm::dvec3 position, glm::dquat rotation = glm::dquat(1.0, 0.0, 0.0, 0.0),
                glm::dvec3 scale = glm::dvec3(1.0, 1.0, 1.0)) {
    Transform result;
    result.position = position;
    result.rotation = rotation;
    result.scale = scale;
    return result;
}

glm::dquat turn(glm::dvec3 axis, double degrees) {
    return glm::angleAxis(glm::radians(degrees), axis);
}

bool near(glm::dvec3 actual, glm::dvec3 expected, double margin) {
    return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::dvec3(margin, margin, margin)));
}

bool near(const glm::dmat4& actual, const glm::dmat4& expected, double margin) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(actual[column][row] - expected[column][row]) > margin) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Scene transform applies scale, then rotation, then translation", "[unit]") {
    // Scale 2 turns (1,0,0) into (2,0,0). A 90 degree turn about +Y sends +X to -Z. Translation moves the result to x = 10.
    const Transform t = posed(glm::dvec3(10.0, 0.0, 0.0), turn(glm::dvec3(0.0, 1.0, 0.0), 90.0), glm::dvec3(2.0, 2.0, 2.0));
    const glm::dvec4 point = t.matrix() * glm::dvec4(1.0, 0.0, 0.0, 1.0);
    CHECK(near(glm::dvec3(point), glm::dvec3(10.0, 0.0, -2.0), 1e-12));
}

TEST_CASE("Scene transform scales each axis in local space before rotating", "[unit]") {
    // Non-uniform scale (1,2,3) and a 90 degree turn about +Z. Local +Y is stretched to 2 and then turned into -X.
    const Transform t = posed(glm::dvec3(0.0), turn(glm::dvec3(0.0, 0.0, 1.0), 90.0), glm::dvec3(1.0, 2.0, 3.0));
    CHECK(near(glm::dvec3(t.matrix() * glm::dvec4(1.0, 0.0, 0.0, 0.0)), glm::dvec3(0.0, 1.0, 0.0), 1e-12));
    CHECK(near(glm::dvec3(t.matrix() * glm::dvec4(0.0, 1.0, 0.0, 0.0)), glm::dvec3(-2.0, 0.0, 0.0), 1e-12));
    CHECK(near(glm::dvec3(t.matrix() * glm::dvec4(0.0, 0.0, 1.0, 0.0)), glm::dvec3(0.0, 0.0, 3.0), 1e-12));
}

TEST_CASE("Scene transform decomposition recovers a composed transform", "[unit]") {
    const Transform original =
        posed(glm::dvec3(-4.0, 5.0, 6.0), glm::normalize(glm::dquat(glm::radians(glm::dvec3(30.0, 45.0, 60.0)))),
              glm::dvec3(0.5, 2.0, 3.0));
    const std::optional<Transform> recovered = decompose_exact(original.matrix());
    REQUIRE(recovered.has_value());
    CHECK(near(recovered->position, original.position, 1e-12));
    CHECK(near(recovered->scale, original.scale, 1e-12));
    CHECK(near(recovered->matrix(), original.matrix(), 1e-12));
}

TEST_CASE("Scene transform decomposition keeps a mirror as a negative scale", "[unit]") {
    const Transform mirrored = posed(glm::dvec3(1.0, 2.0, 3.0), turn(glm::dvec3(0.0, 1.0, 0.0), 30.0),
                                     glm::dvec3(-2.0, 1.0, 3.0));
    const std::optional<Transform> recovered = decompose_exact(mirrored.matrix());
    REQUIRE(recovered.has_value());
    CHECK(near(recovered->scale, glm::dvec3(-2.0, 1.0, 3.0), 1e-12));
    CHECK(near(recovered->matrix(), mirrored.matrix(), 1e-12));
}

TEST_CASE("Scene transform decomposition rejects shear and degenerate matrices", "[unit]") {
    // Column 1 leans toward X, so the axes are not orthogonal.
    const glm::dmat4 sheared(glm::dvec4(1.0, 0.0, 0.0, 0.0), glm::dvec4(1.0, 1.0, 0.0, 0.0),
                             glm::dvec4(0.0, 0.0, 1.0, 0.0), glm::dvec4(0.0, 0.0, 0.0, 1.0));
    CHECK_FALSE(decompose_exact(sheared).has_value());

    // A zero scale axis has no rotation to recover.
    const glm::dmat4 flattened(glm::dvec4(0.0), glm::dvec4(0.0, 1.0, 0.0, 0.0), glm::dvec4(0.0, 0.0, 1.0, 0.0),
                               glm::dvec4(0.0, 0.0, 0.0, 1.0));
    CHECK_FALSE(decompose_exact(flattened).has_value());

    // A projective row (w column entries) is not affine.
    glm::dmat4 projective(1.0);
    projective[0][3] = 0.5;
    CHECK_FALSE(decompose_exact(projective).has_value());

    glm::dmat4 not_finite(1.0);
    not_finite[3][0] = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(decompose_exact(not_finite).has_value());
}

TEST_CASE("Scene transform validity rejects non-finite values and non-unit rotations", "[unit]") {
    CHECK(Transform{}.is_valid());
    CHECK(posed(glm::dvec3(0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(0.0, -1.0, 2.0)).is_valid());

    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(posed(glm::dvec3(nan, 0.0, 0.0)).is_valid());
    CHECK_FALSE(posed(glm::dvec3(0.0), glm::dquat(1.0, 0.0, 0.0, 0.0), glm::dvec3(1.0, nan, 1.0)).is_valid());
    CHECK_FALSE(posed(glm::dvec3(0.0), glm::dquat(2.0, 0.0, 0.0, 0.0)).is_valid());
    CHECK_FALSE(posed(glm::dvec3(0.0), glm::dquat(nan, 0.0, 0.0, 0.0)).is_valid());
}
