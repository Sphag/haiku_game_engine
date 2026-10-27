# Denoising and volumetric fog (US-045)

The full profile's ray-traced signals (US-043 shadows and reflections, US-044 diffuse probes) are one frame of noisy samples. This story adds
an in-house temporal and spatial denoiser for those signals and a froxel volumetric fog with directional and local lighting and shadows. Both
run as separate compute passes with a CPU reference each, and both are checked on the GPU against that reference.

## Pieces

| Piece | Where | Role |
|---|---|---|
| `render::TemporalDenoiser` | `engine/render/include/engine/render/denoiser.hpp`, `engine/render/src/denoiser.cpp` | The GPU pass for one target: prefilter, reprojection, history rejection, blend, two ping-ponged history sets |
| `denoise_reference` | `denoiser.cpp` | The CPU reference of one frame, on the same values |
| `denoise.hlsl` | `engine/render/shaders/denoise.hlsl` | `CSTemporalDenoise` |
| `render::VolumetricFog` | `engine/render/include/engine/render/volumetric_fog.hpp`, `engine/render/src/volumetric_fog.cpp` | The two GPU passes: inject (lighting, shadows, temporal blend) and integrate (column accumulation) |
| `inject_reference`, `integrate_reference`, `apply_fog` | `volumetric_fog.cpp` | The CPU reference and the pixel application |
| `volumetric_fog.hlsl` | `engine/render/shaders/volumetric_fog.hlsl` | `CSFogInject` (inline ray queries for the shadows), `CSFogIntegrate` |
| `camera_cut`, `light_changed` | `denoiser.hpp` | The reset tests `SceneRenderer` uses to drop the history |
| RT geometry outputs | `rt_effects.hpp`, `rt_effects.hlsl` | The primary hit's depth, normal, and motion, which the denoiser reads |
| Renderer wiring | `scene_renderer.cpp` (`build_denoisers`, `build_fog`, `update_denoise_and_fog`) | The passes in the frame's graph, the resets, and the lit color the reflection and post passes read |

## Denoiser

Inputs per pixel: the noisy value (rgba), the linear view depth, the world normal, and the motion vector (the UV offset to the previous frame).

1. **Prefilter.** A box over `spatial_radius` (0 to 2 pixels). A neighbour counts only if its depth is within `depth_tolerance` (relative) and
   its normal's dot product with the pixel's is at least `normal_threshold`. The pixel itself always counts.
2. **Reprojection.** The previous pixel is `uv + motion`, rounded down. It is outside the screen, or its previous depth or normal disagrees
   with the current pixel's, the history is rejected (disocclusion).
3. **Blend.** An accepted sample blends toward the filtered value with weight `max(1 - history_weight, 1 / (age + 1))`. The age counts accepted
   frames up to `max_age`, so the history's share falls every frame and a stale value cannot persist. A rejected pixel restarts at its
   filtered value with age 1.
4. **Reset.** `update_frame(settings, reset)` drops the history for that frame. A size change restarts it too.

The history is three textures per set: the value (rgba, the output), the depth (R32F), and the normal with the age in w. Set `p` reads history
`p` and writes `1 - p`, and `output()` is the history the last `record()` wrote.

## Volumetric fog

The grid is `width x height x depth` froxels. The PRD's full profile uses 160 x 90 x 64. Slice edges grow exponentially from `near` to `far`,
and a froxel's centre is on the view ray through its column's centre, at its slice's geometric middle.

Each froxel's in-scatter is `density(y) * albedo * sum(light * phase * visibility * attenuation)`:

- The sun: radiance times a Henyey-Greenstein phase, times the visibility of the segment to infinity (`kFogSunDistance`).
- Each local light: radiance times `(1 - d / range)^2 / (1 + d^2)`, times the phase, times the visibility of the segment to the light, if the
  light is `shadowed`.
- The density falls off as `density * exp(-height_falloff * y)`, and the extinction equals the density.

The visibility is an inline ray query against the top-level structure (opaque triangles only). A shadowed fog therefore needs the RayTracing
feature. The inject pass reprojects each centre into the previous injected grid and blends with the same weight rule as the denoiser, so a
camera cut or `reset` drops the history.

The integrate pass accumulates each column front to back: a slice adds `S * T` with `S = scatter * (1 - exp(-ext * dz)) / ext`, and multiplies
`T` by `exp(-ext * dz)`. `apply_fog` attenuates a pixel's colour by the transmittance of its slice and adds the accumulated radiance.

The RHI has no volume textures, so the grid is a buffer: 16 bytes per froxel, at `((z * height + y) * width + x) * 16`.

## Wiring into `SceneRenderer`

Both effects are set by `RayTracedSettings::denoise` and `RayTracedSettings::volumetric_fog`. Both flags are fixed at create, and both need
`ray_traced.enabled`. The fog also needs the post pass, because it adds to the HDR color.

Each frame, in the order of the graph:

1. **`ray-traced effects`** builds the top level and writes the shadow, the reflection, and the primary geometry (depth, normal, motion).
2. **`denoise shadow`**, **`denoise reflection`**: each denoiser filters its target over the same geometry. The scene's lighting binds the
   denoisers' next outputs (`TemporalDenoiser::next_output`), so the sun's shadow and the reflection are the denoised ones.
3. **`fog inject`**, **`fog integrate`**: the froxel grid, traced against the same top level.
4. **`scene`**: the opaque and blended passes, with the denoised targets.
5. **`fog apply`**: the scene's NDC depth picks each pixel's slice, and the fogged color replaces the lit color for the reflection and post
   passes. A pixel with no geometry (NDC 0, the cleared background) takes the far plane.

`SceneRenderer::frame_passes()` returns the last frame's passes with their names and times, so each effect is measured on its own.

The primary geometry is computed from the camera. The RT pass gets the previous camera (relative to the current one), and each primary hit's
motion is the change of its view projection between the two cameras. A moving traced instance has no motion of its own, so its history is
rejected only where its depth or normal disagree with the previous frame's.

### Resets

The renderer drops the history of both effects when:

- the camera is a cut: it moved more than `kCameraCutMeters` (5 m) or turned more than `kCameraCutDegrees` (30°) since the last frame, or
  `notify_camera_cut()` was called;
- the sun turned more than `kLightResetDegrees` (0.5°), or it appeared or went away. This resets the denoisers and the fog;
- a point light moved more than `kLightResetMeters` (1 mm), or the set of point lights changed. This resets the fog only.

The first frame, and the first frame after a resize, start with no history. The age floor still bounds any history that is not reset.

## Limits

- **The fog takes point lights only.** Spot lights are not used, and the renderer does not trace the shadows of local lights (their
  `shadowed` flag is off). The sun's shadow is traced when `FogSettings::shadows` is set. The PRD's budget of four shadowed local lights is not
  enforced.
- **Fog shadows need RT.** A device without the RayTracing feature cannot create `VolumetricFog`. The native profile has no raster shadow
  path for the fog.
- **Visibility is opaque and world-space.** The fog's shadow rays trace the top level, with no alpha masks and no skinned or blended items (the
  same geometry as the RT pass).
- **Motion follows the camera only.** A moving instance is not motion-compensated (see above).
- **The scene test does not compare a frame with a reference image.** It checks the passes, their order, and that the fog changes the frame. The
  denoiser's and the fog's values are compared with their CPU references in their own suites.
- **No benchmark numbers.** These passes are not in `engine_bench --suite acceptance` (US-074), so the fixed-budget numbers are not measured.

## Verification

- CPU unit tests, `tests/src/render_denoise_tests.cpp` and `tests/src/render_volumetric_tests.cpp`: settings and frame validation, camera-cut
  and light-change thresholds, the prefilter, the depth edge, disocclusion and motion rejection, the reset, the age floor's convergence, NaN
  handling, the slice and density math, the homogeneous transmittance, the sun and local-light visibility, the temporal blend and its reset,
  and the parameter layouts.
- GPU tests, `tests/src/render_denoise_gpu_tests.cpp` and `tests/src/render_volumetric_gpu_tests.cpp`, on DX12 and Vulkan with the debug
  layers: the denoiser over a four-frame sequence with a disocclusion and a camera cut, and the fog over a first frame, a moving sun, and a
  camera cut against a box occluder, both compared with the CPU reference. Each suite also checks the API's refusals.
- Scene GPU tests, `tests/src/render_rt_scene_gpu_tests.cpp` (`run_denoise_fog_case`): the renderer with both effects runs the five passes in
  order (after the RT pass, before the scene for the denoisers and the fog's inject, after the scene for the fog's apply); the pass is absent
  when the flag is off; the fog at density 0.1 moves the frame's total light by more than 3% from density 0.
- CPU tests: the RT frame's previous camera at bytes 144 to 208 (`render_rt_tests.cpp`), and the fog's depth conversion and camera range
  (`render_volumetric_tests.cpp`).
- The GPU tests need the Windows host's RTX 5080 for evidence. The Linux Vulkan GPU suite has no device here and is not claimed.
