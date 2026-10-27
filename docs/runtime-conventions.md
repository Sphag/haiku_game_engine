# Runtime Conventions

These conventions apply to every engine subsystem on both Windows and Linux. They are defined in
`engine/core/include/engine/core/units.hpp`, `coordinates.hpp`, and `performance_contract.hpp`, and tested in
`tests/src/core_units_tests.cpp` and `tests/src/core_coordinates_tests.cpp`. The runtime does not depend on the
editor; these rules hold without it.

## Units

| Quantity | Unit | C++ type |
|---|---|---|
| Length | meters | `double` in APIs, `float` in rendering math |
| Time | seconds | `units::Seconds` (`std::chrono::duration<double>`) |
| Frame time (budgets, reports) | milliseconds | `units::Milliseconds` |

Conversion helpers (`centimeters_to_meters`, `inches_to_meters`, `feet_to_meters`) convert external values into
meters at the boundary. Subsystems do not store values in other length units.

## Axes and handedness

- World space is **right-handed and Y-up**, measured in meters.
- `+X` is right, `+Y` is up, `+Z` points back toward the viewer. The camera looks down `-Z`
  (`coords::kForward == (0, 0, -1)`).
- Positive rotation follows the right-hand rule.

## Matrices and quaternions

- Matrices are **column-major** (`m[column][row]`) and act on **column vectors**: `v' = M * v`.
- A child-to-world transform is `parent * local`, so the local transform is applied first.
- Quaternions are unit length. `q * v` rotates `v` by `q`.

## Clip space

- Engine clip space: NDC `x` points right, `y` points up, and depth is in **[0, 1]** with near at 0 and far at 1.
  D3D12 uses this directly.
- `coords::perspective` and `coords::orthographic` produce engine clip space (right-handed).
- Vulkan NDC has `y` pointing down. A Vulkan consumer passes an engine projection through
  `coords::to_vulkan_clip()`, which flips `y` (`diag(1, -1, 1, 1) * M`) and leaves `x` and depth unchanged.
- `coords::to_ndc()` performs the perspective divide.

## Paths

- Paths are UTF-8 and normalized lexically to forward slashes (see `engine/core/path.hpp`). Normalization never
  touches the file system and does not apply Unicode normalization. Cyrillic text is preserved byte for byte.
- On Windows, drive-rooted or UNC absolute paths of 240 characters or more use the `\\?\` (or `\\?\UNC\`)
  extended-length form when handed to the file APIs, so they stay clear of the `MAX_PATH` (260) limit.

## Time and identifiers

- `MonotonicClock` is the only clock used for elapsed time. Its readings are meaningful only as differences.
- `Guid` is a 128-bit RFC 4122 version 4 identifier. Persistent objects store its lowercase canonical text form.

## Jobs and shutdown

`engine/core/include/engine/core/jobs.hpp` is the in-house job system (US-006).

- Tasks run on a fixed worker pool. A task that throws ends `Failed` with its message. A dependency that
  failed or was cancelled cancels its dependents, and they never run.
- Cancelling a pending job takes effect at once. Cancelling a running job only sets the `CancellationToken`;
  the task must check it. A running job that ends Cancelled counts as cancelled even if it returns normally.
- A task must not wait on a job or group of its own `JobSystem`, and must not call `shutdown` or `wait_idle`.
  Those calls assert, because the worker they would need may be the one blocked.
- Shut the `JobSystem` down before destroying any subsystem a task uses. `Drain` runs the submitted work first;
  `CancelPending` cancels queued work and waits for running tasks. Submissions after shutdown begins come back
  Cancelled. Handles stay valid after the system is gone, and every job is terminal by then.
- A terminal job drops its task (and the task's captures) at once, so a handle does not keep subsystem
  state alive. `shutdown` may be called from several threads; one caller joins the workers and the others return after it.

## Performance contract

`engine/core/include/engine/core/performance_contract.hpp` holds the fixed PRD budgets: p99 frame time at most
`1000/60` ms in every repeat, at most 0.1% of frames above `1000/30` ms, and no frame above 100 ms after warmup.
Percentiles use the nearest-rank rule `rank = ceil(p * N)`. Reported values are rounded only for display;
comparisons use the unrounded values.

Benchmark code uses these constants directly and does not tune them per scene or per machine.
