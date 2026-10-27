# Native-profile GI probes and screen-space reflections (US-040)

The native profile (`native_1080`) gets indirect diffuse light from baked probes, not from ray tracing, and reflections from SSR with IBL as
the fallback. `engine/gi/` is the CPU side: the bake, the volume format, the sampling, and the SSR reference.
`tools/asset_converter/src/gi_importer.cpp` is the converter importer that runs the bake and writes the payload. The GPU side is in the scene
renderer: the probe grid is bound at environment binding 7 (`GiData` in `shaders/scene.hlsl`), the pixel shader blends the probes into the
diffuse term, `ProbeCoverage` is a debug view, and the screen-space reflection pass is `shaders/reflections.hlsl` (`CSReflections`). See
[Scope and what is not done](#scope-and-what-is-not-done).

## Source and payload

The `gi_probes` importer takes a JSON scene description. Every key except `grid` and `occluders` is optional.

```json
{
  "grid": {"origin": [1, 1, 1], "spacing": 2, "dims": [4, 1, 4]},
  "sky": {"zenith": [0.35, 0.55, 0.95], "horizon": [0.75, 0.8, 0.85], "ground": [0.12, 0.1, 0.08],
          "sun_direction": [0, 1, 0], "sun_radiance": [0, 0, 0]},
  "occluders": [{"min": [0, 0, 0], "max": [1, 1, 1], "albedo": [0.6, 0.6, 0.6], "emitted": [0, 0, 0]}],
  "samples": 256,
  "max_ray_distance": 1000
}
```

The importer's settings take no keys, so `"{}"` is the only valid settings text. The payload is the serialized volume. Its cache key is the
source bytes plus the importer version (`kGiImporterVersion`), so the converter rebakes when the source or the bake math changes.

## Bake

Each probe casts `samples` directions on a Fibonacci sphere with +Y as the pole. A direction that finds nothing within
`max_ray_distance` takes the sky radiance. A direction that hits an occluder takes that occluder's `emitted` color plus its `albedo` times the
sky irradiance at the hit face, but only when a ray along the face normal reaches the sky. That is one bounce. Multiple bounces, sky
visibility from the probe's own point (as opposed to the hit face), and occluders other than axis-aligned boxes are not modelled.

The result is projected onto nine SH coefficients in the same convention as `Environment::diffuse_sh` (irradiance divided by pi). A test
checks that a uniform sky through `build_environment()` and through the probe projection agree, so the two diffuse paths cannot drift.

A probe inside an occluder or on its surface is `Blocked`. Other probes are `Valid` once baked, and `Unbaked` before that. `bake_probes()`
takes a budget, so a bake can be spread over several calls.

## Format and invalidation

The volume is little-endian binary: the magic `OWGI`, the format version (`kFormatVersion`, 1), the hash of the bake inputs, the grid, and
one state byte plus nine RGB floats per probe. `deserialize()` rejects another version, a probe count that does not match the grid, an
unknown state, non-finite values, and trailing bytes, each with a message.

`hash_inputs()` is FNV-1a over every bake input and the format version. `is_current()` compares the stored hash with the current inputs.
`bake_probes()` refuses to bake into a volume that is not current, so stale probes are never mixed with new math.

## Runtime sampling and fallbacks

`sample_irradiance()` does trilinear interpolation of the eight probes around a point. Blocked and unbaked probes take no weight, and the
remaining weight is renormalized. `coverage` is the share of the trilinear weight that landed on valid probes. The result is
`coverage * probe_irradiance + (1 - coverage) * environment_irradiance`, where the environment term is the IBL diffuse for the same normal.
Three cases follow from that:

- No volume, a point outside the grid, or no valid probe around the point: coverage 0, and the result is the environment alone.
- Some valid probes: the result blends the probes with the environment in proportion to coverage.
- All valid: coverage 1, and the result is the probes alone.

## Screen-space reflections

`engine/gi/include/engine/gi/screen_reflections.hpp` is the CPU reference of the SSR march. The ray marches a fixed number of uniform steps
through the frame's view-space depth. A sample hits when it lies within `thickness` behind the visible surface. A hit returns the lit color
already on screen, with confidence that fades to zero at the screen edges. A miss, which covers leaving the screen, going behind the eye,
running out of steps, and a surface rougher than `roughness_cutoff`, returns the environment (IBL specular) through `reflection_radiance()`.
Off-screen geometry is never visible to this pass, so a miss means the environment, not black.

Uniform marching misses surfaces thinner than one step, so the step length must stay below `thickness`. The defaults are 256 steps over
100 m (0.39 m per step) with a 0.5 m thickness, which meets the rule.

### GPU reflection pass

`SceneRendererDesc::screen_reflections` adds one compute pass after the opaque pass. The opaque pixel shader writes two more targets: the
shading normal with the roughness, and the Fresnel base with the occlusion. The pass reads the lit color, the depth values, and those
targets, and writes the corrected color into a separate `reflected` target. The post pass reads that target in place of the scene color.
Keeping the lit color single-writer is why the output is a new target.

The march follows `trace_screen_reflection` step for step, in the camera-relative space the scene shader uses. The depth of a point is its
clip `w`. The pass adds only the difference between the traced radiance and the environment specular, so a miss leaves the lit color
exactly as the scene shader wrote it (the environment fallback). It needs the post pass, because the lit color is HDR. `create` rejects it
without the post pass.

## Fixtures and tests

`tests/src/gi_probe_tests.cpp` bakes two fixtures. The outdoor fixture is open sky over a probe grid. The indoor fixture is an 8 x 3 x 8 m
room with floor, walls, roof, and a 2 m window in the north wall. The tests check that:

- indoor probes receive less light than outdoor probes, and probes near the window receive more than the far corner;
- a blocked probe takes no weight, and a neighbour's value is scaled by coverage;
- unbaked and off-grid lookups fall back to the environment, and a budgeted bake resumes;
- stale inputs are detected and never filled in;
- the binary format round-trips, and corrupt, truncated, and foreign-version data is rejected;
- the debug view colors valid, blocked, unbaked, and off-grid positions;
- the importer bakes the same bytes twice and rejects bad sources.

`tests/src/gi_reflection_tests.cpp` checks a hit on a visible wall, the off-screen, behind-the-eye, and rough misses that return the
environment exactly, and the rejection of mismatched buffers.

## Runtime loading

`engine/gi/include/engine/gi/cooked_volume.hpp` reads the converter's output without baking. `load_gi_volume(registry, output_dir,
source_path)` finds the record for a `gi_probes` source, reads `<output_dir>/<guid>.cooked`, unwraps the cooked container, and deserializes
the volume. It returns a reason for every failure: an unregistered path, a record with another importer, a missing cooked file, a bad
container, or a volume in another format version. The caller then renders with the environment alone. `decode_cooked_volume` does the same
for bytes already in memory. Staleness is the converter's job: the cook key covers the source bytes and the importer version, so a changed
source is rebaked before the runtime sees it, and `gi::is_current` checks the bake inputs.

## Debug view

`coverage_debug_color()` classifies a position by its nearest probe: green for valid, red for blocked, yellow for unbaked, and dark gray
outside the grid. It is the golden source for a probe-coverage overlay in the scene renderer, which is not built yet.

## Scope and what is not done

Built and checked on Windows (RTX 5080, DX12 and Vulkan, validation layers on):

- The probe grid is uploaded per volume and read by the pixel shader. Each pixel blends the valid probes around it trilinearly, and the
  environment's diffuse fills the rest (`ProbeDiffuse` in `scene.hlsl`). With no volume the grid is empty and the result is the environment,
  as before. `set_gi_volume` rejects a format version or a probe count that does not match the grid. Invalidation on source change is the
  caller's job: bake again and set the new volume (`gi::is_current` reports a stale one).
- The `ProbeCoverage` view is green where the cell is covered and red where no probe is (outside the grid).
- The reflection pass runs with `screen_reflections`. The GPU test shows a mirror reflecting an on-screen wall (`226,38,38` against the
  environment's `226,226,226`).
- The indoor/outdoor fixture is `tests/src/render_gi_gpu_tests.cpp`. A room under a roof with a doorway, and an open floor, on a slab. The
  room darkens with probes and the open floor darkens less; the coverage view is green inside and red outside. The fixture runs the
  `gi_probes` importer, writes its payload as `<guid>.cooked`, and loads it with `load_gi_volume` before `set_gi_volume`, so the GPU reads the
  file the runtime loader reads.

Known gaps. They are outside the US-040 acceptance criteria, and the status is **Implemented**:

- The probe bake runs on the CPU only. The runtime loader (`load_gi_volume`) is used by the GPU fixture and tested against a registry and a
  cooked file. No game executable exists yet (only `engine_tests` and `asset_converter` are built), so no startup path calls it, and a
  shipped scene does not get its probes from the converter output by itself.
- Alpha-blended surfaces get no reflection (the pass reads the opaque targets only).
- Reflections through off-screen geometry fall back to the environment; the probes do not cover the mirror image.
- The native Linux Vulkan GPU suite was not run (no usable Vulkan device on this WSL host).
- `engine_bench --suite acceptance` (US-074) does not exist yet, so no performance numbers are recorded.
