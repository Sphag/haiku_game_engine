# Scene rendering and picking (US-033)

US-033 draws a scene's objects through the render graph (US-032) on DX12 and Vulkan, and it picks them by entity ID. The
library is `engine_render` (`engine/render/`). It depends on `engine_scene` for the scene graph and on `engine_rhi` for the device.
The CPU parts run without a GPU, and the GPU parts run on each backend's GPU suite.

## Pipeline

1. **Extraction** (`extract_frame`, `engine/render/include/engine/render/frame_snapshot.hpp`). It runs at the synchronization point.
   It refreshes dirty transforms through the `SceneGraph`, skips renderables whose node is not alive (`stale`) or whose mesh index is
   unknown (`unknown_mesh`), culls the rest against the frustum, and copies each survivor into a `SnapshotItem`. The snapshot is a
   value. It holds the view-projection matrix, the items, and the entity handles, but no pointer into the graph or the ECS registry.
   A snapshot taken before a change keeps its contents after the change.
2. **Culling.** Each mesh has a bounding sphere in object space (`compute_bounds`). The sphere is moved to camera-relative space, and
   its radius is scaled by the largest axis length of the model matrix. `sphere_visible` tests it against the six frustum planes
   (Gribb and Hartmann, for depth in 0 to 1). A sphere that touches a plane is kept.
3. **Drawing** (`SceneRenderer::render`). The frame is a render graph of three passes: upload the per-frame buffer, draw every item
   into three color targets and a depth buffer, and copy the targets to readback buffers.
4. **Readback** into a `SceneImage`: RGBA8 color, the entity pick ID per pixel, and the NDC depth per pixel.

## Conventions

- **Camera-relative positions.** The camera's position is subtracted from each object in `double`, and the result is converted to
  `float` once. The view matrix is the camera's rotation alone. The transform math is described in `docs/scene-graph.md`.
- **Clip space.** Engine clip space has depth in 0 to 1 and y up (`docs/runtime-conventions.md`). The scene renderer gives DX12 the
  matrix as it is. On Vulkan it gives `coords::to_vulkan_clip()` of the matrix, because Vulkan's NDC y points down. Both backends
  therefore place the same pixel for the same point.
- **Pick IDs.** A pick ID is the 1-based position of an item among the survivors, in renderable order, and zero means background.
  `FrameSnapshot::entity_of` maps an ID back to its entity. Because the IDs follow the survivors, a deletion renumbers the items that
  follow it. A new snapshot gives the current numbering, and a handle that was deleted is never returned (the slot's generation changes).
- **Depth.** The depth buffer is `D32Float`, tested with LESS and written. The depth values are also written to a separate `R32Float`
  target by the pixel shader, because the RHI cannot copy a depth buffer to the host. Background is 0 for that target, and the depth
  buffer starts at 1.

## Frame layout

`engine/render/shaders/scene.hlsl` reads a frame buffer at `t0` in set 0 for each draw (one draw per opaque mesh and material group, and one
per alpha-blended item). The frame is the camera, then one 208-byte record per item, then the palettes of the skinned items (US-036):

| Bytes | Content |
|---|---|
| 0..63 | view-projection, column-major (Vulkan's flipped matrix on Vulkan) |
| 64..127 | shadow draws only: the light's unremapped clip matrix |
| 128..191 | previous view-projection, for motion vectors |
| 192..199 | velocity scale: (0.5, -0.5) on DX12, (0.5, 0.5) on Vulkan |
| 256 + 208 * i | item i: model (0), inverse-transpose normal matrix (64), tint (112), pick id (128), bone count (132), palette offset (136), previous model (144) |

A skinned item's palette is its bone count of matrices for the current pose, then the same count for the previous pose, at the palette offset.
The previous model is the item's previous model moved by the camera's motion. The layout is in `scene_renderer.hpp`, and
`docs/animation.md` describes the skinning and the motion vectors.

Materials and textures are in sets 1 and 2, and they are described in `docs/materials.md`.

## Reference rasterizer

`rasterize_reference` (`reference_raster.hpp`) draws the same snapshot on the CPU with the same matrix, the same pixel-center
sampling, and the same depth rule. It is the golden source for the tests and a check on the GPU results. It does not clip triangles
at the near plane: a triangle with a vertex at w <= 0 is skipped. The golden scene keeps clear of that case, and the GPU clips it.

## Golden scene

`tests/src/render_scene_fixture.hpp` holds the scene. The camera is at z = 10, looking down -Z, with a 60 degree field and a 64 x 64
target. It has a near box (red) and a far box (blue) behind it, a half-size box to the right (green), a distant box (yellow) far behind,
and two culled objects (one off to the side, one behind the eye). The renderable list also has one destroyed node.

- Before deletion, the center pixel is the near box, with pick 1 and depth at distance 9. Pixel (49, 32) is the side box, with pick 3 and
  depth at distance 9.5. The corner is background.
- After the near box is destroyed, the center is the far box, with pick 1 and depth at distance 13. The side box is pick 2, so its
  number moved with the survivors.

The depth values come from the formula `far / (far - near) - far * near / ((far - near) * d)`, not from the renderer.

## Tests

- `tests/src/render_scene_tests.cpp` (`[unit]`, and `[integration]` for the Validation device): camera validity and axes, the frustum,
  mesh validation and bounds, extraction numbers, snapshot independence, deletion and slot reuse, golden pixels from the reference
  rasterizer, the depth test in both draw orders, picking after deletion, and a moved object. The last case builds the pipeline on the
  Validation device and checks that the missing mesh upload is reported as `Unsupported`, with nothing left allocated.
- `tests/src/render_scene_gpu_tests.cpp` (`[gpu][dx12]`, `[gpu][vulkan]`): the golden scene on each backend with the debug layers on. It
  checks the golden pixels before and after the deletion, compares every pick ID with the reference, and checks that release leaves the
  resource counts where they started.

## GPU indirect draws (US-037)

With `DrawPath::Indirect` (the default), the opaque and alpha-mask items are culled and drawn on the GPU. The frame has these steps:

1. Extraction still culls on the CPU against the view (`extract_frame`), so the snapshot holds the visible items. Those items are the reference.
2. The opaque items are grouped by (mesh, material). Up to `max_draw_groups` (64 by default) groups become indirect groups, and any others
   draw indexed. A skinned group needs the skinned indirect entry, so without it the skinned groups draw indexed.
3. The candidates of the indirect groups are uploaded as one frame buffer, in group order. The cull buffer holds the frustum planes (from
   the same view-projection the CPU culled with), each group's index count and bounding sphere, and each candidate's group.
4. The cull pass (`CSCull`, compute) tests each candidate's sphere against the six planes, with the same test as the CPU. A visible
   candidate takes the next slot of its group's arguments, and the group's count grows by one (`InterlockedAdd`). The slot's record is
   a `DrawIndexed` of 20 bytes with the candidate's index as `first_instance`. A record past the group's capacity is not written.
5. Each group is one indirect count draw: DX12 `ExecuteIndirect` with the count buffer, or Vulkan `vkCmdDrawIndexedIndirectCount`. The
   count is clamped to the capacity by the API. The vertex shader (`VSIndirect`, `VSSkinnedIndirect`) takes the item index from a
   per-instance vertex attribute (bound in slot 1, at the location after the vertex inputs: 3 static, 5 skinned), not from `SV_InstanceID`, because DX12 does not add `first_instance` to
   `SV_InstanceID`.

Shadow tiles and alpha-blended items stay on the indexed path (the shadow casters are chosen on the CPU, and blended items are drawn back to
front). `IndirectStats` reports what the frame did: the groups on each path, the candidates, the items the GPU kept (`visible`), the items
drawn within capacity (`drawn`), and the overflow. `gpu_culling = false` draws every candidate.

Indirect falls back to indexed when the shaders are missing (`SceneShaders::indirect_vertex` and `cull`), the device lacks `IndirectCount`,
or `max_draw_groups` is zero. `indirect_stats().active` says which path the last frame used.

Limits: the cull selects no levels of detail, because meshes have one level (LOD generation is a later story). The argument buffer holds
`max_draw_groups * draw_group_capacity` records (the capacity defaults to `max_items`), so memory grows with both.
Tests: `tests/src/render_indirect_gpu_tests.cpp` draws the golden scene and a skinned box indexed and indirect, and compares the pixels, the
pick IDs, the motion vectors, and the visible count with the CPU's drawn count. It also covers culling off, overflow past the capacity, and an
empty frame. `engine/render/src/scene_renderer.cpp` (`build_indirect`, `render`) holds the code.

## Limits of this version

- Shading is physically based (US-034, `docs/materials.md`). Direct lights and shadows arrive with US-035.
- Triangles that cross the near plane are not clipped on the reference path. The GPU clips them.
- Alpha-blended items are sorted back to front by the distance to their bounds' center, and they do not write entity IDs or depth. Order-independent
  transparency is not in this version.
- Visibility is decided on the CPU during extraction, and the GPU culls the opaque groups again against the same planes. The shadow path stays
  indexed. GPU indirect draws are described above.
- The scene renderer reads back every target after each frame, which is for tests and diagnostics. The editor and runtime need a path that
  keeps the targets on the GPU.
- `engine_bench` does not exist yet, so the acceptance benchmark cannot run for this story.

## Mesh shaders on meshlets (US-038)

A static mesh with attached meshlets draws its opaque and mask items through mesh shaders when the device has them. `SceneRenderer::attach_meshlets` appends the cooked clusters of the mesh's LOD 0 to the pools, and each (mesh, material) group is one task dispatch. `ASMeshlet` culls each (item, meshlet) pair against the frustum and the normal cone on the GPU. `MSMeshlet` writes the survivors, with the vertex transform of `VSMain` and the pixel shader of the other paths. `SceneRendererDesc::geometry` and `set_geometry_path` switch a frame between this path and the conventional one, and `geometry_path()` reports the path a frame used. Each task group writes its survivor count, read back as `meshlet_stats()`. Skinned meshes, alpha-blended items, and shadow tiles stay on the other paths. The frame layout gains a meshlet header at bytes 200 to 224 of the camera area, which only meshlet draws write. The design, the limits, and the tests are in `mesh-shaders.md`.
