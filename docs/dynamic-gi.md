# Dynamic ray-traced GI (US-044)

The full profile's diffuse bounce comes from a probe grid that the GPU traces every frame. The native profile keeps the baked probes of
US-040 and does not use this path.

## Pieces

| Piece | Where | Role |
|---|---|---|
| `gi::DynamicGrid` | `engine/gi/include/engine/gi/dynamic_grid.hpp` | CPU state: camera-following cells, sample counts, update plan, lighting, resets, and the volume the sampler reads |
| `render::DynamicGi` | `engine/render/include/engine/render/dynamic_gi.hpp` | The compute pass: owns the grid, the tables, and the history buffer |
| `dynamic_gi.hlsl` | `engine/render/shaders/dynamic_gi.hlsl` | `CSDynamicGi`: one thread per planned probe, inline ray queries against the RT scene |
| `gpu_upload` | `engine/render/src/gpu_upload.{hpp,cpp}` | Copies per-frame tables through the render graph (shared with `RtEffects`) |
| `SceneRenderer` | `engine/render/src/scene_renderer.cpp` | With `RayTracedSettings::dynamic_gi`, owns the pass, updates it each frame, records it in the ray-traced graph pass, and binds its buffers |
| `scene.hlsl` | `engine/render/shaders/scene.hlsl` | `DynamicProbeDiffuse` samples the history in slot order with the sampling block; it replaces the baked grid when the block is enabled |

## Per frame

1. `grid().follow(eye)` moves the grid to the camera, in whole cells. `grid().set_lighting(...)` records the sun and the sky.
2. `update_frame(camera_world)` plans the budget (`grid().plan_updates()`) and uploads the parameters, the plan, and the sampling block.
3. The caller builds the top-level structure, as for `RtEffects`, and calls `record()`. The pass dispatches `ceil(budget / 64)` groups,
   and `record()` commits the plan to the grid.

The traced scene is camera-relative (the renderer places the camera at the origin of the top-level structure), so a probe at world
`(cell + 0.5) * spacing` is traced from that point minus `camera_world`. The parameters carry `camera_world` at byte 64, and the scene shader
adds the camera's world position back when it samples.

## Sampling in the frame

The sampling block (`sampling()`) has a 48-byte header and then one valid flag per slot. The header holds the window's origin cell, the
spacing, the dims, an enabled flag, and the residue of the origin modulo the dims. A window cell's slot is `(residue + local) mod dims`,
so the shader needs no table. A slot is valid when it has samples, so a probe is read only after a trace has been recorded for it.

`SceneRenderer` binds the history and the sampling block at environment bindings 8 and 9. When `RayTracedSettings::dynamic_gi` is off,
both bindings hold zeroed dummies with enabled = 0, and the scene reads the baked grid as before. With it on, `DynamicProbeDiffuse`
blends the valid probes trilinearly; a point with no valid probe gets the environment's diffuse alone.

The sky a probe ray sees is the uniform `RayTracedSettings::dynamic_sky_radiance`. The bounce does not use the environment map.

## The trace

A planned probe sits at the centre of its cell, `(cell + 0.5) * spacing`. It casts `rays_per_probe` directions on a spherical Fibonacci set,
so every ray carries the same solid angle.

- A ray that misses the scene brings back the sky radiance.
- A ray that hits a surface brings back `albedo * (sky + sun * max(dot(n, sun), 0) * lit)`, where `lit` is zero when a shadow ray from the
  hit toward the sun is blocked within `shadow_distance`.

The radiance is projected onto nine SH coefficients with the band weights of `gi/probes.hpp`, so `evaluate_irradiance` reads the history
in the same convention as the baked volume.

## Temporal accumulation

A probe's blend weight is `1 / (1 + samples)`, where the samples are the traces it already holds, capped at `max_history - 1`. The history
is therefore a running average until the floor `1 / max_history`, and the first trace replaces whatever the buffer held.

- A camera move that changes a probe's cell resets that probe (the cell is the key, not the slot).
- A lighting change cuts each probe to `lighting_history` samples, so the bounce follows the light within a few frames.
- A teleport resets every probe (`reset()`).
- A streaming change resets the probes inside a world region (`reset_region()`).
- A plan that is committed after its probe has moved does not count that trace.

## Fixed contract

The full profile (PRD, Image Profiles) uses `full_profile_grid()`: a 32 x 8 x 32 grid of 4 m probes, 64 rays per probe, and a budget of
512 probes per frame, one sixteenth of the grid. The history is 8192 slots of 144 bytes.

## Limits

- Every triangle is opaque to these rays (`RAY_FLAG_FORCE_OPAQUE`). An alpha-masked surface blocks light as if it were solid; the RT
  effects of US-043 test the masks, this pass does not.
- A probe placed inside solid geometry is not detected. Its trace starts inside the surface, and the result is not meaningful.
- One bounce only: the hit is shaded with the sky and the sun, not with the grid itself.
- The sky is uniform. The sun has no disc or lobe in this pass.
- A probe becomes valid one frame after its first trace. The sampling block is built before the frame's commit.
- Each frame uploads through the render graph, which waits. The pass uploads its parameters, plan, and sampling block, and
  `set_scene` uploads the instance and triangle tables, so a frame waits on the GPU several times. Batching these is future work.
- The frame's direct light and the grid are not separated in the test: `render_rt_scene_gpu_tests.cpp` checks the total light of
  the frame with the sun on and off, not the value of one probe.

## Tests

- `gi_dynamic_grid_tests.cpp` (`[unit]`): description limits, camera-follow cells, teleport and region resets, round-robin budget, blend
  weights, lighting change, and the volume layout.
- `render_dynamic_gi_tests.cpp` (`[unit]`): the full-profile numbers and the parameter and update byte layouts.
- `render_dynamic_gi_gpu_tests.cpp` (`[gpu]`, DX12 and Vulkan): only planned probes change; a moving sun changes the bounce, stays finite,
  and converges to a fresh trace; a reset discards the old light; the API refuses records before a scene and a plan.
- `render_rt_scene_gpu_tests.cpp` (`[gpu]`, DX12 and Vulkan): the full profile's scene with the grid on. After 16 fill frames the sun goes
  off, and the frame's total light drops by more than 20%, with the image still lit by the environment.
- `render_dynamic_gi_tests.cpp` (`[unit]`): the camera offset in the parameter block, and the sampling block's window, residue (for a
  negative origin), and valid flags.
