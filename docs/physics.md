# Physics Adapter

The Jolt adapter (US-052). The contract is in `engine/physics/include/engine/physics/physics_world.hpp`, and the implementation is in `engine/physics/src/physics_world.cpp`. Jolt is vendored in `third_party/jolt` and built by its own `Jolt/Jolt.cmake` from `engine/physics/CMakeLists.txt`. No Jolt type appears in the public header.

## Worlds and steps

- `PhysicsWorld::create` makes one Jolt physics system for one runtime scene. Worlds share no bodies, gravity, or job system, and closing one leaves the others running.
- Simulation runs at a fixed 60 Hz (`kFixedRateHz`, `kFixedStep`). `update(frame_seconds)` accumulates the frame time and runs whole steps.
- The catch-up limit (`max_catch_up_steps`, default 4) bounds the steps one update may run. Time beyond the limit is dropped and counted in `PhysicsStats::dropped_steps`. `catch_up_events` counts the updates that hit the limit, so a slow frame cannot cause a burst of steps.
- `set_paused(true)` runs no steps and discards the frame time that passes while paused. Resuming runs only the time that follows.

## Transforms

- `step_transform` returns a body's pose as of the last step.
- `render_transform` interpolates between the last two steps at `interpolation_alpha()`, a fraction in `[0, 1)`. Position is interpolated linearly and rotation with slerp. Both are kept in double world space.
- Jolt keeps single-precision positions relative to the large-world origin (`docs/large-world.md`). `origin_subscriber()` returns the world's `OriginSubscriber`, which moves every body by each shift. It stays valid after the world closes, and then it does nothing.

## Bodies and events

- A body refers to an `ecs::EntityHandle`. An entity owns at most one body per world, and a stale handle never matches a later entity in the same slot.
- Motion types are `Static`, `Dynamic`, and `Kinematic`. Shapes are boxes and spheres for now; colliders, filters, and queries are US-053.
- `move_kinematic` sets the target of a kinematic body. The body reaches it during the next step.
- Contact added and removed events, and body activation and deactivation events, come from Jolt's worker threads. They are queued under a mutex and returned by `take_events()` on the owning thread, each tagged with its step.

## Joints

- `add_constraint` joins two bodies of the world with a `ConstraintDesc` (US-054). The kinds are `Fixed` (the two anchors coincide and the frames stay still relative to each other), `Hinge` (the anchors coincide and the bodies rotate about a shared axis, optionally limited to `[min_limit, max_limit]` radians), and `Distance` (the anchors stay between `min_distance` and `max_distance` metres apart).
- Anchors and axes are body-local, in metres from each body's centre of mass. For box, sphere, and capsule colliders that centre is the body origin. Each body's axis and reference normal must be non-zero and perpendicular.
- A joint names its bodies. `remove_body` removes every joint that names the body before the body goes, so no joint points at a destroyed body. The other body wakes, because the joint no longer holds it.
- `remove_constraint` removes one joint, and a stale handle matches nothing: handles are never reused. `constraint_count` reports the live joints. `close()` removes them all with the bodies.
- A joint added or removed wakes its dynamic bodies. A body that was asleep while held would otherwise stay asleep after the joint went.

## Queries

- `overlaps(shape)` returns the entities of the bodies the shape overlaps now. Touching does not count, and triggers are returned when the filter includes them.
- A `shape_cast` hit's `normal` is the cast's contact normal, which points from the hit body toward the cast shape. A raycast's `normal` is the surface normal at the hit. At an edge the surface normal picks one face, so a capsule touching the edge of a step would read a wall as a floor.

## Character controller

`CharacterController` (`engine/physics/include/engine/physics/character.hpp`, US-054) is a capsule that is not a body. It moves by shape casts against the world's colliders, so it neither pushes nor is pushed by dynamic bodies. Triggers never block it. It is driven once per frame, after `update`:

- It walks surfaces up to `max_slope_degrees` and blocks steeper ones. A walk over a walkable surface keeps its horizontal speed.
- It climbs ledges up to `step_height`, and a ledge that is higher stops it at its face.
- It jumps at `jump_speed` under its gravity, and it lands on the next walkable surface.
- It rides the platform it stands on. The feet keep their offset from the platform's last pose, including its rotation, and the platform's motion comes from `step_transform`.
- `overlaps(world)` lists the bodies its capsule touches now, triggers included.

The feet rest one skin (2 cm) above a surface. On a slope the rounded base touches the surface off the centre, so the feet stand r(1/cos θ - 1) above the surface beneath them. For a 30-degree slope and a 0.35 m radius that is about 7 cm.

A step is tried half a radius beyond the frame's motion. The rounded base rests on the tread only once its centre is past the riser's edge, which one frame's motion may not reach. Each riser therefore adds up to half a radius of forward motion.

## Closure

`close()` removes and destroys every body, drops queued events, and releases the job system, which joins its worker threads. After closure `update` and `add_body` return `InvalidState`, queries return nothing, and `take_events()` is empty. The destructor closes the world.

## Build notes

- `USE_SSE4_1`/`USE_SSE4_2` are set for Jolt. Jolt's `JPH_USE_*` macros must match in every target that includes its headers, and `Jolt.cmake` exports them to consumers.
- The Jolt target is C++20, because clang 10 (the local Linux compiler) accepts Jolt's `__builtin_bit_cast` only in that mode.
- Precompiled headers are disabled for Jolt on clang older than 11, because the clang 10 precompiled header rejects Jolt's constexpr `BitCast`. Debug builds of Jolt use line tables only (`-g1`) on clang, because clang 10 crashes on full type debug information in `ConvexShape.cpp`.
- Jolt is vendor code and does not inherit the first-party warnings-as-errors policy.

## Benchmark contract

The PRD's `physics_stress` workload (US-077) sets the scale these joints and characters must support: 2 000 active dynamic bodies, 200 static colliders, 100 constraints, 20 characters, 1 000 raycasts/s and 100 shape casts/s, all at the fixed 60 Hz step. The scenario keeps applying velocity kicks to its bodies, so a run must not replace them with sleeping bodies; the report counts active and sleeping bodies so a run that lets them sleep is visible.

The headless profile (`physics_headless`) is implemented: `engine_bench --suite acceptance` runs `physics_stress` on the CPU in Release, with no GPU in the frame. The run is in simulated time, so its schedule is the same on every host. Its action sequence (`physics_action_sequence_v1`, in `benchmarks/manifest.json`) is:

- every dynamic body gets a seeded velocity kick every 30 frames, staggered by index, so bodies are awake and interacting;
- every character walks a circle at 1.5 m/s and jumps on a staggered 3-second period;
- raycasts and shape casts are issued at the table rates from seeded origins over the world centre.

The report counts simulation steps, catch-up events, lost timesteps (steps the catch-up limit dropped), queries and their hits, active and sleeping dynamic bodies, interactions, and contact and activation events, and it times each measured frame's simulation work. A run fails on any lost timestep, step error, non-finite transform, or body or character outside the reference bounds (400 m horizontally from the world centre, -50 m to 400 m in height). The visual profile (`physics_visual`) needs the render path with physics in the frame, which is not implemented, so it reports `missing`. No Release measurement exists yet, so these figures are a contract, not a result.
