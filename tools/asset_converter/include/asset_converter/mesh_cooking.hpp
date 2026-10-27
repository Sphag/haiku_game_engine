#pragma once

// Mesh cooking (US-023): turns one imported mesh into the geometry the GPU paths read. meshoptimizer does the work here,
// in the converter only; the runtime reads the cooked result through the model payload and never links the library.
//
// cook_mesh() produces, in this order:
//   1. Drops triangles that repeat a vertex index. They cover no area and have no meaningful normal.
//   2. Merges vertices whose attributes are identical, then reorders vertices and triangles for the vertex cache and
//      the vertex fetch. Vertices no triangle uses are removed.
//   3. Builds a LOD chain by simplifying to about half the triangles per level, up to kMaxLodCount levels. Simplification
//      stops early when it cannot reduce the mesh, so tiny meshes have one or two levels.
//   4. Splits each LOD into meshlets of at most 64 vertices and 124 triangles, with a bounding sphere and normal cone.
//   5. Computes the mesh bounds from the vertices that remain.
//
// The vertex arrays are shared by every LOD. A mesh with no triangles left after step 1 is returned with empty LODs; the
// importer reports and skips it.

#include "asset_converter/model_format.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::converter {

// What cooking did that the converter reports.
struct MeshCookReport {
    std::size_t degenerate_triangles = 0;  // dropped in step 1
};

// Cooks `mesh` in place. `triangles` are the source triangles, 3 indices per triangle, into the mesh's attribute arrays,
// which hold one entry per source vertex (or none, when the attribute is absent). On success the attributes hold the
// cooked vertices, and `bounds` and `lods` are set. On failure the mesh is unchanged. Failure means the input breaks the
// basic rules: a partial triangle, an index out of range, or an attribute array of the wrong length.
[[nodiscard]] core::Result<MeshCookReport, std::string> cook_mesh(ModelMesh& mesh, std::vector<std::uint32_t> triangles);

}  // namespace engine::converter
