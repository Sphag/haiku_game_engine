// Mesh cooking (US-023): the indexed geometry, the LOD chain, the bounds, and the meshlets that cook_mesh() produces, checked
// against the source triangles and against the limits in the payload format. The meshes are built in code, so the expected
// values are known: an n-by-n grid of quads has 2n^2 triangles over (n+1)^2 vertices, and the bounds of a grid are its box.

#include "asset_converter/mesh_cooking.hpp"
#include "asset_converter/model_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using engine::converter::cook_mesh;
using engine::converter::Meshlet;
using engine::converter::MeshLod;
using engine::converter::ModelMesh;
using engine::converter::ModelPayload;
using engine::converter::kMaxLodCount;
using engine::converter::kMeshletMaxTriangles;
using engine::converter::kMeshletMaxVertices;
using engine::converter::read_model_payload;
using engine::converter::validate_model;
using engine::converter::write_model_payload;

namespace {

constexpr float kEpsilon = 1e-4f;

bool near(float a, float b) {
    return std::fabs(a - b) <= kEpsilon;
}

bool near(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

struct Grid {
    ModelMesh mesh;
    std::vector<std::uint32_t> triangles;
};

// An n-by-n grid of quads in the z = 0 plane. Every vertex has a normal and a UV that follow its position, so the
// correspondence checks cover all attributes.
Grid make_grid(std::uint32_t n) {
    Grid grid;
    for (std::uint32_t y = 0; y <= n; ++y) {
        for (std::uint32_t x = 0; x <= n; ++x) {
            grid.mesh.positions.push_back(glm::vec3(static_cast<float>(x), static_cast<float>(y), 0.0f));
            grid.mesh.normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
            grid.mesh.uvs.push_back(glm::vec2(static_cast<float>(x) / static_cast<float>(n), static_cast<float>(y) / static_cast<float>(n)));
        }
    }
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::uint32_t a = y * (n + 1) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + n + 1;
            const std::uint32_t d = c + 1;
            grid.triangles.insert(grid.triangles.end(), {a, c, b, b, c, d});
        }
    }
    return grid;
}

// A vertex's attributes as numbers: position, normal, and UV. A triangle is three of these, rotated so the smallest comes
// first, so two triangles with the same corners compare equal whatever their starting corner.
using Corner = std::array<float, 8>;
using Triangle = std::array<Corner, 3>;

Corner corner_of(const ModelMesh& mesh, std::uint32_t vertex) {
    Corner corner{};
    const glm::vec3 position = mesh.positions[vertex];
    const glm::vec3 normal = mesh.normals.empty() ? glm::vec3(0.0f) : mesh.normals[vertex];
    const glm::vec2 uv = mesh.uvs.empty() ? glm::vec2(0.0f) : mesh.uvs[vertex];
    corner = {position.x, position.y, position.z, normal.x, normal.y, normal.z, uv.x, uv.y};
    return corner;
}

Triangle canonical(Triangle triangle) {
    const auto smallest = std::min_element(triangle.begin(), triangle.end());
    std::rotate(triangle.begin(), smallest, triangle.end());
    return triangle;
}

std::vector<Triangle> source_triangles(const ModelMesh& mesh, const std::vector<std::uint32_t>& indices) {
    std::vector<Triangle> triangles;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        triangles.push_back(canonical({corner_of(mesh, indices[t]), corner_of(mesh, indices[t + 1]), corner_of(mesh, indices[t + 2])}));
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

// The triangles of a flat index list as a sorted set, each rotated to start at its smallest index. Two lists hold the same
// triangles exactly when their sets are equal, whatever their order and starting corners.
std::vector<std::array<std::uint32_t, 3>> triangle_set(const std::vector<std::uint32_t>& indices) {
    std::vector<std::array<std::uint32_t, 3>> triangles;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        std::array<std::uint32_t, 3> triangle = {indices[t], indices[t + 1], indices[t + 2]};
        std::rotate(triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

// Expands the meshlets of a LOD back into global vertex indices, meshlet by meshlet.
std::vector<std::uint32_t> expand_meshlets(const MeshLod& lod) {
    std::vector<std::uint32_t> indices;
    for (const Meshlet& meshlet : lod.meshlets) {
        for (std::uint32_t t = 0; t < meshlet.triangle_count; ++t) {
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                const std::uint8_t local = lod.meshlet_triangles[meshlet.triangle_offset + 3 * t + corner];
                indices.push_back(lod.meshlet_vertices[meshlet.vertex_offset + local]);
            }
        }
    }
    return indices;
}

ModelPayload payload_of(const ModelMesh& mesh) {
    ModelPayload model;
    model.meshes.push_back(mesh);
    return model;
}

// The payload rules must accept the mesh. The message names the rule that failed.
void check_valid(const ModelMesh& mesh) {
    auto valid = validate_model(payload_of(mesh));
    INFO("validation: " << (valid.ok() ? "" : valid.error()));
    CHECK(valid.ok());
}

ModelMesh cooked_grid(std::uint32_t n) {
    Grid grid = make_grid(n);
    auto cooked = cook_mesh(grid.mesh, grid.triangles);
    INFO("cook failed: " << (cooked.ok() ? "" : cooked.error()));
    REQUIRE(cooked.ok());
    return grid.mesh;
}

}  // namespace

TEST_CASE("Mesh cooking: LOD 0 reproduces the source triangles with their attributes", "[unit]") {
    Grid grid = make_grid(8);
    const std::vector<Triangle> expected = source_triangles(grid.mesh, grid.triangles);
    auto cooked = cook_mesh(grid.mesh, grid.triangles);
    REQUIRE(cooked.ok());
    CHECK(cooked.value().degenerate_triangles == 0);

    const ModelMesh& mesh = grid.mesh;
    REQUIRE(mesh.lods.size() >= 1);
    CHECK(mesh.lods[0].error == 0.0f);
    CHECK(source_triangles(mesh, mesh.lods[0].indices) == expected);
    CHECK(mesh.positions.size() == 81);  // every source vertex is used, so none are removed
    check_valid(mesh);
}

TEST_CASE("Mesh cooking: meshlets fit the limits, cover each LOD in order, and reproduce its triangles", "[unit]") {
    const ModelMesh mesh = cooked_grid(40);  // 3200 triangles
    REQUIRE(mesh.lods.size() >= 2);
    for (const MeshLod& lod : mesh.lods) {
        REQUIRE(!lod.meshlets.empty());
        std::size_t triangles = 0;
        for (const Meshlet& meshlet : lod.meshlets) {
            CHECK(meshlet.vertex_count >= 1);
            CHECK(meshlet.vertex_count <= kMeshletMaxVertices);
            CHECK(meshlet.triangle_count >= 1);
            CHECK(meshlet.triangle_count <= kMeshletMaxTriangles);
            triangles += meshlet.triangle_count;
        }
        CHECK(triangles == lod.indices.size() / 3);
        CHECK(triangle_set(expand_meshlets(lod)) == triangle_set(lod.indices));
    }
    // 3200 triangles at 124 per meshlet need at least 26 meshlets.
    CHECK(mesh.lods[0].meshlets.size() >= 26);
    check_valid(mesh);
}

TEST_CASE("Mesh cooking: the LOD chain halves the triangles and its error never decreases", "[unit]") {
    const ModelMesh mesh = cooked_grid(40);
    REQUIRE(mesh.lods.size() >= 2);
    REQUIRE(mesh.lods.size() <= kMaxLodCount);
    for (std::size_t lod = 1; lod < mesh.lods.size(); ++lod) {
        CHECK(mesh.lods[lod].indices.size() < mesh.lods[lod - 1].indices.size());
        CHECK(mesh.lods[lod].error >= mesh.lods[lod - 1].error);
        for (std::uint32_t index : mesh.lods[lod].indices) {
            CHECK(index < mesh.positions.size());
        }
    }
    // Errors are absolute and small on a flat grid. The coarsest level may carry a little error from its boundary.
    CHECK(mesh.lods.back().error < 0.01f * glm::length(glm::vec3(40.0f, 40.0f, 0.0f)));
}

TEST_CASE("Mesh cooking: bounds are the exact box, the sphere reaches every vertex, and meshlet spheres reach theirs",
          "[unit]") {
    Grid grid = make_grid(4);
    for (glm::vec3& position : grid.mesh.positions) {
        position += glm::vec3(10.0f, -2.0f, 3.0f);
    }
    REQUIRE(cook_mesh(grid.mesh, grid.triangles).ok());
    const ModelMesh& mesh = grid.mesh;

    CHECK(mesh.bounds.min == glm::vec3(10.0f, -2.0f, 3.0f));
    CHECK(mesh.bounds.max == glm::vec3(14.0f, 2.0f, 3.0f));
    CHECK(near(mesh.bounds.center, glm::vec3(12.0f, 0.0f, 3.0f)));
    CHECK(near(mesh.bounds.radius, std::sqrt(8.0f)));  // half-diagonal of the 4 by 4 square
    for (const glm::vec3& position : mesh.positions) {
        CHECK(glm::length(position - mesh.bounds.center) <= mesh.bounds.radius + kEpsilon);
    }

    for (const MeshLod& lod : mesh.lods) {
        for (const Meshlet& meshlet : lod.meshlets) {
            for (std::uint32_t k = 0; k < meshlet.vertex_count; ++k) {
                const glm::vec3 position = mesh.positions[lod.meshlet_vertices[meshlet.vertex_offset + k]];
                CHECK(glm::length(position - meshlet.center) <= meshlet.radius + kEpsilon);
            }
            CHECK(near(glm::length(meshlet.cone_axis), 1.0f));
        }
    }
}

TEST_CASE("Mesh cooking: a single triangle is one LOD with one meshlet", "[unit]") {
    ModelMesh mesh;
    mesh.positions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    REQUIRE(cook_mesh(mesh, {2, 0, 1}).ok());
    REQUIRE(mesh.lods.size() == 1);
    CHECK(mesh.lods[0].indices.size() == 3);
    REQUIRE(mesh.lods[0].meshlets.size() == 1);
    CHECK(mesh.lods[0].meshlets[0].vertex_count == 3);
    CHECK(mesh.lods[0].meshlets[0].triangle_count == 1);
    CHECK(mesh.bounds.max == glm::vec3(1.0f, 1.0f, 0.0f));
    check_valid(mesh);
}

TEST_CASE("Mesh cooking: triangles that repeat a vertex are dropped and reported, and the rest cook", "[unit]") {
    ModelMesh mesh;
    mesh.positions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f)};
    // Two good triangles, one that repeats an index, and one that is a single point.
    auto cooked = cook_mesh(mesh, {0, 1, 2, 0, 0, 1, 1, 1, 1, 2, 3, 1});
    REQUIRE(cooked.ok());
    CHECK(cooked.value().degenerate_triangles == 2);
    CHECK(mesh.lods[0].indices.size() == 6);
    check_valid(mesh);
}

TEST_CASE("Mesh cooking: a mesh whose triangles all degenerate, or that has none, cooks to an empty mesh", "[unit]") {
    ModelMesh all_degenerate;
    all_degenerate.positions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)};
    all_degenerate.normals = {glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    auto cooked = cook_mesh(all_degenerate, {0, 0, 1, 1, 1, 1});
    REQUIRE(cooked.ok());
    CHECK(cooked.value().degenerate_triangles == 2);
    CHECK(all_degenerate.positions.empty());
    CHECK(all_degenerate.normals.empty());
    REQUIRE(all_degenerate.lods.size() == 1);
    CHECK(all_degenerate.lods[0].indices.empty());
    CHECK(all_degenerate.lods[0].meshlets.empty());
    CHECK(all_degenerate.bounds.radius == 0.0f);

    ModelMesh none;
    none.positions = {glm::vec3(0.0f)};
    REQUIRE(cook_mesh(none, {}).ok());
    CHECK(none.positions.empty());
    REQUIRE(none.lods.size() == 1);
    CHECK(none.lods[0].indices.empty());
    check_valid(none);
}

TEST_CASE("Mesh cooking: vertices with identical attributes merge, and vertices no triangle uses are removed", "[unit]") {
    ModelMesh mesh;
    mesh.positions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f),
                      glm::vec3(9.0f, 9.0f, 9.0f)};
    mesh.normals.assign(5, glm::vec3(0.0f, 0.0f, 1.0f));
    const std::vector<std::uint32_t> source = {0, 1, 2, 3, 2, 1};
    const std::vector<Triangle> expected = source_triangles(mesh, source);
    REQUIRE(cook_mesh(mesh, source).ok());
    CHECK(mesh.positions.size() == 3);  // vertex 3 duplicates 0, and vertex 4 is unused
    CHECK(mesh.normals.size() == 3);
    CHECK(source_triangles(mesh, mesh.lods[0].indices) == expected);  // same corners after the merge
    check_valid(mesh);
}

TEST_CASE("Mesh cooking: invalid input fails without changing the mesh", "[unit]") {
    ModelMesh mesh;
    mesh.positions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    mesh.normals = {glm::vec3(0.0f, 0.0f, 1.0f)};  // one normal for three vertices
    CHECK(!cook_mesh(mesh, {0, 1, 2}).ok());
    CHECK(mesh.positions.size() == 3);
    CHECK(mesh.lods.empty());

    mesh.normals.clear();
    CHECK(!cook_mesh(mesh, {0, 1}).ok());   // not a whole triangle
    CHECK(!cook_mesh(mesh, {0, 1, 3}).ok());  // index out of range

    mesh.positions[2].y = std::numeric_limits<float>::quiet_NaN();
    CHECK(!cook_mesh(mesh, {0, 1, 2}).ok());
    CHECK(mesh.positions.size() == 3);
    CHECK(mesh.lods.empty());
}

TEST_CASE("Mesh cooking: corrupt cooked data is rejected by the payload rules", "[unit]") {
    const ModelMesh cooked = cooked_grid(8);
    REQUIRE(validate_model(payload_of(cooked)).ok());

    const auto rejects = [&cooked](auto mutate) {
        ModelPayload model = payload_of(cooked);
        mutate(model.meshes[0]);
        return !validate_model(model).ok();
    };

    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].indices[0] = (mesh.lods[0].indices[0] + 1) % mesh.positions.size(); }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlet_triangles[0] = 255; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlets[0].vertex_count = kMeshletMaxVertices + 1; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlets[0].triangle_count = kMeshletMaxTriangles + 1; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlets[0].triangle_offset += 3; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlets.pop_back(); }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlets[0].radius = 0.0f; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].meshlets[0].cone_axis = glm::vec3(2.0f, 0.0f, 0.0f); }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.bounds.min.x += 1.0f; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.bounds.radius = 0.0f; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[0].error = 0.5f; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods.clear(); }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods.resize(kMaxLodCount + 1); }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[1].error = -1.0f; }));
    CHECK(rejects([](ModelMesh& mesh) { mesh.lods[1].indices.push_back(0); }));
}

TEST_CASE("Mesh cooking: cooked geometry survives a write and read unchanged", "[unit]") {
    const ModelMesh cooked = cooked_grid(12);
    const ModelPayload model = payload_of(cooked);
    auto back = read_model_payload(write_model_payload(model));
    REQUIRE(back.ok());
    const ModelMesh& mesh = back.value().meshes[0];

    CHECK(mesh.bounds.min == cooked.bounds.min);
    CHECK(mesh.bounds.max == cooked.bounds.max);
    CHECK(mesh.bounds.radius == cooked.bounds.radius);
    REQUIRE(mesh.lods.size() == cooked.lods.size());
    for (std::size_t lod = 0; lod < mesh.lods.size(); ++lod) {
        const MeshLod& a = cooked.lods[lod];
        const MeshLod& b = mesh.lods[lod];
        CHECK(b.error == a.error);
        CHECK(b.indices == a.indices);
        CHECK(b.meshlet_vertices == a.meshlet_vertices);
        CHECK(b.meshlet_triangles == a.meshlet_triangles);
        REQUIRE(b.meshlets.size() == a.meshlets.size());
        for (std::size_t m = 0; m < a.meshlets.size(); ++m) {
            CHECK(b.meshlets[m].vertex_offset == a.meshlets[m].vertex_offset);
            CHECK(b.meshlets[m].vertex_count == a.meshlets[m].vertex_count);
            CHECK(b.meshlets[m].triangle_offset == a.meshlets[m].triangle_offset);
            CHECK(b.meshlets[m].triangle_count == a.meshlets[m].triangle_count);
            CHECK(near(b.meshlets[m].center, a.meshlets[m].center));
            CHECK(b.meshlets[m].radius == a.meshlets[m].radius);
            CHECK(near(b.meshlets[m].cone_axis, a.meshlets[m].cone_axis));
            CHECK(b.meshlets[m].cone_cutoff == a.meshlets[m].cone_cutoff);
        }
    }
}
