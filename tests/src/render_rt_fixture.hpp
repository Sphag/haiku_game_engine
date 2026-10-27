#pragma once

// The reference scene of the ray-traced effects (US-043), shared by the CPU suite and the GPU suite. The GPU suite builds the bottom-level
// structures from the mesh data here and the top-level structure from the instance records, so both suites trace the same geometry.
//
// The camera sits at (0, 1.5, 6) and looks toward (0, 0.5, -2), with a 60 degree vertical field and a square image. The objects:
//   ground      a slab at y = 0 (roughness 0.1)
//   pillar      a red box at (-6, 2, 3), behind the camera's left side, so it is off screen
//   mirror      a vertical mirror panel at z = -3, facing the camera (metallic, roughness 0.05), which shows the pillar. A horizontal
//               mirror cannot do this: a reflection keeps the horizontal bearing of its ray, so an object outside the view's horizontal
//               field cannot appear in a floor. A vertical mirror changes the bearing, and the image of the pillar lies on screen.
//   occluder    a white box at (occluder_x, 1, 1) that moves between frames, and casts the sun's shadow
//   fence       an alpha-masked quad at z = -3 with a checkerboard of holes (the mask atlas), which casts a shadow with holes
//   rough box   a green box at (0, 0.5, 2) with roughness 0.9, above the reflection cutoff, so it is never reflected
// No two objects overlap in either occluder position. Where two surfaces interpenetrate, a ray's hit can be either surface, and the
// colors then depend on that tie rather than on the scene.
// The sun is above and behind the fence (z < 0). Its shadows fall toward the camera, onto the floor the camera can see. A sun in front
// would put the fence's shadow behind the fence and the mirror, where no pixel shows it. The direction points from the surface to the sun.

#include "engine/render/rt_effects.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace engine::render::tests {

constexpr std::uint32_t kRtSize = 64;

// The mesh indices of the bottom-level structures: the box is opaque, the fence quad is a non-opaque (alpha-mask) structure.
constexpr int kRtBoxMesh = 0;
constexpr int kRtQuadMesh = 1;

// The instance indices in the scene (the TLAS instance_id).
constexpr std::uint32_t kRtGround = 0;
constexpr std::uint32_t kRtPillar = 1;
constexpr std::uint32_t kRtOccluder = 2;
constexpr std::uint32_t kRtFence = 3;
constexpr std::uint32_t kRtRoughBox = 4;
constexpr std::uint32_t kRtMirror = 5;

// The geometry of one mesh, as the bottom-level structure needs it: float3 positions (stride 12) and uint32 indices.
struct RtMeshData {
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
    std::vector<glm::vec2> uvs;  // one per vertex, the triangle table's source
};

inline RtMeshData rt_box_mesh() {
    RtMeshData mesh;
    mesh.positions = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}, {1, 0}, {1, 1}, {0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3,   // back, z = -1
                    4, 6, 5, 4, 7, 6,   // front, z = +1
                    0, 4, 5, 0, 5, 1,   // bottom
                    3, 2, 6, 3, 6, 7,   // top
                    0, 3, 7, 0, 7, 4,   // left
                    1, 5, 6, 1, 6, 2};  // right
    return mesh;
}

inline RtMeshData rt_quad_mesh() {
    RtMeshData mesh;
    mesh.positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    mesh.uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// The triangle records of a mesh, in index order: the instance's triangle table must hold them in this order.
inline std::vector<RtTriangleRecord> rt_triangles_of(const RtMeshData& mesh) {
    std::vector<RtTriangleRecord> triangles;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        RtTriangleRecord triangle;
        for (std::size_t k = 0; k < 3; ++k) {
            triangle.positions[k] = mesh.positions[mesh.indices[i + k]];
            triangle.uvs[k] = mesh.uvs[mesh.indices[i + k]];
        }
        triangles.push_back(triangle);
    }
    return triangles;
}

// The alpha atlas: a 4 x 4 checkerboard, opaque where x + y is even. Row 0 first, as the GPU atlas is uploaded.
inline RtMaskImage rt_checker_mask() {
    RtMaskImage mask;
    mask.width = 4;
    mask.height = 4;
    mask.alpha.assign(16, 0.0f);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            mask.alpha[y * 4 + x] = (x + y) % 2 == 0 ? 1.0f : 0.0f;
        }
    }
    return mask;
}

// The scene's instances and triangles. The occluder's x is the only thing that changes between frames.
struct RtSceneRecords {
    std::vector<RtInstanceRecord> instances;
    std::vector<RtTriangleRecord> triangles;
    std::vector<int> mesh;  // per instance: kRtBoxMesh or kRtQuadMesh, the bottom-level structure it uses
    RtMaskImage mask;
};

// An instance's material and placement. The triangle range is filled in by add() in rt_scene.
inline RtInstanceRecord rt_instance(const glm::vec3& center, const glm::vec3& half, const glm::vec3& albedo, float roughness) {
    RtInstanceRecord record;
    record.transform = glm::translate(glm::mat4(1.0f), center) * glm::scale(glm::mat4(1.0f), half);
    record.albedo = albedo;
    record.roughness = roughness;
    return record;
}

// `fence_masked` = false keeps the fence's structure non-opaque but tests no mask, so the same geometry without holes is the comparison.
inline RtSceneRecords rt_scene(float occluder_x, bool fence_masked = true) {
    const RtMeshData box = rt_box_mesh();
    const RtMeshData quad = rt_quad_mesh();
    const std::vector<RtTriangleRecord> box_triangles = rt_triangles_of(box);
    const std::vector<RtTriangleRecord> quad_triangles = rt_triangles_of(quad);

    RtSceneRecords scene;
    scene.mask = rt_checker_mask();
    // Each instance owns its triangles, so the triangle table repeats a mesh for every instance that uses it.
    const auto add = [&scene](const RtInstanceRecord& instance, int mesh, const std::vector<RtTriangleRecord>& triangles) {
        RtInstanceRecord record = instance;
        record.triangle_base = static_cast<std::uint32_t>(scene.triangles.size());
        record.triangle_count = static_cast<std::uint32_t>(triangles.size());
        scene.instances.push_back(record);
        scene.mesh.push_back(mesh);
        scene.triangles.insert(scene.triangles.end(), triangles.begin(), triangles.end());
    };
    add(rt_instance({0.0f, -0.05f, 0.0f}, {20.0f, 0.05f, 20.0f}, {0.55f, 0.55f, 0.55f}, 0.1f), kRtBoxMesh, box_triangles);
    add(rt_instance({-6.0f, 2.0f, 3.0f}, {0.5f, 2.0f, 0.5f}, {0.9f, 0.1f, 0.1f}, 0.5f), kRtBoxMesh, box_triangles);
    add(rt_instance({occluder_x, 1.0f, 1.0f}, {0.5f, 1.0f, 0.5f}, {0.8f, 0.8f, 0.9f}, 0.5f), kRtBoxMesh, box_triangles);
    RtInstanceRecord fence = rt_instance({1.5f, 1.5f, -3.0f}, {1.5f, 1.5f, 1.0f}, {0.2f, 0.6f, 0.9f}, 0.5f);
    fence.masked = fence_masked;
    fence.mask_cutoff = 0.5f;
    add(fence, kRtQuadMesh, quad_triangles);
    add(rt_instance({0.0f, 0.5f, 2.0f}, {0.5f, 0.5f, 0.5f}, {0.2f, 0.8f, 0.2f}, 0.9f), kRtBoxMesh, box_triangles);
    RtInstanceRecord mirror = rt_instance({-2.25f, 2.0f, -3.0f}, {1.75f, 2.0f, 0.05f}, {0.95f, 0.95f, 0.95f}, 0.05f);
    mirror.metallic = 1.0f;
    add(mirror, kRtBoxMesh, box_triangles);
    return scene;
}

// The view of the reference scene. Shadows and reflections come from the frame's flags, so a test sets them on the returned frame.
inline RtFrame rt_view() {
    RtFrame frame = look_at_frame(glm::vec3(0.0f, 1.5f, 6.0f), glm::vec3(0.0f, 0.5f, -2.0f), 1.0471975511965976f, 1.0f);
    frame.sun_direction = glm::normalize(glm::vec3(0.5f, 0.7f, -0.5f));
    frame.sun_radiance = glm::vec3(1.0f);
    frame.ambient = glm::vec3(0.1f);
    frame.roughness_cutoff = 0.4f;
    frame.reflection_distance = 100.0f;
    frame.shadow_distance = 1000.0f;
    frame.bias = 0.01f;
    return frame;
}

}  // namespace engine::render::tests
