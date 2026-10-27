// Meshlets for the mesh shader path (US-038), on the CPU. A mesh cooked by the converter must validate against its source triangles. Each
// broken rule must be rejected: the limits, the ranges, the bounding spheres, and the triangle coverage. The CPU reference of the task stage's
// frustum and cone tests must agree with what the GPU is specified to do on clear cases.

#include "render_meshlet_fixture.hpp"

#include "engine/render/camera.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/meshlet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cstdint>
#include <string>

using engine::render::Frustum;
using engine::render::make_box_mesh;
using engine::render::make_frustum;
using engine::render::make_plane_mesh;
using engine::render::make_sphere_mesh;
using engine::render::MeshData;
using engine::render::MeshletCluster;
using engine::render::MeshletData;
using engine::render::meshlet_visible;
using engine::render::tests::cook_for_render;
using engine::render::tests::CookedMesh;
using engine::render::validate_meshlets;

namespace {

// A camera at the origin looking down -Z, with depth in [0, 1] as the engine uses.
Frustum eye_frustum() {
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    return make_frustum(projection);
}

// A single flat square at z = -5 centered on the view axis. Its normal is +Z (toward the eye) or -Z (away from it).
MeshletCluster flat_square(const glm::vec3& center, const glm::vec3& normal, float cutoff) {
    MeshletCluster square;
    square.vertex_count = 4;
    square.triangle_count = 2;
    square.center = center;
    square.radius = 0.71f;
    square.cone_axis = normal;
    square.cone_cutoff = cutoff;
    return square;
}

}  // namespace

TEST_CASE("A cooked box and a cooked sphere validate against their source triangles", "[unit]") {
    const CookedMesh box = cook_for_render(make_box_mesh(glm::vec3(0.5f)));
    CHECK_FALSE(validate_meshlets(box.mesh, box.meshlets).has_value());
    CHECK(box.meshlets.meshlets.size() == 1);  // twelve triangles fit one meshlet

    // A sphere has enough triangles for several meshlets, so each meshlet's own range and sphere are checked.
    const CookedMesh sphere = cook_for_render(make_sphere_mesh(1.0f, 12, 16));
    CHECK_FALSE(validate_meshlets(sphere.mesh, sphere.meshlets).has_value());
    CHECK(sphere.meshlets.meshlets.size() >= 2);
    std::size_t triangles = 0;
    for (const MeshletCluster& meshlet : sphere.meshlets.meshlets) {
        triangles += meshlet.triangle_count;
        CHECK(meshlet.vertex_count <= engine::render::kMeshletMaxVertices);
        CHECK(meshlet.triangle_count <= engine::render::kMeshletMaxTriangles);
    }
    CHECK(triangles == sphere.mesh.indices.size() / 3);
}

TEST_CASE("Meshlet validation rejects limits, ranges, spheres, and coverage that do not match the mesh", "[unit]") {
    const CookedMesh good = cook_for_render(make_sphere_mesh(1.0f, 12, 16));
    REQUIRE_FALSE(validate_meshlets(good.mesh, good.meshlets).has_value());

    SECTION("a meshlet with no vertices") {
        MeshletData broken = good.meshlets;
        broken.meshlets[0].vertex_count = 0;
        const auto problem = validate_meshlets(good.mesh, broken);
        REQUIRE(problem.has_value());
        CHECK(problem->find("vertex count") != std::string::npos);
    }
    SECTION("more vertices than the mesh stage writes") {
        MeshletData broken = good.meshlets;
        broken.meshlets[0].vertex_count = engine::render::kMeshletMaxVertices + 1;
        REQUIRE(validate_meshlets(good.mesh, broken).has_value());
    }
    SECTION("a triangle corner that names a vertex the meshlet does not have") {
        MeshletData broken = good.meshlets;
        broken.triangles[broken.meshlets[0].triangle_offset] = static_cast<std::uint8_t>(broken.meshlets[0].vertex_count);
        const auto problem = validate_meshlets(good.mesh, broken);
        REQUIRE(problem.has_value());
        CHECK(problem->find("triangle corner") != std::string::npos);
    }
    SECTION("a vertex range past the end of the vertex list") {
        MeshletData broken = good.meshlets;
        broken.meshlets.back().vertex_offset = static_cast<std::uint32_t>(broken.vertices.size());
        const auto problem = validate_meshlets(good.mesh, broken);
        REQUIRE(problem.has_value());
        CHECK(problem->find("vertex range") != std::string::npos);
    }
    SECTION("a bounding sphere that does not contain its vertices") {
        MeshletData broken = good.meshlets;
        broken.meshlets[0].radius = 0.0f;
        const auto problem = validate_meshlets(good.mesh, broken);
        REQUIRE(problem.has_value());
        CHECK(problem->find("bounding sphere") != std::string::npos);
    }
    SECTION("a vertex that names a different mesh vertex, so the triangles change") {
        MeshletData broken = good.meshlets;
        // Point one meshlet vertex at a vertex of another meshlet: the sphere may still hold it, but the triangles no longer match.
        const std::uint32_t other = broken.vertices[broken.meshlets.back().vertex_offset];
        broken.vertices[broken.meshlets[0].vertex_offset] = other;
        CHECK(validate_meshlets(good.mesh, broken).has_value());
    }
    SECTION("a meshlet that drops a triangle") {
        MeshletData broken = good.meshlets;
        broken.meshlets[0].triangle_count -= 1;
        const auto problem = validate_meshlets(good.mesh, broken);
        REQUIRE(problem.has_value());
        CHECK(problem->find("not the mesh's triangles") != std::string::npos);
    }
    SECTION("no meshlets for a mesh with triangles") {
        MeshletData none;
        REQUIRE(validate_meshlets(good.mesh, none).has_value());
    }
}

TEST_CASE("The meshlet visibility reference culls a meshlet outside the frustum and one whose cone faces away", "[unit]") {
    const Frustum frustum = eye_frustum();
    const glm::mat4 identity(1.0f);

    // Facing the eye: the normal +Z points back at the camera at the origin. Visible.
    CHECK(meshlet_visible(flat_square({0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}, 0.0f), identity, frustum));
    // Facing away: the normal -Z points away from the camera. A flat cone (cutoff 0) culls the whole meshlet.
    CHECK_FALSE(meshlet_visible(flat_square({0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, -1.0f}, 0.0f), identity, frustum));
    // A cutoff of one never culls, however the normals face.
    CHECK(meshlet_visible(flat_square({0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, -1.0f}, 1.0f), identity, frustum));
    // Outside the frustum on the right, even facing the eye.
    CHECK_FALSE(meshlet_visible(flat_square({100.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}, 0.0f), identity, frustum));
    // The model moves the sphere and turns the cone with it: a translation of -5 in Z puts the square at the origin's distance of 10, and it still faces the eye.
    const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
    CHECK(meshlet_visible(flat_square({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.0f), moved, frustum));
    CHECK_FALSE(meshlet_visible(flat_square({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.0f), moved, frustum));
}

TEST_CASE("A flipped plane's meshlet is culled by its cone, and a box's meshlet is not", "[unit]") {
    const Frustum frustum = eye_frustum();
    const glm::mat4 identity(1.0f);
    // A plane whose front faces the camera, and the same plane with its winding and normals reversed.
    const CookedMesh front = cook_for_render(make_plane_mesh(1.0f));
    MeshData flipped = make_plane_mesh(1.0f);
    for (glm::vec3& normal : flipped.normals) {
        normal = -normal;
    }
    for (std::size_t t = 0; t < flipped.indices.size(); t += 3) {
        std::swap(flipped.indices[t + 1], flipped.indices[t + 2]);
    }
    const CookedMesh back = cook_for_render(flipped);
    REQUIRE(front.meshlets.meshlets.size() == 1);
    REQUIRE(back.meshlets.meshlets.size() == 1);
    // Both planes sit in front of the camera at z = -5; the back one is flipped, so its cone axis is -Z.
    MeshletCluster front_meshlet = front.meshlets.meshlets[0];
    MeshletCluster back_meshlet = back.meshlets.meshlets[0];
    front_meshlet.center = glm::vec3(0.0f, 0.0f, -5.0f) + front_meshlet.center;
    back_meshlet.center = glm::vec3(0.0f, 0.0f, -5.0f) + back_meshlet.center;
    CHECK(meshlet_visible(front_meshlet, identity, frustum));
    CHECK_FALSE(meshlet_visible(back_meshlet, identity, frustum));

    // A box's normals cover every direction, so the cooker stores a cutoff of one and the cone never culls it.
    const CookedMesh box = cook_for_render(make_box_mesh(glm::vec3(0.5f)));
    REQUIRE(box.meshlets.meshlets.size() == 1);
    CHECK(box.meshlets.meshlets[0].cone_cutoff >= 1.0f);
    CHECK(meshlet_visible(box.meshlets.meshlets[0], glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f)), frustum));
}
