# Large-World Coordinates

The precision model for worlds far from the origin (US-050). The code is in `engine/world/include/engine/world/origin.hpp`, and the camera-relative render path is in `engine/render/` (`frame_snapshot.hpp`).

## Precision by layer

| Layer | Type | Why |
| --- | --- | --- |
| Authored world transforms | `double` (`Transform`, `docs/scene-graph.md`) | A position 5 000 km out still resolves about a nanometre. |
| Cell and tile placement, streaming, terrain LOD | `glm::dvec2` camera positions (`cell_coord_at`, `StreamingManager::update`, `select_terrain_lods`) | A float camera at 1e6 m has a spacing of 6 cm, so a boundary could be decided in the wrong cell. |
| Render frame | camera-relative floats (`extract_frame`) | The camera's world position is subtracted in `double` before the conversion to float, so the GPU sees small numbers. |
| Single-precision state kept across frames | `float(world - origin)` (`WorldOrigin::to_local`) | Physics bodies and similar caches. The origin moves so that these values stay near zero. |

## Origin rebasing

`WorldOrigin` holds the origin in `double`. `update(camera)` runs between frames. When the camera is more than the rebase distance (2048 m by default) from the origin, the origin moves to the camera, and every subscriber gets an `OriginShift { from, to }` before `update` returns. Subscribers run in the order they subscribed.

- `OriginShift::delta()` is `to - from`. A single-precision local position written against `from` moves by `-delta` to be written against `to` (`rebase_local`).
- A matrix that maps `from`-local points to clip space becomes `M * translate(delta)` for `to`-local points (`rebase_matrix`).
- Inside 2048 m, a float has a spacing of 0.24 mm. That bounds the error of every local position.

### What a rebase does not change

- Authored world positions, scene graph transforms, entity handles, and pick ids are untouched. Rebasing never rewrites the scene.
- Cell coordinates come from the exact world position, so a rebase does not move a cell boundary.
- The renderer needs no rebase. Its frame snapshot is camera-relative in `double`, and its previous-frame matrices are relative to the previous camera (`MotionHistory`), not to the origin. Motion vectors and temporal history are therefore unaffected by where the origin is.

### Subscriber contract (physics and scripts)

A subscriber is the place where origin-relative float data lives. On `on_origin_shifted`, it:

1. Moves each of its single-precision positions with `shift.rebase_local` (or by `-shift.delta()`).
2. Keeps its entity handles. A rebase never invalidates a handle.
3. Does not call back into the `WorldOrigin` from the callback.

Physics is not wired yet. The adapter that moves bodies is US-052, and its queries are verified in US-053. US-050 proves the event contract with a test subscriber (`BodyAdapter` in `tests/src/world_origin_tests.cpp`).

## Tests

`tests/src/world_origin_tests.cpp` (CPU, `[unit]`):

- The origin holds inside the rebase distance and moves to the camera beyond it.
- Each rebase reaches each subscriber once, in order, with the exact delta.
- A local position 0.3 m from a camera at 1e6 m is exact to float precision, where a plain float difference is 0.0125 m off.
- `rebase_local` and `rebase_matrix` agree with the world.
- A physics-style adapter stays within 1 mm of its authored world positions across several rebases.
- A camera 0.1 mm short of a cell boundary at 1e6 m selects the cell before it.
- A 20 km route at 0.5 m per step, with about ten rebases: node world positions stay exact, the camera's offset stays bounded, the picked entity is the expected handle, and the cells match the exact positions.

## Not covered

- The 200 000-instance streaming route and teleport case (`streaming_stress`) are measured by the benchmark (US-078, `engine_bench --suite streaming_soak`), not by the test suite.
- Physics queries before and after rebasing are in US-053.
