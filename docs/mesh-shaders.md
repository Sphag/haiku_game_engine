# Mesh shader path (US-038)

The mesh shader path draws the static opaque and alpha-mask items of a mesh through GPU meshlets. A meshlet is a cluster of at
most 64 vertices and 124 triangles from the converter's LOD 0 (`mesh_cooking.hpp`). The path is one of the geometry paths of the
scene renderer. The others are the indirect and indexed paths (`scene-rendering.md`), which stay in place and draw the same
geometry.

## What is implemented

- **RHI.** `ShaderStage::Amplification` is the task stage, and `PipelineKind::Mesh` takes an optional amplification
  stage, a mesh stage, and a pixel stage, with no vertex input. `dispatch_mesh` records the task dispatch on both backends:
  `DispatchMesh` (DX12, through a pipeline state stream on `ID3D12Device2`) and `vkCmdDrawMeshTasksEXT` (Vulkan, through
  `VK_EXT_mesh_shader`). Both backends need a pass and a bound mesh pipeline.
- **Capabilities.** `Feature::MeshShaders` comes from the adapter: DX12 `MeshShaderTier` 1 or better, and Vulkan with
  `meshShader`, `taskShader`, and `vertexPipelineStoresAndAtomics`. `DisabledForTest` forces it off, and then nothing
  uses mesh shaders (the device report says so).
- **Shaders.** `scene.hlsl` has `ASMeshlet` (amplification) and `MSMeshlet` (mesh). They reuse `PSMain` and the vertex fields
  of `VSMain`. DXC compiles them as `as_6_5` and `ms_6_5`. The SPIR-V build adds `-fspv-extension=SPV_EXT_mesh_shader`.
- **Cooked data.** `MeshletData` (`engine/render/meshlet.hpp`) takes the cooker's clusters. `validate_meshlets` checks the
  limits, the ranges, each vertex against its sphere, and that the meshlets' triangles are exactly the mesh's triangles
  (same vertices, same cyclic order, each once). A mesh cooked once serves both paths.
- **Pools.** Every attached mesh appends to four fixed-capacity buffers (records, vertex indices, triangle bytes, and a
  copy of the mesh vertices in the static layout). A fifth buffer holds the visibility counts. `MeshletPoolSettings` sets the
  capacities. An attach that does not fit fails with `OutOfRange` and changes nothing.
- **Culling on the GPU.** `ASMeshlet` runs one thread per (item, meshlet) pair. A pair survives when its bounding sphere,
  scaled by the item's longest column, is inside the six frustum planes (the planes of the view-projection, as
  `camera.cpp`), and its normal cone faces the eye. The cone test is meshoptimizer's sphere form,
  `dot(center - eye, axis) >= cutoff * |center - eye| + radius`, with the eye at the origin because the model is
  camera-relative. A cutoff of one never culls, and neither does a two-sided group: its back faces draw, so only the frustum
  test applies to it. The survivors go to the mesh stage as the payload.
- **Draws.** Each (mesh, material) group of static opaque and mask items is one task dispatch. A group's frame buffer holds
  its items and a meshlet header (camera area, bytes 200 to 224: the first record, the meshlet count, the item count, the
  first counter, the task group count, and whether cones cull), so every draw has its own buffer, as the indexed path does.
- **Switching.** `SceneRendererDesc::geometry` and `set_geometry_path` select `Meshlets` or `Conventional`. The effective
  path is `geometry_path()`, which is `Conventional` when the feature or the shaders are missing. Switching does not upload
  anything.
- **Counts.** Each task group writes its survivor count. The counts need no reset, because each task group owns one entry
  and the readback reads only the entries the frame wrote. `meshlet_stats()` reports the groups, the pairs (candidates), and
  the survivors (visible).

## What stays on the other paths

- Skinned meshes (they draw through the indirect path, US-037). `attach_meshlets` refuses them.
- Alpha-blended items, which draw through the indexed path in the blended pass.
- Shadow tiles, which draw through the shadow path.
- Meshes without attached meshlets.

## Limits and scope calls

- **LOD 0 only.** The cooker builds more levels, but the renderer draws LOD 0 on both paths, so a mesh has one geometry
  to compare. LOD selection needs a distance policy that does not exist yet.
- **Meshes are attached in memory.** Loading meshlets from the cooked payload into the renderer is not part of this story.
  The runtime path reads `OWMD` payloads once the asset loader feeds the renderer. The tests cook a source mesh with the
  converter and convert the result (`tests/src/render_meshlet_fixture.hpp`).
- **Pool uploads copy the whole list.** Each attach re-uploads the used prefix of each pool. This is cheap for the test
  scenes and wrong for a streaming world. Incremental uploads come with the streaming story.
- **No benchmark.** `engine_bench` does not exist yet, so the `render_stress` route (`PRD` benchmark table) is not run.
  The `meshlet` path is not measured against the budget.

## Bring-up findings

- **An atomic on a member of the payload struct lost its update.** The first task stage counted survivors with
  `InterlockedAdd(payload.count, ...)` on a groupshared payload. The DXC output on DX12 returned the atomic's slot but left
  the count at zero after the barrier, so every pair looked culled. The count and the pairs are now plain groupshared
  variables, and the payload is built after the barrier. Keep atomics off the payload struct.
- **The CPU reference draws back faces.** `rasterize_reference` does not cull by facing, but the GPU culls the back faces of
  one-sided materials. A test that draws a flipped plane against the reference must hide the plane or use a two-sided material.
- **Cone culling ignored two-sided materials.** A flipped plane with a two-sided material was culled by its cone, which dropped
  its back faces. The header now carries whether cones cull, and the CPU reference takes the same flag.

## Verification

Recorded in `build.md` (US-038 row). The GPU tests are in `tests/src/render_meshlet_gpu_tests.cpp`:

- The meshlet frame agrees with the conventional frame of the same renderer, pixel for pixel (pick ids, colors within one
  step, depth within 1e-6), and both agree with the CPU reference within the scene tests' bounds.
- The GPU's visible count equals the CPU reference's count of surviving pairs, and a flipped plane's meshlet is culled by its
  cone.
- With mesh shaders disabled, the device reports `DisabledForTest`, the attach fails with `Unsupported`, the frame draws
  conventionally, and it matches the CPU reference.
- A pool with room for one meshlet refuses the second mesh with `OutOfRange`.
- A flipped plane with a one-sided material is culled by its cone, and a flipped plane with a two-sided material is kept.
  The visible count matches the CPU reference's count in both cases.

The CPU tests in `tests/src/render_meshlet_tests.cpp` cover validation (each rule has a rejecting case), the frustum and
cone reference, and the cooked data round trip.

Unsupported or not run: the native Linux Vulkan GPU suite (this WSL host has no Vulkan device), and any GUI check (no
window exists).
