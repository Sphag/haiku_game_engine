#pragma once

// Meshlets for the mesh shader path (US-038). The converter cooks a mesh's LOD 0 into meshlets (tools/asset_converter/mesh_cooking.hpp); the
// renderer reads the same cooked triangles on both paths. A meshlet names a range of `vertices` (mesh vertex indices) and a range of
// `triangles` (three meshlet-local corner indices per triangle, one byte each), with a bounding sphere and a normal cone. The mesh stage writes
// at most kMeshletMaxVertices vertices and kMeshletMaxTriangles triangles per meshlet.
//
// The runtime never links the converter. A caller that cooked a mesh copies its LOD 0 into a MeshData (positions, normals, UVs, indices) and a
// MeshletData (the cooked clusters), and both go to the renderer. render_meshlet_fixture.hpp shows the conversion.

#include "engine/render/camera.hpp"
#include "engine/render/mesh.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {

// The limits the mesh stage's output arrays are declared with (scene.hlsl MSMeshlet).
inline constexpr std::uint32_t kMeshletMaxVertices = 64;
inline constexpr std::uint32_t kMeshletMaxTriangles = 124;

// One cluster. The ranges index MeshletData::vertices and MeshletData::triangles. The sphere is in the mesh's object space, and the cone is
// meshoptimizer's: cone_cutoff is the cosine that bounds the normals' spread, and 1 means the cone never culls.
struct MeshletCluster {
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_offset = 0;  // in bytes into MeshletData::triangles
    std::uint32_t triangle_count = 0;
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    glm::vec3 cone_axis{0.0f, 0.0f, 1.0f};
    float cone_cutoff = 1.0f;
};

// The cooked meshlets of one mesh's LOD 0. Clusters are packed back to back, and their triangles together are the mesh's triangles.
struct MeshletData {
    std::vector<MeshletCluster> meshlets;
    std::vector<std::uint32_t> vertices;     // mesh vertex indices, grouped per meshlet
    std::vector<std::uint8_t> triangles;     // meshlet-local corners, three per triangle
};

// Returns the first problem with `meshlets` as the meshlets of `mesh`, or nothing. Checks the limits and the ranges, that every vertex lies in
// its meshlet's sphere, and that the meshlets' triangles are exactly the mesh's triangles (the same three vertices in the same cyclic order,
// each once). A mesh with no triangles has no meshlets to attach.
[[nodiscard]] std::optional<std::string> validate_meshlets(const MeshData& mesh, const MeshletData& meshlets);

// The CPU reference of the task stage's test (scene.hlsl MeshletVisible): the meshlet's sphere, scaled by the model's longest column, must
// not be behind a frustum plane, and, when `cone_test` is set, its normal cone must not face away from the eye. The model is camera-relative,
// so the eye is the origin. `cone_test` is false for a two-sided material, whose back faces draw.
[[nodiscard]] bool meshlet_visible(const MeshletCluster& meshlet, const glm::mat4& model, const Frustum& frustum, bool cone_test = true) noexcept;

}  // namespace engine::render
