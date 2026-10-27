#include "engine/render/meshlet.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::render {
namespace {

using Triangle = std::array<std::uint32_t, 3>;

// The same triangle with its smallest index first. The cyclic order is kept, so the winding is part of the key.
Triangle canonical(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    if (a <= b && a <= c) {
        return {a, b, c};
    }
    if (b <= a && b <= c) {
        return {b, c, a};
    }
    return {c, a, b};
}

bool finite(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

std::optional<std::string> validate_meshlets(const MeshData& mesh, const MeshletData& meshlets) {
    if (const std::optional<std::string> problem = validate_mesh(mesh)) {
        return "mesh: " + *problem;
    }
    if (mesh.indices.empty()) {
        return std::string("a mesh with no triangles has no meshlets");
    }
    if (meshlets.meshlets.empty()) {
        return std::string("the meshlet list is empty");
    }
    for (std::size_t i = 0; i < meshlets.meshlets.size(); ++i) {
        const MeshletCluster& meshlet = meshlets.meshlets[i];
        const std::string where = "meshlet " + std::to_string(i) + ": ";
        if (meshlet.vertex_count == 0 || meshlet.vertex_count > kMeshletMaxVertices) {
            return where + "the vertex count must be from 1 to " + std::to_string(kMeshletMaxVertices);
        }
        if (meshlet.triangle_count == 0 || meshlet.triangle_count > kMeshletMaxTriangles) {
            return where + "the triangle count must be from 1 to " + std::to_string(kMeshletMaxTriangles);
        }
        if (static_cast<std::uint64_t>(meshlet.vertex_offset) + meshlet.vertex_count > meshlets.vertices.size()) {
            return where + "the vertex range is outside the vertex list";
        }
        if (static_cast<std::uint64_t>(meshlet.triangle_offset) + 3ull * meshlet.triangle_count > meshlets.triangles.size()) {
            return where + "the triangle range is outside the triangle list";
        }
        if (!finite(meshlet.center) || !std::isfinite(meshlet.radius) || meshlet.radius < 0.0f) {
            return where + "the bounding sphere is not finite";
        }
        if (!finite(meshlet.cone_axis) || !std::isfinite(meshlet.cone_cutoff) || meshlet.cone_cutoff < -1.0f || meshlet.cone_cutoff > 1.0f) {
            return where + "the normal cone is not valid";
        }
        // A small tolerance covers the float error of the cooker's sphere.
        const float reach = meshlet.radius * (1.0f + 1e-4f) + 1e-5f;
        for (std::uint32_t v = 0; v < meshlet.vertex_count; ++v) {
            const std::uint32_t vertex = meshlets.vertices[meshlet.vertex_offset + v];
            if (vertex >= mesh.positions.size()) {
                return where + "a vertex index is outside the mesh";
            }
            if (glm::length(mesh.positions[vertex] - meshlet.center) > reach) {
                return where + "a vertex lies outside the bounding sphere";
            }
        }
        for (std::uint32_t corner = 0; corner < 3 * meshlet.triangle_count; ++corner) {
            if (meshlets.triangles[meshlet.triangle_offset + corner] >= meshlet.vertex_count) {
                return where + "a triangle corner is outside the meshlet's vertices";
            }
        }
    }

    // The meshlets' triangles must be the mesh's triangles, each once. Both lists are canonical and sorted, so the comparison is exact.
    std::vector<Triangle> expected;
    expected.reserve(mesh.indices.size() / 3);
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        expected.push_back(canonical(mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]));
    }
    std::vector<Triangle> actual;
    actual.reserve(expected.size());
    for (const MeshletCluster& meshlet : meshlets.meshlets) {
        for (std::uint32_t t = 0; t < meshlet.triangle_count; ++t) {
            const std::size_t corner = meshlet.triangle_offset + 3ull * t;
            actual.push_back(canonical(meshlets.vertices[meshlet.vertex_offset + meshlets.triangles[corner]],
                                       meshlets.vertices[meshlet.vertex_offset + meshlets.triangles[corner + 1]],
                                       meshlets.vertices[meshlet.vertex_offset + meshlets.triangles[corner + 2]]));
        }
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    if (expected != actual) {
        return std::string("the meshlets' triangles are not the mesh's triangles");
    }
    return std::nullopt;
}

bool meshlet_visible(const MeshletCluster& meshlet, const glm::mat4& model, const Frustum& frustum, bool cone_test) noexcept {
    const glm::vec3 center = glm::vec3(model * glm::vec4(meshlet.center, 1.0f));
    // The longest column scales the radius, the same as the cull of the indirect path (camera.cpp keeps the sphere test itself).
    const float scale = std::max(glm::length(glm::vec3(model[0])), std::max(glm::length(glm::vec3(model[1])), glm::length(glm::vec3(model[2]))));
    const float radius = meshlet.radius * scale;
    if (!sphere_visible(frustum, center, radius)) {
        return false;
    }
    // meshoptimizer's sphere form of the backface test: the cone culls when dot(center - eye, axis) >= cutoff * |center - eye| + radius.
    // A cutoff of 1 never culls. The eye is the origin, because the model is camera-relative.
    if (cone_test && meshlet.cone_cutoff < 1.0f) {
        const glm::vec3 axis = glm::normalize(glm::inverseTranspose(glm::mat3(model)) * meshlet.cone_axis);
        if (glm::dot(center, axis) >= meshlet.cone_cutoff * glm::length(center) + radius) {
            return false;
        }
    }
    return true;
}

}  // namespace engine::render
