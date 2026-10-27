# Scene Graph and Transforms

The object hierarchy of `engine_scene` (`engine/scene/`): `SceneGraph` (`scene_graph.hpp`) and `Transform` (`transform.hpp`). The inspector (US-062), the gizmos (US-063), and the scene format (US-017) build on it. Nodes are ECS entities, so the graph uses `engine_ecs` for storage.

## Transforms

`Transform` is the authored local value of a node, relative to its parent (or to the world for a root):

- `position` (`dvec3`), `rotation` (`dquat`, unit length), `scale` (`dvec3`). A zero or negative scale is valid.
- `matrix()` is `T * R * S`: scale first, then rotation, then translation. This matches the conventions in `docs/runtime-conventions.md`.
- Components are `double`. A position 5 000 km from the origin still resolves about a nanometre. Rendering converts to camera-relative floats (US-050).

A world matrix is the product of the parent's world matrix and the node's local matrix, so it is a full affine matrix. TRS cannot hold it in general: a rotated child under a non-uniformly scaled parent has shear. `decompose_exact` recovers a `Transform` only when the matrix really is TRS. It returns `nullopt` for shear, a zero scale axis, a projective row, or a non-finite value. A negative determinant is kept as a negative x scale.

## Local and world transforms

- `create(initial, parent)` makes a node. An invalid parent makes a root.
- `set_local(node, value)` replaces the local transform.
- `world_matrix(node)` returns the cached world matrix. It recomputes the node and any dirty ancestors on demand, and returns `nullptr` for a handle that is not alive.
- `update()` recomputes every dirty node and returns how many it recomputed.

## Dirty propagation

Changing a local transform marks the node and its whole subtree dirty. Each node caches its world matrix and a dirty flag. Marking stops at a node that is already dirty, because the invariant guarantees that its subtree is dirty too:

> If a node is dirty, all of its descendants are dirty.

The invariant also gives the reverse: a clean node has only clean ancestors. The refresh walk up from a node therefore stops at the first clean node. A refresh runs from the topmost dirty node down, so every parent is clean before its children read it.

## Reparenting

`reparent(node, new_parent, mode)` takes one of two modes:

- `KeepWorldTransform`: the local transform is recomputed as `inverse(new_parent_world) * world`, so the node does not move in the world.
- `KeepLocalTransform`: the local transform is unchanged, so the node moves with its new parent.

The world-preserving mode can fail, and it then changes nothing:

| Error | Cause |
| --- | --- |
| `SingularParent` | The new parent's world matrix has no inverse, for example a zero scale. |
| `NotRepresentable` | The result would need shear or a zero scale axis (see Transforms). Use `KeepLocalTransform`. |

## Cycles and handles

A node cannot become its own ancestor. `reparent` returns `Cycle` when the new parent is the node or one of its descendants. Every operation checks its handles against the graph's world. A stale handle, or a handle from another world, returns `NotAlive`, even when its slot has been reused. Invalid transforms (non-finite values or a non-unit rotation) return `InvalidTransform`.

## Deleting a parent

`destroy(node, ChildPolicy)` names what happens to the children. There is no default:

- `DestroySubtree`: the children and all their descendants are destroyed.
- `ReparentToParent`: the children move to the deleted node's parent, or become roots, and keep their world transforms. If any child cannot keep its world transform (the `NotRepresentable` or `SingularParent` cases above), the whole deletion fails and the graph does not change.

Destroying a node removes its ECS components. A reused slot never inherits the old node's children or transform.

## Limits

- Not thread-safe. The owning thread makes structural changes at a synchronization point (US-014).
- `update()` scans every node. It is correct, but a benchmark has not measured it yet.
- Scene nodes do not carry persistent GUIDs yet. `ecs::World` supports them; the scene format that uses them is US-017.
- No benchmark has measured this module. It is not on a measured path yet, so `engine_bench` was not run for this change.

## Tests

- `tests/src/scene_transform_tests.cpp`: composition order, non-uniform scale, decomposition, mirrors, shear and degenerate rejection, and validity.
- `tests/src/scene_graph_tests.cpp`: local and world composition, scaling through a parent, orbits, dirty propagation (including subtree scope and update counts), keep-world and keep-local reparenting, cycle rejection, singular and shear refusals, stale handles and slot reuse, both delete policies, a 10 000-level chain, and millimetre offsets at 5 000 km from the origin.
