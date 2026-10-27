#pragma once

// The cooked model payload (US-020, geometry from US-023): the imported meshes, the node hierarchy, and the materials of
// one model. The model importer writes it, and the tests and later stages read it back through read_model_payload().
//
// Layout, little-endian throughout (the file is an importer payload; the cooked container wraps it as before):
//   magic "OWMD", u32 format version (2)
//   u32 mesh count, then per mesh:
//     string name, i32 material index (-1 for none), u32 vertex count, u32 flags (bit 0 normals, bit 1 tangents,
//     bit 2 uv0), f32 positions[3] per vertex, then normals[3], tangents[4], and uv0[2] when their flag is set,
//     bounds: f32 min[3], max[3], center[3], radius (the box and the sphere that reaches every vertex),
//     u32 LOD count (1 to 4), then per LOD (LOD 0 is the full detail):
//       f32 error (absolute, in model units; 0 for LOD 0; non-decreasing with the LOD number),
//       u32 index count, u32 indices (a multiple of 3, each below the vertex count),
//       u32 meshlet count, then per meshlet: u32 vertex offset, u32 vertex count (1 to 64), u32 triangle offset (in
//       bytes), u32 triangle count (1 to 124), f32 center[3], f32 radius, f32 cone axis[3], f32 cone cutoff,
//       u32 meshlet vertex count, u32 meshlet_vertices (mesh vertex indices, grouped per meshlet),
//       u32 meshlet triangle byte count, u8 meshlet_triangles (meshlet-local corners, 3 per triangle)
//   u32 material count, then per material: string name, f32 base_color[4], f32 metallic, f32 roughness,
//     f32 emissive[3], u32 alpha mode (0 opaque, 1 mask, 2 blend), f32 alpha cutoff, u8 double sided,
//     string base_color_texture, string normal_texture, string metallic_roughness_texture, string emissive_texture
//   u32 node count, then per node: string name, i32 parent (-1 for a root), f32 local[16] (column-major, so
//     world = parent_world * local), u32 mesh count, u32 mesh indices
//
// A string is a u32 byte count followed by UTF-8 bytes. A texture string is a source-relative path, or empty when the
// material has no such texture. Nodes are ordered parents first. Reading checks every count against the remaining bytes,
// so a corrupt file cannot make the reader allocate more than it contains.

#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::converter {

inline constexpr std::string_view kModelPayloadMagic = "OWMD";
inline constexpr std::uint32_t kModelPayloadVersion = 2;

// Meshlet limits (US-023). A meshlet holds at most 64 vertices and 124 triangles, the limits the GPU mesh path uses.
inline constexpr std::uint32_t kMeshletMaxVertices = 64;
inline constexpr std::uint32_t kMeshletMaxTriangles = 124;
// LOD 0 plus at most three simplified levels, each about half the triangles of the one before.
inline constexpr std::size_t kMaxLodCount = 4;

// The box of a mesh's vertices and a sphere around it. Empty for a mesh with no vertices (all zero).
struct MeshBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 center{0.0f};  // the middle of the box; the sphere is centered here
    float radius = 0.0f;     // reaches every vertex of the mesh
};

// One cluster of triangles from a LOD. Its vertex and triangle ranges point into the LOD's packed arrays.
struct Meshlet {
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_offset = 0;  // in bytes into meshlet_triangles
    std::uint32_t triangle_count = 0;
    glm::vec3 center{0.0f};  // bounding sphere of the meshlet's vertices
    float radius = 0.0f;
    glm::vec3 cone_axis{0.0f, 0.0f, 1.0f};  // normal cone: unit axis of the triangle normals
    float cone_cutoff = 1.0f;               // cosine of the cone's half angle; 1 when the normals spread over a hemisphere
};

// One level of detail. Every LOD indexes the mesh's vertex arrays, so unused vertices can remain in a coarse LOD.
struct MeshLod {
    float error = 0.0f;  // absolute simplification error in model units; 0 for LOD 0
    std::vector<std::uint32_t> indices;
    std::vector<Meshlet> meshlets;  // packed back to back; their triangles are exactly the LOD's, in any order
    std::vector<std::uint32_t> meshlet_vertices;
    std::vector<std::uint8_t> meshlet_triangles;
};

// Attribute arrays that are empty were absent in the source. A present array has one entry per vertex.
struct ModelMesh {
    std::string name;
    std::int32_t material = -1;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;  // xyz tangent, w handedness: bitangent = w * cross(normal, tangent)
    std::vector<glm::vec2> uvs;       // set 0; v = 0 is the top row of the texture image
    MeshBounds bounds;
    std::vector<MeshLod> lods;  // 1 to kMaxLodCount entries; lods[0] uses every vertex
};

enum class AlphaMode : std::uint32_t { Opaque = 0, Mask = 1, Blend = 2 };

struct ModelMaterial {
    std::string name;
    glm::vec4 base_color{1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    glm::vec3 emissive{0.0f};
    AlphaMode alpha_mode = AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    bool double_sided = false;
    std::string base_color_texture;
    std::string normal_texture;
    std::string metallic_roughness_texture;
    std::string emissive_texture;
};

struct ModelNode {
    std::string name;
    std::int32_t parent = -1;
    glm::mat4 local{1.0f};
    std::vector<std::uint32_t> meshes;
};

struct ModelPayload {
    std::vector<ModelMesh> meshes;
    std::vector<ModelNode> nodes;
    std::vector<ModelMaterial> materials;
};

// Writes the payload. The model must already be valid (see read_model_payload for the rules); the writer does not repeat
// the checks, so the same model always produces the same bytes.
[[nodiscard]] std::string write_model_payload(const ModelPayload& model);

// The rules a payload must meet: attribute counts match the vertex count, indices are in range, parents come before
// their children, material and mesh references exist, numbers are finite, and every string is UTF-8. The importer checks
// its output with this before writing it, and the reader checks every file with it.
[[nodiscard]] core::Result<core::Unit, std::string> validate_model(const ModelPayload& model);

// Reads a payload. Counts are checked against the bytes that remain before anything is allocated.
[[nodiscard]] core::Result<ModelPayload, std::string> read_model_payload(std::string_view bytes);

}  // namespace engine::converter
