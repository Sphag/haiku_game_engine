#include "asset_converter/mesh_cooking.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace engine::converter {
namespace {

using core::Result;
using core::failure;
using core::success;

// The largest error a simplification step may introduce, as a fraction of the mesh extent. A level that cannot halve
// its triangles within this bound ends the chain instead of producing a coarser mesh with a large error.
constexpr float kLodTargetError = 0.1f;

template <typename T>
void add_stream(std::vector<meshopt_Stream>& streams, const std::vector<T>& values) {
    if (!values.empty()) {
        streams.push_back(meshopt_Stream{values.data(), sizeof(T), sizeof(T)});
    }
}

// Reorders one attribute array with a vertex remap table. The table maps old vertices to new ones, and unused ones to no
// entry, so the array shrinks to new_count.
template <typename T>
void remap_vertices(std::vector<T>& values, const std::vector<std::uint32_t>& remap, std::size_t new_count) {
    if (values.empty()) {
        return;
    }
    std::vector<T> out(new_count);
    meshopt_remapVertexBuffer(out.data(), values.data(), values.size(), sizeof(T), remap.data());
    values.swap(out);
}

void remap_all(ModelMesh& mesh, const std::vector<std::uint32_t>& remap, std::size_t new_count) {
    remap_vertices(mesh.positions, remap, new_count);
    remap_vertices(mesh.normals, remap, new_count);
    remap_vertices(mesh.tangents, remap, new_count);
    remap_vertices(mesh.uvs, remap, new_count);
}

template <typename Vector>
bool finite_components(const Vector& value) {
    for (glm::length_t i = 0; i < value.length(); ++i) {
        if (!std::isfinite(value[i])) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool all_finite(const std::vector<T>& values) {
    return std::all_of(values.begin(), values.end(), [](const T& value) { return finite_components(value); });
}

MeshBounds compute_bounds(const std::vector<glm::vec3>& positions) {
    MeshBounds bounds;
    if (positions.empty()) {
        return bounds;
    }
    bounds.min = positions.front();
    bounds.max = positions.front();
    for (const glm::vec3& position : positions) {
        bounds.min = glm::min(bounds.min, position);
        bounds.max = glm::max(bounds.max, position);
    }
    bounds.center = (bounds.min + bounds.max) * 0.5f;
    for (const glm::vec3& position : positions) {
        bounds.radius = std::max(bounds.radius, glm::length(position - bounds.center));
    }
    return bounds;
}

// Splits one LOD into meshlets. The builder may reorder triangles and rotate their corners, so the meshlets hold the
// LOD's triangles as a set. Each meshlet's data is packed into the LOD's arrays back to back.
void build_meshlets(MeshLod& lod, const std::vector<glm::vec3>& positions) {
    if (lod.indices.empty()) {
        return;
    }
    const float* position_data = &positions.front().x;
    const std::size_t vertex_count = positions.size();

    std::vector<meshopt_Meshlet> raw(meshopt_buildMeshletsBound(lod.indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles));
    // The worst case for both packed arrays is one entry per index.
    std::vector<std::uint32_t> vertices(lod.indices.size());
    std::vector<std::uint8_t> triangles(lod.indices.size());
    const std::size_t count = meshopt_buildMeshlets(raw.data(), vertices.data(), triangles.data(), lod.indices.data(),
                                                    lod.indices.size(), position_data, vertex_count, sizeof(glm::vec3),
                                                    kMeshletMaxVertices, kMeshletMaxTriangles, 0.0f);
    raw.resize(count);

    lod.meshlets.clear();
    lod.meshlets.reserve(count);
    lod.meshlet_vertices.clear();
    lod.meshlet_triangles.clear();
    for (const meshopt_Meshlet& source : raw) {
        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(&vertices[source.vertex_offset],
                                                                   &triangles[source.triangle_offset], source.triangle_count,
                                                                   position_data, vertex_count, sizeof(glm::vec3));
        Meshlet meshlet;
        meshlet.vertex_offset = static_cast<std::uint32_t>(lod.meshlet_vertices.size());
        meshlet.vertex_count = source.vertex_count;
        meshlet.triangle_offset = static_cast<std::uint32_t>(lod.meshlet_triangles.size());
        meshlet.triangle_count = source.triangle_count;
        meshlet.center = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
        meshlet.radius = bounds.radius;
        meshlet.cone_axis = glm::vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]);
        meshlet.cone_cutoff = bounds.cone_cutoff;
        lod.meshlets.push_back(meshlet);

        const auto vertex_begin = vertices.begin() + static_cast<std::ptrdiff_t>(source.vertex_offset);
        lod.meshlet_vertices.insert(lod.meshlet_vertices.end(), vertex_begin,
                                    vertex_begin + static_cast<std::ptrdiff_t>(source.vertex_count));
        const auto triangle_begin = triangles.begin() + static_cast<std::ptrdiff_t>(source.triangle_offset);
        lod.meshlet_triangles.insert(lod.meshlet_triangles.end(), triangle_begin,
                                     triangle_begin + static_cast<std::ptrdiff_t>(3 * source.triangle_count));
    }
}

}  // namespace

Result<MeshCookReport, std::string> cook_mesh(ModelMesh& mesh, std::vector<std::uint32_t> triangles) {
    const std::size_t source_vertices = mesh.positions.size();
    if (triangles.size() % 3 != 0) {
        return failure(std::string("the index count is not a multiple of 3"));
    }
    if ((!mesh.normals.empty() && mesh.normals.size() != source_vertices) ||
        (!mesh.tangents.empty() && mesh.tangents.size() != source_vertices) ||
        (!mesh.uvs.empty() && mesh.uvs.size() != source_vertices)) {
        return failure(std::string("an attribute array does not have one entry per vertex"));
    }
    for (std::uint32_t index : triangles) {
        if (index >= source_vertices) {
            return failure(std::string("an index is out of range"));
        }
    }
    if (!all_finite(mesh.positions) || !all_finite(mesh.normals) || !all_finite(mesh.tangents) || !all_finite(mesh.uvs)) {
        return failure(std::string("a vertex attribute is not finite"));
    }

    // Step 1. A triangle that repeats an index has two equal corners, so it is a line or a point.
    MeshCookReport report;
    std::vector<std::uint32_t> kept;
    kept.reserve(triangles.size());
    for (std::size_t t = 0; t < triangles.size(); t += 3) {
        const std::uint32_t a = triangles[t];
        const std::uint32_t b = triangles[t + 1];
        const std::uint32_t c = triangles[t + 2];
        if (a == b || b == c || a == c) {
            ++report.degenerate_triangles;
            continue;
        }
        kept.insert(kept.end(), {a, b, c});
    }

    mesh.lods.assign(1, MeshLod{});
    if (kept.empty()) {
        mesh.positions.clear();
        mesh.normals.clear();
        mesh.tangents.clear();
        mesh.uvs.clear();
        mesh.bounds = MeshBounds{};
        return success(report);
    }

    // Step 2. Vertices whose attributes match exactly become one vertex. The vertex cache and fetch orders follow, and
    // the fetch pass drops vertices no triangle uses.
    std::vector<meshopt_Stream> streams;
    add_stream(streams, mesh.positions);
    add_stream(streams, mesh.normals);
    add_stream(streams, mesh.tangents);
    add_stream(streams, mesh.uvs);
    std::vector<std::uint32_t> remap(source_vertices);
    const std::size_t unique = meshopt_generateVertexRemapMulti(remap.data(), kept.data(), kept.size(), source_vertices,
                                                                streams.data(), streams.size());
    std::vector<std::uint32_t> merged(kept.size());
    meshopt_remapIndexBuffer(merged.data(), kept.data(), kept.size(), remap.data());
    remap_all(mesh, remap, unique);

    std::vector<std::uint32_t> cached(merged.size());
    meshopt_optimizeVertexCache(cached.data(), merged.data(), merged.size(), unique);
    std::vector<std::uint32_t> fetch(unique);
    const std::size_t used = meshopt_optimizeVertexFetchRemap(fetch.data(), cached.data(), cached.size(), unique);
    std::vector<std::uint32_t> ordered(cached.size());
    meshopt_remapIndexBuffer(ordered.data(), cached.data(), cached.size(), fetch.data());
    remap_all(mesh, fetch, used);
    mesh.lods[0].indices = std::move(ordered);

    // Step 3. Each level halves the triangles of the one before. The error is relative to the mesh extent, so it is
    // scaled to model units. It is also kept as a running maximum, because a level simplified from a coarser one does not
    // report the error against the original mesh.
    const std::size_t vertex_count = mesh.positions.size();
    const float* position_data = &mesh.positions.front().x;
    const float extent_scale = meshopt_simplifyScale(position_data, vertex_count, sizeof(glm::vec3));
    while (mesh.lods.size() < kMaxLodCount) {
        const MeshLod& finer = mesh.lods.back();
        const std::size_t target = (finer.indices.size() / 3 / 2) * 3;
        if (target == 0) {
            break;
        }
        std::vector<std::uint32_t> simplified(finer.indices.size());
        float relative_error = 0.0f;
        const std::size_t count = meshopt_simplify(simplified.data(), finer.indices.data(), finer.indices.size(), position_data,
                                                   vertex_count, sizeof(glm::vec3), target, kLodTargetError, 0, &relative_error);
        if (count == 0 || count >= finer.indices.size()) {
            break;
        }
        simplified.resize(count);
        MeshLod lod;
        lod.error = std::max(finer.error, relative_error * extent_scale);
        lod.indices = std::move(simplified);
        mesh.lods.push_back(std::move(lod));
    }

    // Step 4 and 5.
    for (MeshLod& lod : mesh.lods) {
        build_meshlets(lod, mesh.positions);
    }
    mesh.bounds = compute_bounds(mesh.positions);
    return success(report);
}

}  // namespace engine::converter
