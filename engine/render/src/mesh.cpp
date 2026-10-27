#include "engine/render/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace engine::render {
namespace {

bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::vec2& v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

// Appends one face of a box. `normal` is the outward unit axis; `u` and `v` span the face, with (u, v, normal) right-handed so that the
// two triangles wind counter-clockwise from outside.
void add_face(MeshData& mesh, const glm::vec3& center, const glm::vec3& u, const glm::vec3& v, const glm::vec3& normal) {
    const auto base = static_cast<std::uint32_t>(mesh.positions.size());
    const glm::vec3 corners[4] = {center - u - v, center + u - v, center + u + v, center - u + v};
    const glm::vec2 uvs[4] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (std::size_t i = 0; i < 4; ++i) {
        mesh.positions.push_back(corners[i]);
        mesh.normals.push_back(normal);
        mesh.uvs.push_back(uvs[i]);
    }
    const std::uint32_t triangles[6] = {0, 1, 2, 0, 2, 3};
    for (const std::uint32_t index : triangles) {
        mesh.indices.push_back(base + index);
    }
}

}  // namespace

std::uint32_t skin_joint_count(const MeshData& mesh) noexcept {
    std::uint32_t count = 0;
    for (std::size_t v = 0; v < mesh.joints.size() && v < mesh.weights.size(); ++v) {
        for (int i = 0; i < 4; ++i) {
            if (mesh.weights[v][i] > 0.0f) {
                count = std::max(count, mesh.joints[v][i] + 1);
            }
        }
    }
    return count;
}

std::optional<std::string> validate_mesh(const MeshData& mesh) {
    if (mesh.positions.empty()) {
        return std::string("the mesh has no vertices");
    }
    if (mesh.indices.empty()) {
        return std::string("the mesh has no triangles");
    }
    if (mesh.indices.size() % 3 != 0) {
        return std::string("the index count is not a multiple of three");
    }
    for (const std::uint32_t index : mesh.indices) {
        if (index >= mesh.positions.size()) {
            return std::string("an index names a vertex that does not exist");
        }
    }
    for (const glm::vec3& position : mesh.positions) {
        if (!finite(position)) {
            return std::string("a vertex position is not finite");
        }
    }
    if (!mesh.normals.empty() && mesh.normals.size() != mesh.positions.size()) {
        return std::string("the normal count does not match the vertex count");
    }
    for (const glm::vec3& normal : mesh.normals) {
        if (!finite(normal) || std::abs(glm::length(normal) - 1.0f) > 1e-3f) {
            return std::string("a vertex normal is not a finite unit vector");
        }
    }
    if (!mesh.joints.empty() || !mesh.weights.empty()) {
        if (mesh.joints.size() != mesh.positions.size() || mesh.weights.size() != mesh.positions.size()) {
            return std::string("the joint or weight count does not match the vertex count");
        }
        for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                const float weight = mesh.weights[v][i];
                if (!std::isfinite(weight) || weight < 0.0f) {
                    return std::string("a vertex weight is negative or not finite");
                }
                if (mesh.joints[v][i] >= kMaxSkinJoints) {
                    return std::string("a vertex names a joint beyond the 256 a palette holds");
                }
                sum += weight;
            }
            if (std::abs(sum - 1.0f) > 1e-3f) {
                return std::string("a vertex's weights do not sum to one");
            }
        }
    }
    if (!mesh.uvs.empty() && mesh.uvs.size() != mesh.positions.size()) {
        return std::string("the texture coordinate count does not match the vertex count");
    }
    for (const glm::vec2& uv : mesh.uvs) {
        if (!finite(uv)) {
            return std::string("a texture coordinate is not finite");
        }
    }
    return std::nullopt;
}

std::vector<glm::vec3> vertex_normals(const MeshData& mesh) {
    if (!mesh.normals.empty()) {
        return mesh.normals;
    }
    // The cross product of two edges has the face's normal and twice its area, so summing unnormalized face normals weights by area.
    std::vector<glm::vec3> sums(mesh.positions.size(), glm::vec3(0.0f));
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t a = mesh.indices[t];
        const std::uint32_t b = mesh.indices[t + 1];
        const std::uint32_t c = mesh.indices[t + 2];
        const glm::vec3 face = glm::cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
        sums[a] += face;
        sums[b] += face;
        sums[c] += face;
    }
    for (glm::vec3& sum : sums) {
        const float length = glm::length(sum);
        // A vertex with no area around it keeps the +Z normal, so the result is always unit length.
        sum = length > 0.0f ? sum / length : glm::vec3(0.0f, 0.0f, 1.0f);
    }
    return sums;
}

MeshBounds compute_bounds(const MeshData& mesh) {
    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(-std::numeric_limits<float>::max());
    for (const glm::vec3& position : mesh.positions) {
        low = glm::min(low, position);
        high = glm::max(high, position);
    }
    MeshBounds bounds;
    bounds.center = (low + high) * 0.5f;
    for (const glm::vec3& position : mesh.positions) {
        bounds.radius = std::max(bounds.radius, glm::length(position - bounds.center));
    }
    return bounds;
}

MeshData make_box_mesh(const glm::vec3& h) {
    MeshData mesh;
    // Each face: its center, the two axes spanning it, and its outward normal. Every (u, v, normal) triple is right-handed.
    add_face(mesh, {h.x, 0, 0}, {0, h.y, 0}, {0, 0, h.z}, {1, 0, 0});
    add_face(mesh, {-h.x, 0, 0}, {0, 0, h.z}, {0, h.y, 0}, {-1, 0, 0});
    add_face(mesh, {0, h.y, 0}, {0, 0, h.z}, {h.x, 0, 0}, {0, 1, 0});
    add_face(mesh, {0, -h.y, 0}, {h.x, 0, 0}, {0, 0, h.z}, {0, -1, 0});
    add_face(mesh, {0, 0, h.z}, {h.x, 0, 0}, {0, h.y, 0}, {0, 0, 1});
    add_face(mesh, {0, 0, -h.z}, {0, h.y, 0}, {h.x, 0, 0}, {0, 0, -1});
    return mesh;
}

MeshData make_sphere_mesh(float radius, std::uint32_t rings, std::uint32_t segments) {
    MeshData mesh;
    if (rings < 2 || segments < 3) {
        return mesh;  // validate_mesh rejects the empty result
    }
    const float pi = 3.14159265358979323846f;
    const auto push = [&mesh, radius](const glm::vec3& normal, const glm::vec2& uv) {
        mesh.positions.push_back(normal * radius);
        mesh.normals.push_back(normal);
        mesh.uvs.push_back(uv);
    };
    // One vertex at each pole, then the rings between them, each with segments + 1 vertices so the seam has its own UVs.
    push(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.5f, 0.0f));
    for (std::uint32_t r = 1; r < rings; ++r) {
        const float theta = pi * static_cast<float>(r) / static_cast<float>(rings);  // 0 at the top pole
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float phi = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            const glm::vec3 normal(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            push(normal, glm::vec2(static_cast<float>(s) / static_cast<float>(segments), static_cast<float>(r) / static_cast<float>(rings)));
        }
    }
    const auto bottom = static_cast<std::uint32_t>(mesh.positions.size());
    push(glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.5f, 1.0f));

    // Vertex index of ring r (1 to rings - 1) at segment s.
    const auto ring = [segments](std::uint32_t r, std::uint32_t s) { return 1 + (r - 1) * (segments + 1) + s; };
    // Triangles are counter-clockwise seen from outside: each fan and band keeps the winding of the pattern (upper, lower next, lower).
    for (std::uint32_t s = 0; s < segments; ++s) {
        mesh.indices.insert(mesh.indices.end(), {0, ring(1, s + 1), ring(1, s)});
    }
    for (std::uint32_t r = 1; r + 1 < rings; ++r) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            mesh.indices.insert(mesh.indices.end(), {ring(r, s), ring(r, s + 1), ring(r + 1, s + 1)});
            mesh.indices.insert(mesh.indices.end(), {ring(r, s), ring(r + 1, s + 1), ring(r + 1, s)});
        }
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        mesh.indices.insert(mesh.indices.end(), {ring(rings - 1, s), ring(rings - 1, s + 1), bottom});
    }
    return mesh;
}

MeshData make_plane_mesh(float half) {
    MeshData mesh;
    const glm::vec3 corners[4] = {{-half, -half, 0}, {half, -half, 0}, {half, half, 0}, {-half, half, 0}};
    const glm::vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (std::size_t i = 0; i < 4; ++i) {
        mesh.positions.push_back(corners[i]);
        mesh.normals.push_back({0, 0, 1});
        mesh.uvs.push_back(uvs[i]);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

}  // namespace engine::render
