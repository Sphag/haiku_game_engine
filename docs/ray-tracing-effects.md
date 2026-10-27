# Ray-traced shadows and reflections (US-043)

`RtEffects` (`engine/render/include/engine/render/rt_effects.hpp`, `engine/render/src/rt_effects.cpp`) runs one compute pass,
`shaders/rt_effects.hlsl` (`CSRayTracedEffects`), that traces the camera's primary rays against a top-level acceleration
structure. It uses inline ray queries (`RayQuery`, SM 6.5) so that alpha-mask cutouts can be tested in the traversal. A ray
pipeline has no any-hit stage, so it cannot do that. The same shader runs on DX12 (DXIL) and on Vulkan (SPIR-V, with
`SPV_KHR_ray_query`).

## What the pass writes

Both targets are `R16G16B16A16` float, `width x height`:

| Target | Value |
|---|---|
| `shadow` | `x = 1` where the primary surface sees the sun; `0` where a shadow ray hits geometry within `shadow_distance`. A primary ray that hits nothing writes `1`. |
| `reflection` | `rgb` = the reflected surface's direct sun and ambient, weighted by Fresnel (dielectric F0 0.04, or the albedo for metals); `a = 1` when the reflection ray hits geometry within `reflection_distance`, `0` when it does not. |

The pass does not shade the primary surface and does not add the environment. The caller keeps its own direct lighting and its
environment term. Where `reflection.a` is `0`, the caller uses the environment, as the screen-space path does.

Limits, all per frame (`RtFrame`):

- **Roughness cutoff.** Only surfaces with `roughness <= roughness_cutoff` are reflected. The reference scene uses 0.4.
- **Reflection distance.** The reflection ray stops at `reflection_distance` (default 100 m).
- **Shadow distance.** The shadow ray stops at `shadow_distance` (default 1000 m). Shadows are directional only.
- **One bounce.** A reflection shades its hit with the sun and ambient, and its shadow ray. Nothing else bounces.
- **Bias.** Each ray starts `bias` meters along the surface normal.

Rays are not limited to the screen. A reflection of geometry outside the view is traced like any other hit. The off-screen case
is the point of the pass, and the reference scene checks it.

## Alpha-mask geometry

A masked instance's bottom-level structure is built non-opaque (`BottomLevelDesc::opaque = false`). Each non-opaque candidate
is tested in the traversal. The shader interpolates the UV from the triangle record at the candidate's barycentrics, samples
the mask atlas inside the instance's rectangle, and commits the candidate only where the alpha is at least `mask_cutoff`. A
masked hole therefore lets both the camera rays and the shadow and reflection rays through. Opaque instances use opaque
structures and skip the test.

## Geometry records

The pass reads two tables, both written by `RtEffects::set_scene`:

- **Instances** (64 bytes each, indexed by `TopLevelInstance::instance_id`): albedo, metallic, roughness, the instance's first
  triangle, the mask flag, the mask cutoff, and the mask rectangle.
- **Triangles** (64 bytes each): the object-space positions and UVs of each triangle. An instance owns a contiguous range, in the
  index order of its mesh. The table repeats a mesh for every instance that uses it.

The caller builds the top-level structure with `make_top_level_instance` (the record's transform, and `instance_id` = the record
index). The layouts are in `rt_effects.hlsl` and in `rt_effects.cpp` (`kParam*`, `kInstance*`, `kTriangle*`).

## Capability report

`rt_effects_geometry_support()` lists the geometry classes and whether the pass traces them. `geometry_supported(name)` answers
for one class.

| Class | Traced | Why |
|---|---|---|
| Opaque triangle meshes | yes | Opaque bottom-level structure. |
| Alpha-mask triangles | yes | Non-opaque structure and the candidate test above. |
| Alpha-blended triangles | no | A blended surface casts no shadow and gives no reflection hit. The raster term is kept. |
| Skinned and deforming meshes | no | The triangle table is uploaded once per scene. A refit (US-041) needs a per-frame table update, which the pass does not do. |
| Heightfield terrain | yes | Terrain tiles from `make_terrain_render_tile` are static opaque meshes, so they are traced like any opaque mesh. The splat weights do not affect tracing. |
| Procedural and AABB primitives | no | The RHI builds triangle structures only (US-041). |
| Particles, hair, and curves | no | Not triangle geometry. |

## Frame flow

1. `RtEffects::create` (needs the RayTracing feature; fails with `FeatureDisabled` otherwise).
2. `set_scene` uploads the tables and writes the descriptors. It replaces an earlier scene.
3. Per frame: write the top-level instances (`write_top_level`), build the top-level structure (`build_top_level`), `update_frame`,
   then `record` in a command list after the build. Both targets start in `Common` and end in `ShaderRead`.

`set_scene` and `update_frame` are synchronous: they go through the render graph's `execute`, which waits on its fence. Call them
between frames.

## Reference suite

The shared reference scene (`tests/src/render_rt_fixture.hpp`) and its checks:

- `render_rt_tests.cpp` (CPU, `[unit]`): the frame and scene validation, the byte images, the policies, the capability report,
  and `trace_reference` on the reference scene. The CPU tests check the scene's expectations (the pillar is off screen but its
  reflection is on screen, the shadow moves, the fence has holes, the rough box is never reflected, the distance limit loses the
  pillar, and RT off is the raster fallback).
- The off-screen case uses a **vertical** mirror. A horizontal floor cannot show it: a reflection keeps the horizontal bearing of
  its ray, so an object outside the view's horizontal field has an image outside the field too. The first version of the scene
  put the pillar in the floor's reflection, and the CPU reference found zero pixels.
- The scene has no overlapping objects in either occluder position. Where two surfaces interpenetrate, a reflection ray can hit
  either one, and the colors then depend on which one wins. The first GPU run showed this as 63 color mismatches in the moved frame.
- The sun is behind the fence (z < 0), so the fence's shadow falls toward the camera, onto the floor it can see. With the sun in front,
  the shadow falls behind the fence and the mirror, and the Windows CPU check found no mask-dependent shadow pixels.
- `render_rt_gpu_tests.cpp` (`[gpu]`, DX12 and Vulkan with debug layers): every frame is compared with
  `trace_reference` (up to 16 edge pixels may disagree). The cases cover the off-screen reflection, a moving occluder (the TLAS is
  rewritten and rebuilt between frames), the masked and unmasked fence, the roughness cutoff, the distance limit, RT on against RT off,
  the API order checks, and the missing feature.

## Known gaps

- **Full-profile constraints from the PRD not yet met.** The table in the PRD asks for half-internal-resolution reflections and
  for RT shadows with temporal and spatial filtering. The pass runs at the size it is created with (a caller can pass half the
  internal size), but nothing upsamples its output, and it does not filter. The filter belongs to the denoiser (US-045).
- **Renderer integration limits.** `SceneRenderer` runs the pass when `SceneRendererDesc::ray_traced` is enabled (see
  "Renderer integration" below). The pass traces only the static, non-blended items of the snapshot: skinned meshes and
  alpha-blended surfaces are not traced, and a blended surface casts no RT shadow. Each frame rebuilds the tables and the
  scene's `RtEffects` bindings, which is simple but not optimal. There is no game executable yet (US-074 and later stories).
- **One directional light.** The traced shadow is the sun's, and `scene.hlsl` applies it to every directional light, so
  the renderer rejects a frame with two. The sun's ambient term at a reflected hit is zero, so a reflected surface in shadow
  is black.
- **Single frame, no filtering.** There is no temporal or spatial filter, so shadow edges are hard. US-045 owns the denoiser.
- **Directional shadows only.** Local lights use the shadow atlas (US-035), not this pass.
- **Vulkan on Linux is not run here.** This WSL host has no usable Vulkan device; the Linux Vulkan GPU suite is not claimed.

## Renderer integration

- `SceneRendererDesc::ray_traced` (`RayTracedSettings`) turns the pass on. It needs the RayTracing feature and
  `SceneShaders::ray_traced` (CSRayTracedEffects). `build()` creates the pass, and `set_ray_traced_settings` changes the cutoff and
  the ray lengths afterward.
- Each static mesh gets bottom-level structures on first use: opaque, or non-opaque for alpha-mask materials. Its vertex and
  index buffers carry `ShaderResource` usage. The build moves them to `ShaderRead` and back to `Common` through the graph, so draws
  see the state they always saw.
- Each frame, `prepare_ray_traced` writes a top-level structure from the snapshot's items (instance id = item order), binds the
  scene and the camera, and the render graph's pass builds the top level and records the pass before the scene pass.
- The mask atlas stacks the base-color alpha of the alpha-mask materials in one column. A texture's rectangle spans `w - 1` texels,
  so a UV of 1 lands on the last texel, as the shader's clamp does. The cutoff is `alpha_cutoff / (base alpha * tint alpha)`.
- `scene.hlsl` reads the targets at bindings 3 and 4 of the lighting set. The sun's visibility multiplies a directional light, and
  a traced reflection replaces the environment specular where its alpha is 1. Without the pass, 1x1 dummies hold the neutral values
  (shadow 1, reflection 0). `LoadTraced` clamps each read to the target's size, so the dummies work at any pixel.
- An off-screen mirror can show an object only if the snapshot holds it. `extract_frame(..., keep_offscreen = true)` keeps the
  out-of-view items as shadow-only items. The scene draw never draws them.
- `render_rt_scene_gpu_tests.cpp` (`[gpu]`) renders the reference scene through `SceneRenderer`, RT on against RT off: the sun's
  shadow follows the occluder, and a vertical mirror shows the pillar that is off screen.
