# ECS Storage and Queries

In-house entity-component storage in `engine_ecs` (`engine/ecs/`). No external ECS library is used.

## Pieces

- `EntityHandle` (`entity.hpp`): index, generation, and world id. See US-012.
- `World` (`world.hpp`): issues and checks handles and persistent GUIDs.
- `SparseSet<T>` (`sparse_set.hpp`): one component pool. A sparse array indexed by entity slot points into dense arrays of handles and components.
- `Registry` (`registry.hpp`): owns a `World` and one pool per component type. It is the public entry point for components and queries.

## Behavior

- `add<T>(e, args...)` returns a pointer to the new component, or `nullptr` if `e` is not alive in this registry's world or already has a `T`. The existing value is kept.
- `get<T>(e)` returns a pointer or `nullptr`. `has`, `remove`, and `count` follow the same rules.
- `destroy(e)` removes every component of `e` before releasing its slot, so a reused slot never inherits old components.
- A dense entry keeps the full handle. A stale handle that shares a slot with a newer entity is never a member.
- `query<A, B, ...>()` returns the live entities that hold every listed type. It walks the smallest pool and tests the others.
  - An empty type list, a type no entity has ever held, or a type with no current members returns an empty result.
  - Result order is not stable and is not a save-format contract. Compare membership.
- Components need to be move-constructible and move-assignable, not copyable. Pools are move-only. Storage is `std::vector`, so alignment follows `alignof(T)`.

## Command buffers

`CommandBuffer` (`command_buffer.hpp`) records structural changes made while systems read the registry: `create`, `destroy`, `add<T>`, and `remove<T>`. Recording never touches the registry.

- `create()` returns a `PendingEntity`. Later commands in the same buffer can name it. `ApplyReport::created[k]` holds the handle of `PendingEntity{k + 1}`.
- `apply(registry)` verifies the whole buffer first, then applies the accepted commands in recording order. Nothing changes during verification.
- Verification tracks the effects of earlier commands in the buffer. It rejects, and reports by command position:
  - `EntityNotAlive`: a stale or foreign handle, an entity destroyed earlier in the buffer, or a pending entity that is not alive.
  - `ComponentPresent`: an add of a component the entity already has, including one added earlier in the buffer.
  - `ComponentAbsent`: a remove of a component the entity does not have at that point.
- A rejected command is skipped and the rest of the buffer still applies. Apply empties the buffer.
- Move-only components can be recorded. The value moves into the buffer.

## Systems and stages

`Schedule` (`systems.hpp`) runs systems as jobs on `core::JobSystem`.

- A system declares `Access` (`read<Ts...>()`, `write<Ts...>()`) and optional `after` names. A written type is also readable.
- `add()` registers a system. `build()` checks names, rejects unknown or self dependencies and cycles, and packs systems into stages.
- Two systems conflict when one writes a type that the other reads or writes. Conflicting systems never share a stage. When both are ready, the one registered first runs first.
- Systems in one stage run at the same time. Each one records into its own `CommandBuffer`.
- A stage ends at a sync point. If every job succeeded and the frame was not cancelled, the buffers apply in registration order. Otherwise the stage's buffers are discarded and `run()` returns `Cancelled` or `Failed`. Earlier stages keep their applied changes.
- Inside a system, `read`/`write`/`query` are checked against the declared access. A debug build asserts on an undeclared type. A Release build does not check.
- `FrameReport` gives the status, stages applied, commands applied, rejections (with system name), created entities, and the error text of a failed system.

## Tests

`tests/src/ecs_storage_tests.cpp` (`[unit]`) covers add/get/remove, stale handles in reused slots, swap-removal in the middle of a pool, over-aligned components across growth, move-only components, destructor counts, registry destruction, foreign handles, query intersections, empty queries, bulk churn, and registry moves.

`tests/src/ecs_command_buffer_tests.cpp` (`[unit]`) covers deferred application, pending entities, stale and foreign handles, slot reuse, double destroy, duplicate and missing components, pending entities destroyed in the same buffer, move-only values, and clear.

`tests/src/ecs_systems_tests.cpp` (`[unit]`) covers access rules, stage packing and explicit ordering, build errors, no overlap of conflicting writers on a four-worker pool, sync-point visibility, two systems destroying one entity, cancellation before and during a frame, a throwing system, repeated frames, and the debug access check.

## Not yet covered

- Component reflection (metadata for the inspector and serialization) lives in `engine_scene`; see `docs/reflection.md`.
- Performance is not measured. `engine_bench` does not exist yet, so the acceptance benchmark run is pending.
