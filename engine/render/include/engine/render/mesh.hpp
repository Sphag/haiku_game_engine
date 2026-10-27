#pragma once

// Indexed triangle meshes for the scene renderer. Vertices hold a position, a normal, and a texture coordinate (US-034 added the last two). A skinned mesh (US-036) also holds up to four
// joint indices and weights per vertex; those arrays are either empty or match the position count.
// A mesh is valid when every index names a vertex, the indices form whole triangles, and the optional normal and UV arrays are either
// empty or match the position count. Triangles are counter-clockwise as seen from the side they face (front faces), so the normal from
// the winding, (b - a) x (c - a), points out of a closed surface. Back faces are culled unless the material is two-sided.

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {

struct MeshData {
    std::vector<glm::vec3> positions;    // object space, meters
    std::vector<glm::vec3> normals;      // unit length, object space; empty to derive them from the winding (vertex_normals)
    std::vector<glm::vec2> uvs;          // texture coordinates, v = 0 at the top row of an image; empty means all zero
    std::vector<std::uint32_t> indices;  // three per triangle
    std::vector<glm::uvec4> joints;      // skinned meshes: the joint of each influence, below kMaxSkinJoints; empty for a static mesh
    std::vector<glm::vec4> weights;      // skinned meshes: non-negative influences that sum to one; same count as joints
};

// Joint indices are stored in one 32-bit word per vertex, a byte each, so a skeleton has at most this many joints.
inline constexpr std::uint32_t kMaxSkinJoints = 256;

// Joints the palette must hold for a mesh: one more than the highest joint with a positive weight. Zero for a static mesh.
[[nodiscard]] std::uint32_t skin_joint_count(const MeshData& mesh) noexcept;

// Bounding sphere in object space, used for frustum culling.
struct MeshBounds {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// Returns the first problem found, or nothing for a valid mesh. A skinned mesh needs one joint and one weight set per vertex, the weights
// finite, non-negative, and summing to one within 1e-3, and joints below kMaxSkinJoints.
[[nodiscard]] std::optional<std::string> validate_mesh(const MeshData& mesh);

// The mesh's normals, one per vertex. The stored normals when present; otherwise each vertex averages the face normals of its triangles,
// weighted by area. Assumes a valid mesh.
[[nodiscard]] std::vector<glm::vec3> vertex_normals(const MeshData& mesh);

// Sphere centered on the middle of the vertices' bounding box that contains every vertex. Assumes a valid mesh.
[[nodiscard]] MeshBounds compute_bounds(const MeshData& mesh);

// An axis-aligned box centered on the origin with six faces of four vertices each (flat normals), twelve triangles, and a 0-1 UV per face.
[[nodiscard]] MeshData make_box_mesh(const glm::vec3& half_extents);

// A UV sphere centered on the origin. `rings` latitude bands and `segments` longitude slices; the poles are single vertices shared by
// the first and last ring. Normals point away from the center and UVs run from (0, 0) to (1, 1) over the sphere.
[[nodiscard]] MeshData make_sphere_mesh(float radius, std::uint32_t rings, std::uint32_t segments);

// A square in the XY plane centered on the origin, facing +Z, with UVs from (0, 0) at the top left to (1, 1) at the bottom right.
[[nodiscard]] MeshData make_plane_mesh(float half_size);

}  // namespace engine::render
