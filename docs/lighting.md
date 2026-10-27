# Lighting and raster shadows (US-035)

`engine/render/` (library `engine_render`) lights the scene with directional, point, and spot lights, and shadows the directional lights
with cascades and the spot lights with tiles of one atlas. The CPU side (`lighting.hpp`, `shadow_atlas.hpp`) decides what each light
touches and which tiles are drawn. The GPU side (`shaders/scene.hlsl`) shades with the lists and reads the shadows. The debug views in
`LightingView` show the decisions per pixel, so a wrong bound, an acne ring, or a missing light is visible in one image.

## Lights

`Light` (world space, double position) is a directional, point, or spot light. `validate_light` rejects non-finite values, a range of zero
on a positional light, a direction that is not a unit vector, spot angles outside `0 <= inner <= outer < pi/2`, and bias values out of range.

- Falloff (`light_attenuation`, mirrored in `LightScale` in the shader): intensity over distance squared, times a smooth window
  `(1 - (d / range)^4)^2` that reaches zero at the range. A spot light multiplies that by a squared smoothstep between the cosines of
  the outer and inner cones.
- Shading: GGX specular with Smith visibility and Schlick Fresnel, and Lambert diffuse weighted by the energy the specular term leaves
  (`DirectBrdf`). Direct light adds to the image-based light of US-034.
- Priority: the renderer sorts lights by `priority` (highest first, ties keep the author's order). Past `LightingSettings::max_lights`,
  the lowest priorities are dropped and counted in `LightingStats::dropped_lights`.

## Clusters

The view is a grid of screen tiles (`tile_px` pixels) by `slices` depth slices. Slices are spaced exponentially between the camera's near
and far planes, so near geometry gets finer clusters. `ClusterGrid::slice_of` and `cluster_of` are the one definition of that mapping; the
shader repeats the formula with the same header values.

`assign_clusters` gives each light the clusters its sphere reaches. A point or spot light's sphere has radius `range`. The screen footprint
is the projection of the sphere's bounding box, which contains the sphere's projection, so the assignment is conservative: a cluster may
hold a light that does not touch it, and never misses one that does. A sphere that reaches the near plane or passes behind the eye covers
every tile of its slices. A directional light covers every cluster.

Overflow control: each cluster stores at most `max_lights_per_cluster` indices (16-bit count per cluster). Lights are assigned in priority
order, so a full cluster keeps the highest-priority lights. A cluster that drops a light sets `kClusterOverflowBit`, and the count of
dropped pairs is reported. The `ClusterOverflow` view paints those clusters red.

The lists are built on the CPU each frame and uploaded in one buffer with the lights and shadow records. The GPU looks a fragment up by its
pixel position and its view depth (`dot(p, forward)`), so no compute pass is needed for culling.

## Shadows

### Cascades

`plan_cascades` splits the camera's frustum from the near plane to `min(far, shadow_distance)` with the practical scheme: `lambda`
blends logarithmic and uniform splits. Each cascade is a sphere around its slice's axis at mid depth, so its size stays the same as the
camera turns (the radius rounds up to 1/16 m). The light's view looks at that sphere from `radius + caster_margin` away. Its projection is
orthographic, with depth from 0 to `distance + radius`. The center snaps to the light's texel grid, so the map moves in whole texels and
does not shimmer.

`caster_margin` extends each cascade's depth range toward the light, so casters outside the sphere still shadow it.

Casters outside the view are kept too. When a directional light casts shadows, `extract_frame` keeps an object outside the frustum as a
shadow-only item (pick id zero) if it lies in the cylinder from the shadow volume toward the light. It is drawn into shadow tiles and never
into the scene. Without this, a shadow would depend on what the camera happened to see.

### Spot lights and the local atlas

A spot light gets one perspective tile whose field of view is its outer cone. Its near plane is `max(0.02, range * 0.002)`.

### Atlases

There are two square R32Float depth atlases, each with its own D32 depth buffer: the cascade atlas (default 4096, tiles of 2048, for four
cascades) and the local atlas (default 4096, tiles of 2048, for four spot lights). These are the native profile's sizes (PRD Image
Profiles). Each tile is a square region, allocated by shelves: a shelf is a row of tiles of one height. A released tile of the same size is
reused. A request the atlas cannot fit is not granted, the light casts no shadow that frame, and `shadow_maps_dropped` counts it.

A tile is drawn into the atlas with the atlas's clip remapped to the tile (`tile_remap`). The remap scales clip x and y into the tile and
keeps z and w, so depth values are unchanged. The vertex stage writes the tile's clip position. The light's own unremapped clip goes in
the frame buffer at byte 64, and the pixel stage discards fragments outside the light's frustum. Without that discard, geometry outside the
frustum would land in a neighboring tile.

The row order depends on the backend. The shader reads the same row formula the rasterizer uses:
`row = origin + (0.5 + 0.5 * sign * ndc_y) * size`, where `sign` is `+1` on Vulkan (its NDC y points down, and the flipped matrix is used)
and `-1` on DX12. The renderer writes `sign` into the lighting header, and `tile_row_sign` gives the same value on the CPU.

### Invalidation

Every tile has a key: a hash of its light matrix, its tuning (bias, normal bias, PCF radius, split, tile size, kind), and the casters whose
bounding spheres reach its frustum (model matrix and mesh). `ShadowAtlas::request` redraws a tile when its key changes, when it was just
allocated, or when `invalidate` marked it stale. A tile not requested in a frame is released at `end_frame`. `invalidate_shadows(light_id)`
and `invalidate_all_shadows()` mark tiles stale by hand, for content the renderer cannot see.

A caster that does not reach a tile's frustum does not change its key, so moving it does not change that tile. A camera move changes the
cascade matrices and so changes the cascades, which is correct because the cascades follow the view.

The unit of redraw is the atlas, not the tile. A tile's texels keep what was last drawn into them, so when a caster leaves a tile, the old
depth would stay behind as a phantom occluder. When any tile of an atlas changes, that atlas's color is cleared to the far depth, and every
granted tile in it is drawn again. An atlas with no changed tile draws nothing. `shadow_maps_rendered` counts the tiles whose keys changed;
`shadow_draws` counts the caster draws actually issued, which includes the tiles redrawn with the changed one.

### Bias and filtering

Each light sets `depth_bias` (NDC depth units, subtracted from the receiver's depth), `light_bias` (texels of its map, along the surface
normal before the lookup, so the offset scales with the map's world texel size, which grows with the cascade), and `pcf_radius` (texels;
the filter averages `(2r + 1)^2` comparisons). Taps outside the tile read the tile's edge, so the
filter never reads a neighbor. Points outside a light's volume are lit.

### Scope calls

- Point lights light the scene but cast no shadows. A point shadow needs six faces per light, and that is not in this story.
- Alpha-mask casters cast full shadows, because the shadow pass does not sample the mask. Alpha-blended items cast nothing.
- The shadow pass clears each tile's depth with a full-atlas depth clear, so a large atlas costs more to clear than a small one. The
  performance contract is not measured for this story.

## Debug views

`SceneRenderer::set_lighting_view` replaces the scene's color with one value per pixel:

| View | Shows |
|---|---|
| `Lit` | the shaded image (default) |
| `ClusterCount` | grey, the number of lights in the pixel's cluster over `max_lights_per_cluster` |
| `ClusterOverflow` | red where the cluster dropped a light |
| `CascadeIndex` | red, green, blue, yellow for cascades 0 to 3; black outside every cascade |
| `ShadowFactor` | grey, the product of the visibilities of the shadowed lights (1 is lit) |

`LightingStats` reports, for the last frame: the lights and the dropped lights, the shadow maps used, rendered, and dropped, the shadow draws,
and the overflow clusters and dropped pairs.

## Lighting header and records

The lighting buffer (set 3) holds one header (256 bytes), then the light records (64 bytes each), the shadow records (128 bytes each), the
cluster counts, and the cluster indices. The header holds the cluster grid, the camera forward, the counts, the view mode, the row sign,
and the byte offsets of each array. `scene_renderer.cpp` writes it, and `scene.hlsl` reads it. Each layout is named in both files next to
its offsets.

## Verification

- CPU (`tests/src/lighting_tests.cpp`, unit): light validation, the cluster grid and its slice boundaries, point, directional, and
  off-screen assignment, overflow with priority order, attenuation, cascade splits and fit, texel stability under camera rotation, the
  spot matrix, the atlas (grant, reuse, invalidation, release, refusal, resize), the tile remap on both row orders, and shadow-only
  casters from the snapshot.
- GPU (`tests/src/lighting_gpu_tests.cpp`, DX12 and Vulkan): the cluster and overflow views on the point light, the directional shadow
  and the no-shadow control, the acne fixture (zero bias shows self-shadowing; the tuned bias removes it), the cascade colors on the
  ground, spot shadows with invalidation and the atlas budget, and a missing light that changes the image.
- The GPU suites check the debug views, not the shaded image, so a change to the BRDF does not change them.

Not in the GPU suites: an exact per-pixel comparison of direct lighting with a CPU reference. The direct BRDF exists only in
`scene.hlsl`, and the checks above use the shader's decisions and visibilities.
