#pragma once

// Cooked meshes for the mesh shader tests (US-038). A source mesh goes through the converter's cook_mesh (the same step the asset converter
// runs), and its LOD 0 becomes the two inputs the renderer takes: the indexed MeshData and the MeshletData. Both paths read that one result,
// so a test that switches paths has not converted the source twice.

#include "asset_converter/mesh_cooking.hpp"
#include "asset_converter/model_format.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/meshlet.hpp"

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace engine::render::tests {

struct CookedMesh {
    MeshData mesh;        // LOD 0: the cooked vertices and indices
    MeshletData meshlets;  // the cooked clusters of LOD 0
};

// Cooks `source` and splits the result. The source's normals and UVs are kept when present; the cooker merges identical vertices and drops
// degenerate triangles, so the cooked mesh may have fewer vertices than the source.
inline CookedMesh cook_for_render(const MeshData& source) {
    converter::ModelMesh model;
    model.positions = source.positions;
    model.normals = source.normals;
    model.uvs = source.uvs;
    auto cooked = converter::cook_mesh(model, source.indices);
    if (!cooked.ok()) {
        FAIL("cook_mesh failed: " << cooked.error());
    }
    REQUIRE(!model.lods.empty());
    const converter::MeshLod& lod = model.lods.front();

    CookedMesh out;
    out.mesh.positions = model.positions;
    out.mesh.normals = model.normals;
    out.mesh.uvs = model.uvs;
    out.mesh.indices = lod.indices;
    for (const converter::Meshlet& meshlet : lod.meshlets) {
        MeshletCluster cluster;
        cluster.vertex_offset = meshlet.vertex_offset;
        cluster.vertex_count = meshlet.vertex_count;
        cluster.triangle_offset = meshlet.triangle_offset;
        cluster.triangle_count = meshlet.triangle_count;
        cluster.center = meshlet.center;
        cluster.radius = meshlet.radius;
        cluster.cone_axis = meshlet.cone_axis;
        cluster.cone_cutoff = meshlet.cone_cutoff;
        out.meshlets.meshlets.push_back(cluster);
    }
    out.meshlets.vertices = lod.meshlet_vertices;
    out.meshlets.triangles = lod.meshlet_triangles;
    return out;
}

}  // namespace engine::render::tests
