# Mesh cooking (US-023)

The model importer cooks every mesh after it builds the mesh's attributes and tangents. The result is indexed geometry, a LOD chain, bounds, and meshlets for the GPU paths (FR-06). The cooked data is part of the `OWMD` model payload (`docs/model-import.md`), and the layout is described at the top of `tools/asset_converter/include/asset_converter/model_format.hpp`.

meshoptimizer does the work. It is vendored in `third_party/meshoptimizer` and built as `engine_meshopt_vendor`, which only the converter links. The runtime reads the cooked result and never links the library (FR-03).

## What cooking produces

`cook_mesh()` (`mesh_cooking.hpp`, `mesh_cooking.cpp`) runs these steps:

1. **Drop degenerate triangles.** A triangle that repeats a vertex index has no area and no meaningful normal. Each one is dropped and counted; the importer reports the count as a note.
2. **Merge and reorder vertices.** Vertices whose attributes are identical become one vertex. The vertex cache and vertex fetch orders follow, and vertices that no triangle uses are removed.
3. **Build the LOD chain.** Each level halves the triangles of the level before, up to four levels. Simplification uses a target error of 0.1 of the mesh extent. If a level cannot shrink, the chain ends, so tiny meshes have one or two levels.
4. **Split into meshlets.** Each LOD is split into meshlets of at most 64 vertices and 124 triangles. Each meshlet has a bounding sphere and a normal cone.
5. **Compute bounds.** The box is the exact box of the vertices. The sphere is centered on the box and reaches every vertex.

Every LOD indexes the mesh's one vertex array. LOD 0 uses every vertex; a coarser LOD may leave some unused.

## Error

Each LOD stores an absolute error in model units. meshoptimizer reports its error relative to the mesh extent, so the error is scaled by `meshopt_simplifyScale`. It is also kept as a running maximum, since a level simplified from a coarser one does not report its error against the original mesh. LOD 0 has error 0, and the error never decreases from one level to the next.

## Small and degenerate meshes

- A mesh with no triangles, or none left after step 1, cooks to empty LODs and no vertices. The importer reports it and skips it.
- A single triangle is one LOD with one meshlet.
- A mesh with vertices no triangle uses cooks to the used vertices only.
- A payload with a partial triangle, an index out of range, an attribute array of the wrong length, or a non-finite attribute is rejected before anything changes.

## What the payload checks

`validate_model` checks cooked data on every write and read:

- Each LOD has a whole number of triangles, all indices in range, a non-negative error, and a triangle count no larger than the LOD before it.
- Meshlets are packed back to back, with 1 to 64 vertices and 1 to 124 triangles each. Their vertices are in range, and their local corners refer inside them.
- The meshlets hold exactly the LOD's triangles. The builder may reorder triangles and rotate their corners, so the check compares sets of rotated triangles.
- The bounds are the exact box, and the sphere reaches every vertex. Each meshlet's sphere reaches its own vertices. Cone axes are at most unit length, and cone cutoffs lie in [-1, 1].
- Every vertex is used by LOD 0.

## Not yet covered

- The mesh shader path reads LOD 0's meshlets, with their cones and spheres, from a `MeshletData` built from the cooked mesh (US-038, `docs/mesh-shaders.md`). LOD selection and the rest of the LODs are not read yet.
- The cooker does not call `meshopt_optimizeMeshlet` on the output. That pass can improve locality, and it can be added without changing the payload.
- No meshlet cone apex is stored. The cone axis and cutoff are enough for backface culling.
- The `engine_bench --suite acceptance` run is not available, because the benchmark harness does not exist yet. Cooking runs offline and is not on a measured runtime path.

## Tests

`tests/src/converter_mesh_tests.cpp` covers each rule above with meshes built in code, so the expected values are known. The model import tests in `converter_model_tests.cpp` cover the same path through the fixtures, and find vertices by position because cooking reorders them.
