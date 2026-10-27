[PRD]
# PRD: C++20 Open-World Game Engine

## Overview

Build an engine from scratch for single-player 3D open-world games, including a standalone runtime and an editor. Platforms: Windows x64 with DirectX 12 and Vulkan; Linux x64 with Vulkan. All first-party engine and tool code uses C++20. The ECS, windowing layer, and input system are implemented in-house. Required external components are Dear ImGui, Lua, and Jolt Physics.

The complete first version includes PBR, a meshlet/mesh shader pipeline, a GPU indirect pipeline, ray tracing, GI, volumetrics, temporal upscaling, animation, terrain, particles, world streaming, an asset converter, scene/material/animation/terrain editors, audio, game UI, saves, and standalone game distribution.

This document describes future implementation. Configuring CMake and downloading and adding dependencies to the repository are implementation tasks; they are not performed while preparing the PRD. All stories are mandatory for completing the full version.

## Goals

- Create, edit, save, and run a game without changing engine source code.
- Build the editor, runtime, asset converter, and tests through CMake on both operating systems.
- Store the sources of every library dependency, including transitive dependencies, directly in `third_party/`; build the project without network downloads.
- Provide rendering feature parity across Windows/DX12, Windows/Vulkan, and Linux/Vulkan, with explicit GPU capability checks.
- Achieve a p99 frame time no higher than 1000/60 ms on the user's RTX 5080 computer in two fixed Full HD profiles.
- Test subsystems through dedicated benchmark scenes and a combined scene that exercises compatible features together.
- Distinguish individual module readiness from completion of the full version: no mandatory feature may be replaced with a stub.

## Quality Gates

### Common Commands

US-001 establishes the initial verification infrastructure; US-002–US-004 extend it with dependencies, the shader toolchain, and CI. Starting with US-001, every story must pass the common checks available at its implementation stage. The commands below are a mandatory contract for the future project, not commands that already exist.

For every story on Windows:

```sh
cmake -P cmake/ValidateVendor.cmake
cmake --preset win-dev
cmake --build --preset win-debug
ctest --preset win-unit --output-on-failure --no-tests=error
```

For every story on Linux:

```sh
cmake -P cmake/ValidateVendor.cmake
cmake --preset linux-dev
cmake --build --preset linux-debug
ctest --preset linux-unit --output-on-failure --no-tests=error
```

`win-unit` and `linux-unit` include all CPU unit/integration tests labeled `unit` or `integration`; an empty smoke test alone does not count as functional coverage. First-party code compiles with warnings treated as errors; vendor code does not inherit this requirement.

CI for every story runs builds for both platforms. Development on one operating system is allowed, but a change cannot be considered verified on the other operating system until the corresponding job succeeds. WSL does not replace Windows and native Linux GPU verification.

For changes involving memory, ECS, jobs, streaming, Lua, or physics, also run:

```sh
cmake --preset linux-asan
cmake --build --preset linux-asan
ctest --preset linux-sanitize --output-on-failure --no-tests=error
```

This preset enables AddressSanitizer and UndefinedBehaviorSanitizer for CPU tests. A separate CI job builds first-party code with GCC using `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit`.

### GPU, Editor, and Performance

Once the corresponding subsystems exist, additional checks apply:

- `engine_tests --suite gpu --backend dx12` — Windows/DX12.
- `engine_tests --suite gpu --backend vulkan` — Windows/Vulkan and native Linux/Vulkan.
- `engine_tests --suite editor --backend <dx12|vulkan>` — automated editor scenarios.
- `engine_bench --suite acceptance --backend <dx12|vulkan> --output artifacts/bench/<run-id>` — complete performance acceptance.

Debug GPU runs use the D3D12 debug layer or Vulkan validation layers; lifetime, synchronization, resource state, and GPU validation errors cause failure. Performance is measured separately in Release without debug/validation overhead.

UI stories require verification in the running application whenever the environment permits, including screenshots, an action sequence, and a result log. If a GUI is unavailable, run automated checks of editor models, serialization, and commands, and record the limitation in the report. These checks do not establish visual readiness.

An unavailable GPU extension or missing GUI in development/CI is reported as `unsupported`/`not-run`, not `passed`. Completing the full version requires actual GPU reports for all three OS/backend combinations and a visual editor smoke test on both operating systems. An unavailable environment does not permit substituting a software renderer or WSL for those reports.

Completing the version also requires Release builds through `win-release`/`linux-release`, an offline build from a clean repository copy, standalone package verification, and the performance contract below. The full performance suite need not be repeated for every documentation change; run it for changes affecting performance and for final acceptance.

## User Stories

Dependencies determine implementation order. A story is completed once its own deliverable and checks are finished, without waiting for future dependent stories. Common quality commands are defined above and are not duplicated in acceptance criteria.

### US-001: CMake and Initial Checks
**Description:** As a developer, I want a reproducible build so that I can begin implementation on both operating systems.
**Depends on:** None.
**Acceptance Criteria:**
- [ ] Add the root `CMakeLists.txt`, the presets defined in Quality Gates, isolated build directories, and C++20 without compiler extensions for first-party code.
- [ ] Create minimal `engine_core`, `engine_runtime`, and `engine_tests` targets; include Catch2 sources in `third_party/` with its license and manifest entry.
- [ ] Implement `ValidateVendor.cmake`, the initial manifest, and real CLI tests for help, unknown arguments, and nonzero error exit codes.
- [ ] Configuration performs no network operations; a missing tool or vendor file produces a clear error.

### US-002: Library Dependency Sources
**Description:** As a developer, I want all dependencies delivered with the repository so that I can build offline.
**Depends on:** US-001.
**Acceptance Criteria:**
- [ ] Download every library in the dependency catalog below, pin its revision/hash, and include it as ordinary files in `third_party/`, together with required transitive sources.
- [ ] The manifest records URL, revision, content hash, license, patches, and parent/dependency relationships; validation detects modified or missing files.
- [ ] No required submodules, LFS downloads, package-manager downloads, or FetchContent downloads occur during configure/build.
- [ ] Dependencies on EnTT, Flecs, SDL, GLFW, and ready-made ECS/window/input wrappers are prohibited; system ABI exceptions are listed explicitly.

### US-003: Buildable Shader Toolchain
**Description:** As a developer, I want a local shader compiler so that I can produce both GPU formats offline.
**Depends on:** US-002.
**Acceptance Criteria:**
- [ ] Include DXC sources and required components in the vendor manifest; CMake builds the host toolchain separately from engine compilation flags.
- [ ] Compile one HLSL fixture into DXIL and SPIR-V; diagnostics include the source path and error line.
- [ ] The build uses the pinned toolchain instead of an arbitrary `dxc` from PATH; required runtime/validator components are included in the tool output.
- [ ] Reproduce a clean offline toolchain build on both operating systems; diagnose missing OS SDKs before beginning a large build.

### US-004: CI and Verification Reports
**Description:** As a developer, I want to check changes on both operating systems so that I can detect platform regressions.
**Depends on:** US-003.
**Acceptance Criteria:**
- [ ] Add Windows/MSVC, Linux/Clang, Linux/GCC, and CPU sanitizer jobs with CTest reports.
- [ ] Separate GPU jobs from CPU jobs and require a runner with a real GPU; explicitly retain the status of an unavailable environment.
- [ ] Offline build verification disables networking and starts with an empty build directory; a cache miss does not trigger a download.

### US-005: Core Runtime Services
**Description:** As a developer, I want shared services so that subsystems behave consistently on both operating systems.
**Depends on:** US-002.
**Acceptance Criteria:**
- [ ] Provide logging, assertions, a monotonic clock, GUIDs, normalized UTF-8 paths, and testable file IO/result APIs.
- [ ] Define meters, seconds, Y-up, handedness, quaternion/matrix conventions, and common clip-space conversion rules.
- [ ] Fixture tests cover IO errors, long paths, and Cyrillic text; the runtime does not require the editor.

### US-006: Jobs and Controlled Shutdown
**Description:** As a developer, I want background task execution so that I do not block the main loop.
**Depends on:** US-005.
**Acceptance Criteria:**
- [ ] The in-house job system supports dependencies, task groups, cancellation, and completion waits.
- [ ] Shutdown waits for or cancels tasks without accessing destroyed subsystems.
- [ ] Tests cover dependencies, task failures, cancellation, and repeated startup/shutdown.

### US-007: In-House Windows Windowing
**Description:** As a Windows user, I want a native window so that I can run the editor and game.
**Depends on:** US-005.
**Acceptance Criteria:**
- [ ] The Win32 implementation creates multiple windows and handles close, resize, minimize/restore, focus, and DPI.
- [ ] Native handles are passed to DX12/Vulkan without SDL/GLFW; framebuffer dimensions and logical window dimensions are distinct.
- [ ] Zero size and closing during resize do not cause hangs or access to a destroyed window.

### US-008: In-House Linux Windowing
**Description:** As a Linux user, I want a native window so that I can run the editor and game.
**Depends on:** US-005.
**Acceptance Criteria:**
- [ ] The in-house X11 backend implements the common window API, events, multiple windows, and Vulkan surface handles.
- [ ] Verify native X11 and XWayland; report a missing display while keeping CPU headless mode available.
- [ ] Resize, focus, close, and framebuffer-size semantics match the Windows contract.

### US-009: In-House Windows Input
**Description:** As a player, I want Windows input so that I can control the game without a third-party input layer.
**Depends on:** US-007.
**Acceptance Criteria:**
- [ ] Win32 Raw Input/keyboard/text events and XInput provide keyboard, mouse, UTF-8 text, and gamepad input.
- [ ] Support relative mouse input, wheel events, cursor capture, gamepad hotplug, and clearing held states on focus loss.
- [ ] Test replay verifies pressed/released/held states, Unicode, and the absence of duplicate events within one frame.

### US-010: In-House Linux Input
**Description:** As a player, I want Linux input through the same API so that game controls remain consistent.
**Depends on:** US-008, US-009.
**Acceptance Criteria:**
- [ ] X11/XInput2/XIM and evdev provide keyboard, mouse, text, and gamepad input without a ready-made input library.
- [ ] Support relative mouse input, focus-loss reset, dead zones, and hotplug; diagnose missing device permissions.
- [ ] Shared input replay fixtures produce equivalent normalized events on both operating systems.

### US-011: Actions and Input Routing
**Description:** As a game creator, I want to bind actions to devices so that I can change controls without code.
**Depends on:** US-010.
**Acceptance Criteria:**
- [ ] Action maps support buttons, axes, remapping, dead zones, and a saved user profile.
- [ ] Editor/UI capture prevents captured events from reaching gameplay; switching Play releases capture.
- [ ] Automated scenarios verify simultaneous devices and action-map changes.

### US-012: ECS Handles and Lifetime
**Description:** As a developer, I want in-house entity handles so that I can safely reference game objects.
**Depends on:** US-005.
**Acceptance Criteria:**
- [ ] An entity handle contains an index/generation; create/destroy/reuse does not revive a stale handle.
- [ ] Worlds are isolated; external scene GUIDs are independent of ECS indices.
- [ ] Tests cover repeated destruction, bulk creation, and access to an entity belonging to another world.

### US-013: ECS Storage and Queries
**Description:** As a developer, I want in-house component pools and queries so that I can process data without an external ECS.
**Depends on:** US-012.
**Acceptance Criteria:**
- [ ] Sparse-set storage supports add/remove/get operations and query intersections across multiple components.
- [ ] Correctly handle alignment, move-only components, destructors, and empty queries.
- [ ] Query order is not an implicit save-format contract; tests compare result membership.

### US-014: ECS Systems and Deferred Changes
**Description:** As a developer, I want controlled system ordering so that I can avoid races during structural changes.
**Depends on:** US-006, US-013.
**Acceptance Criteria:**
- [ ] Systems declare read/write access and dependencies; conflicting writes do not execute in parallel.
- [ ] Structural changes during iteration are applied through a command buffer at defined synchronization points.
- [ ] Verify conflicts, ordering, frame-job cancellation, and entity invalidation before command application.

### US-015: Component Reflection
**Description:** As a tool author, I want component metadata so that I can build the inspector and serialization.
**Depends on:** US-013.
**Acceptance Criteria:**
- [ ] In-house descriptors define stable type IDs, fields, defaults, constraints, and schema versions.
- [ ] Register a user-defined component without changing editor code.
- [ ] Automatically detect duplicate type IDs and invalid defaults.

### US-016: Scene Graph and Transforms
**Description:** As a scene author, I want an object hierarchy so that I can edit related transforms.
**Depends on:** US-014, US-015.
**Acceptance Criteria:**
- [ ] Provide local/world transforms, dirty propagation, and reparenting with optional world-transform preservation.
- [ ] Reject cyclic hierarchies; deleting a parent has an explicit policy for children.
- [ ] Verify scaling, rotation, deep hierarchies, and transforms near large coordinates.

### US-017: Scene Format and Prefabs
**Description:** As a game creator, I want to save scenes and reuse object groups.
**Depends on:** US-016.
**Acceptance Criteria:**
- [ ] The versioned scene format stores GUIDs, components, hierarchy, and asset GUID references; writes are atomic.
- [ ] Prefabs support instantiation and local component overrides without fully duplicating the source.
- [ ] Round trips, missing references, and unknown schemas produce testable outcomes without partially destroying the scene.

### US-018: Asset Registry
**Description:** As a game creator, I want stable resource references so that renaming does not break scenes.
**Depends on:** US-005, US-015.
**Acceptance Criteria:**
- [ ] The registry stores GUIDs, source paths, importer settings, content hashes, and a dependency graph.
- [ ] Moving a source together with its metadata preserves its GUID; reject GUID conflicts.
- [ ] A fixture project verifies deletion, missing dependencies, and renaming.

### US-019: Asset Converter CLI
**Description:** As a developer, I want a command-line converter so that I can prepare resources without the editor.
**Depends on:** US-006, US-018.
**Acceptance Criteria:**
- [ ] `asset_converter` supports import/cook/validate and a machine-readable report with error codes.
- [ ] Incremental conversion accounts for settings, tool versions, and transitive dependencies; identical inputs produce identical output hashes.
- [ ] Parallel tasks do not overwrite shared output; a failed import does not replace a previous valid result.

### US-020: Model Import
**Description:** As a scene author, I want to import common model formats so that I can use existing assets.
**Depends on:** US-019.
**Acceptance Criteria:**
- [ ] An Assimp adapter imports glTF/GLB, FBX, and OBJ: meshes, hierarchy, UVs, normals, tangents, and available materials.
- [ ] Convert scale/axes to engine conventions; report unsupported data in the import report.
- [ ] Each format has a fixture verifying geometry and transforms; incomplete materials do not cause crashes.

### US-021: Texture Conversion
**Description:** As a material author, I want prepared GPU textures so that the runtime can load resources quickly.
**Depends on:** US-019.
**Acceptance Criteria:**
- [ ] Import PNG/JPEG and generate mipmaps using distinct rules for color, normal, and linear data.
- [ ] A common cooked format contains metadata and BC7 color/BC5 normal data or an explicit RGBA fallback usable by both backends.
- [ ] Verify sRGB, normal renormalization, odd dimensions, corrupt files, and the correct size of every mip.

### US-022: Skeleton and Animation Clip Import
**Description:** As a character author, I want to import animations so that I can play them in the game.
**Depends on:** US-020.
**Acceptance Criteria:**
- [ ] Convert glTF/GLB and supported FBX fixtures into skeletons, skin weights, and translation/rotation/scale tracks.
- [ ] Validate bone indices, inverse bind transforms, weight normalization, and incompatible skeletons.
- [ ] Support looping, playback speed, and clip events; verify timing and interpolation against reference poses.

### US-023: LOD and Meshlet Cooking
**Description:** As a rendering developer, I want prepared geometry so that I can use multiple GPU paths.
**Depends on:** US-020.
**Acceptance Criteria:**
- [ ] The converter creates indexed geometry, a LOD chain, bounds, and meshlets containing at most 64 vertices/124 triangles.
- [ ] Use meshoptimizer as an asset-processing dependency; the runtime scene/ECS remain in-house.
- [ ] Verify indices, bounds, and correspondence to source geometry; explicitly handle small and degenerate meshes.

### US-024: Runtime Asset Handles
**Description:** As a game developer, I want asynchronous resource handles so that loading does not block a frame.
**Depends on:** US-019.
**Acceptance Criteria:**
- [ ] Handles have loading/ready/failed states, reference lifetime, and a fallback for assets that are not ready.
- [ ] Jobs load cooked resources, cancel on world closure, and never publish partial data.
- [ ] Tests cover duplicate requests, unload/reload, format errors, and consumer destruction during loading.

### US-025: Changed Asset Updates
**Description:** As a game creator, I want to see source changes so that I can iterate quickly.
**Depends on:** US-024.
**Acceptance Criteria:**
- [ ] Windows/Linux file watching triggers debounced incremental conversion of dependent assets.
- [ ] Replace valid results at a safe point; errors preserve the previous working version and display diagnostics.
- [ ] Renames, bursts of changes, and dependency deletion do not cause an infinite rebuild loop.

### US-026: In-House RHI Contract
**Description:** As a rendering developer, I want a shared engine GPU API so that I can support two backends.
**Depends on:** US-005.
**Acceptance Criteria:**
- [ ] The RHI describes devices, queues, buffers/textures, descriptors, pipelines, commands, fences, and timestamp queries.
- [ ] Lifetime and resource transitions are explicit; backend-specific handles do not leak into scene/ECS APIs.
- [ ] The capability report distinguishes mesh shaders, indirect count, and ray tracing; tests can forcibly disable features.

### US-027: DX12 Device and Presentation
**Description:** As a Windows user, I want DX12 output so that I can run the engine through DirectX.
**Depends on:** US-007, US-026.
**Acceptance Criteria:**
- [ ] Implement adapter selection, device/queues, swapchain, frame fences, and present.
- [ ] Correctly handle resize/minimize/restore and GPU shutdown; settings control presentation policy and VSync.
- [ ] The debug device reports backend/adapter/capabilities; a GPU smoke test renders and reads back a reference clear color.

### US-028: Vulkan Device and Presentation
**Description:** As a user of both operating systems, I want Vulkan output so that I can use a shared alternative backend.
**Depends on:** US-007, US-008, US-026.
**Acceptance Criteria:**
- [ ] Implement instance/device, Win32/X11 surfaces, queues, swapchain, and synchronization.
- [ ] Handle out-of-date/suboptimal swapchains, resize, and surface loss without corrupting resources.
- [ ] The same clear/readback fixture passes on Windows and native Linux with validation layers.

### US-029: DX12 Resources and Descriptors
**Description:** As a rendering developer, I want DX12 resource management so that I can safely transfer data to the GPU.
**Depends on:** US-027.
**Acceptance Criteria:**
- [ ] Implement upload/readback, texture/buffer allocation, descriptor pools, and fence-based deferred destruction.
- [ ] Graphics/compute/copy work uses explicit transitions and queue synchronization.
- [ ] GPU round trips and repeated allocation/deallocation produce no validation errors or growth in live resources.

### US-030: Vulkan Resources and Descriptors
**Description:** As a rendering developer, I want the same resource model in Vulkan so that the backends have parity.
**Depends on:** US-028, US-029.
**Acceptance Criteria:**
- [ ] Implement allocation, staging, descriptor lifetime, layout transitions, and synchronization2.
- [ ] Validate capabilities against required bindless/indirect operations before creating a pipeline.
- [ ] Common resource fixtures pass on both Vulkan hosts; deallocation accounts for unfinished GPU work.

### US-031: Shaders, Reflection, and PSO Cache
**Description:** As a material developer, I want a common shader pipeline so that I can run HLSL consistently on both APIs.
**Depends on:** US-003, US-029, US-030.
**Acceptance Criteria:**
- [ ] Compile HLSL into the required target; reflection validates bindings and CPU/GPU layouts.
- [ ] Cache keys include source/includes/defines/compiler/backend/device-relevant metadata; incompatible PSO caches are not silently accepted.
- [ ] Shader reload preserves the last valid PSO; compile errors are available in machine-readable and text formats.

### US-032: Render Graph
**Description:** As a rendering developer, I want to describe pass dependencies so that I can automate resource synchronization.
**Depends on:** US-031.
**Acceptance Criteria:**
- [ ] Passes declare reads/writes, transient resources, and graphics/compute usage; the graph computes ordering and barriers.
- [ ] Reject cycles, read-before-write, and incompatible usage with the names of affected passes.
- [ ] Graph export and CPU/GPU timings are available for diagnostics; lifetime tests pass on both APIs.

### US-033: Scene Rendering and Picking
**Description:** As a scene author, I want to see objects and select them in the viewport.
**Depends on:** US-016, US-023, US-024, US-032.
**Acceptance Criteria:**
- [ ] Render extraction creates an immutable frame snapshot without retaining ECS pointers beyond the synchronization point.
- [ ] Cameras, indexed meshes, depth testing, frustum culling, and entity-ID picking work consistently on both APIs.
- [ ] Golden fixtures verify transforms, depth, and picking results after entity deletion.

### US-034: PBR and Transparency
**Description:** As a material author, I want physically based materials so that I can control object appearance.
**Depends on:** US-021, US-033.
**Acceptance Criteria:**
- [ ] Provide metallic/roughness PBR, normal/emissive/occlusion maps, IBL, and material instances.
- [ ] Support opaque, alpha mask, alpha blend, and two-sided rendering; transparency has a documented ordering policy.
- [ ] Reference spheres/planes verify roughness/metallic extremes, color spaces, and alpha edges.

### US-035: Lighting and Raster Shadows
**Description:** As a scene author, I want lighting and shadows so that I can create readable environments.
**Depends on:** US-034.
**Acceptance Criteria:**
- [ ] Process directional/point/spot lights through clustered/tiled light lists with overflow control.
- [ ] Directional cascaded shadows and a local shadow atlas support bias, filtering, and invalidation.
- [ ] Light/shadow debug views and fixtures reveal incorrect bounds, shadow acne, and missing lights.

### US-036: Playback and Skinning
**Description:** As a character author, I want animated meshes so that I can bring the scene to life.
**Depends on:** US-022, US-034.
**Acceptance Criteria:**
- [ ] The runtime computes poses, blends two clips, supports configurable root motion, and performs GPU skinning.
- [ ] Current/previous transforms produce motion vectors for skinned geometry.
- [ ] Bounds account for animation; fixtures verify the reference pose and transitions across loop boundaries.

### US-037: GPU Indirect Pipeline
**Description:** As a rendering developer, I want GPU-generated draw commands so that I can reduce CPU overhead.
**Depends on:** US-035, US-036.
**Acceptance Criteria:**
- [ ] Compute culling/LOD generates indirect command/count buffers for static and skinned instances.
- [ ] DX12 ExecuteIndirect and Vulkan draw-indirect-count execute the same logical workload; an indexed fallback is available.
- [ ] Verify empty/overflow buffers, disabled culling, and visible-object agreement with the reference path.

### US-038: Mesh Shader Pipeline
**Description:** As a rendering developer, I want meshlet rendering so that I can use a modern GPU geometry pipeline.
**Depends on:** US-023, US-037.
**Acceptance Criteria:**
- [ ] DX12 amplification/mesh and Vulkan task/mesh shaders use cooked meshlets and GPU culling.
- [ ] Determine support through capabilities; static geometry switches between meshlet and indirect-indexed paths without reconverting the source.
- [ ] The fallback works with mesh shaders forcibly disabled; silhouettes/depth match within the specified tolerance.

### US-039: HDR, TAA, and Postprocessing
**Description:** As a player, I want stable imagery so that motion does not cause flickering or incorrect exposure.
**Depends on:** US-035, US-036.
**Acceptance Criteria:**
- [ ] Implement linear HDR render targets, exposure, bloom, tone mapping, and TAA with motion vectors.
- [ ] Camera cuts, resize, and teleport reset temporal history; jitter is consistent across both APIs.
- [ ] Verify the absence of NaN/Inf, history on a moving object, and a reference output overlay's independence from scene exposure; game UI is connected in US-058.

### US-040: Native-Profile GI and Reflections
**Description:** As a player, I want indirect lighting without heavy RT so that I can use the native profile.
**Depends on:** US-019, US-039.
**Acceptance Criteria:**
- [ ] The converter prepares diffuse irradiance probes; the runtime interpolates them and uses IBL/SSR for reflections.
- [ ] Provide fallbacks for empty/incomplete probes and off-screen SSR; version GI data and invalidate it when sources change.
- [ ] Indoor/outdoor fixtures demonstrate indirect lighting; a debug view shows probe coverage.

### US-041: DX12 Ray Tracing Foundation
**Description:** As a rendering developer, I want DXR acceleration structures so that I can implement RT effects.
**Depends on:** US-036, US-037.
**Acceptance Criteria:**
- [ ] Create BLAS/TLAS, shader tables, and ray pipelines with explicit capability checks.
- [ ] Track transform changes, skinned geometry, and unloading through build/refit/lifetime management.
- [ ] A hit/miss fixture and object deletion during an unfinished GPU frame do not cause invalid access.

### US-042: Vulkan Ray Tracing Foundation
**Description:** As a rendering developer, I want RT in Vulkan so that both APIs retain feature parity.
**Depends on:** US-030, US-041.
**Acceptance Criteria:**
- [ ] KHR acceleration structures/ray tracing pipelines implement the common RT contract and required feature checks.
- [ ] Validate shader binding tables, alignment, and build/update synchronization with validation layers.
- [ ] Common hit/miss/skinned/unload fixtures produce equivalent results on Windows and Linux.

### US-043: RT Shadows and Reflections
**Description:** As a player, I want RT shadows and reflections so that I can see dynamic geometry outside screen space.
**Depends on:** US-041, US-042.
**Acceptance Criteria:**
- [ ] Provide ray-traced directional shadows and single-bounce reflections with a roughness cutoff and limited distance.
- [ ] Results account for alpha-mask geometry; list unsupported geometry classes in the capability report.
- [ ] Reference scenes verify off-screen reflections, a moving occluder, and the difference between RT on/off.

### US-044: Dynamic RT GI
**Description:** As a player, I want dynamic indirect lighting so that it responds to scene changes.
**Depends on:** US-040, US-043.
**Acceptance Criteria:**
- [ ] In-house probe-grid GI uses ray tracing, temporal accumulation, and a probe-update budget.
- [ ] The grid follows the camera, responds to changed lighting, and resets incompatible history during streaming/teleport.
- [ ] A moving-light test verifies diffuse-bounce changes and the absence of infinite/NaN values.

### US-045: Denoising and Volumetrics
**Description:** As a player, I want stable RT effects and volumetric light so that the full profile is usable for gameplay.
**Depends on:** US-039, US-043, US-044.
**Acceptance Criteria:**
- [ ] An in-house temporal/spatial denoiser uses depth, normals, and motion vectors, with disocclusion/history rejection.
- [ ] Froxel volumetric fog supports directional/local lighting, shadowing, and temporal stabilization.
- [ ] Camera cuts and moving lights do not leave indefinitely persistent history; effects are visible and measured as separate GPU passes.

### US-046: Temporal Upscaling
**Description:** As a player, I want upscaling so that I can run the full profile at a lower internal resolution.
**Depends on:** US-003, US-039, US-045.
**Acceptance Criteria:**
- [ ] Integrate vendored FSR 2 into both backends; supply depth, exposure, jitter, motion vectors, and a reactive mask according to its contract.
- [ ] Native and full profiles use fixed resolutions; compose UI after upscaling at output resolution.
- [ ] Resize/camera cuts reset history; benchmarks do not enable frame generation or dynamically reduce quality.

### US-047: Terrain Runtime
**Description:** As an open-world author, I want terrain so that I can create large landscapes.
**Depends on:** US-023, US-035, US-037, US-042, US-043.
**Acceptance Criteria:**
- [ ] Heightfield terrain is divided into tiles and supports LOD/stitching, normals, and splat blending of four materials.
- [ ] Far LOD and detailed tiles have consistent boundaries; terrain participates in depth, shadows, and RT geometry in the full profile.
- [ ] A fixture verifies tile boundaries, the absence of holes, and correct LOD switching.

### US-048: World Partition Data
**Description:** As a world author, I want spatial partitioning so that I can store and prepare large scenes.
**Depends on:** US-017, US-018, US-047.
**Acceptance Criteria:**
- [ ] A world is described by grid cells with stable GUIDs, bounds, asset dependencies, and a separate always-loaded layer.
- [ ] The converter prepares cell manifests and distant terrain LODs; an object on a boundary has a single owner.
- [ ] Verify cross-cell references, moving an object across a boundary, and saving modified cells.

### US-049: Asynchronous World Streaming
**Description:** As a player, I want the world to stream as I move so that transitions between locations do not stop gameplay.
**Depends on:** US-006, US-024, US-048.
**Acceptance Criteria:**
- [ ] The streaming manager schedules prefetch/load/activate/deactivate/unload with CPU IO/decode and GPU upload budgets.
- [ ] Activation occurs at safe points; verify the common cell lifecycle with a render consumer and test subscribers for future physics/script adapters.
- [ ] Rapid direction changes, teleport, cancellation, and missing cells do not cause use-after-free; measure transient residency.

### US-050: Large-World Coordinates
**Description:** As a player, I want stability far from the origin so that the open world does not jitter.
**Depends on:** US-016, US-049.
**Acceptance Criteria:**
- [ ] Scene/world coordinates retain sufficient precision; rendering uses camera-relative floats.
- [ ] Origin rebasing is consistent with streaming and temporal history; verify the common physics-adapter event contract with a test subscriber.
- [ ] A route across rebasing boundaries preserves transforms, picking, and stable scene references; actual physics queries are connected and verified in US-053.

### US-051: GPU Particles
**Description:** As an effects author, I want a particle system so that I can add dynamic effects to the world.
**Depends on:** US-037, US-039.
**Acceptance Criteria:**
- [ ] GPU emit/simulate/render supports lifetime, velocity, gravity, color/size curves, and textured billboards.
- [ ] Provide alpha/additive modes, depth interaction, bounds, and indirect draw; resource overflow is bounded by a budget.
- [ ] A seeded fixture is reproducible within one backend; pause, reset, and emitter deletion release resources.

### US-052: Jolt World and Fixed Timestep
**Description:** As a game developer, I want a physics world so that I can simulate rigid bodies.
**Depends on:** US-002, US-014, US-016.
**Acceptance Criteria:**
- [ ] A Jolt adapter creates a separate world for each runtime scene and performs fixed updates at 60 Hz with interpolated render transforms.
- [ ] Static/dynamic/kinematic bodies reference stable entity handles; callbacks are published through a safe queue.
- [ ] Verify pause, the catch-up limit, and world closure without depending on the renderer.

### US-053: Colliders, Queries, and Terrain Physics
**Description:** As a game creator, I want collisions and spatial queries so that objects can interact with the world.
**Depends on:** US-047, US-049, US-050, US-052.
**Acceptance Criteria:**
- [ ] Box/sphere/capsule/convex/mesh/heightfield colliders support filters, triggers, raycasts, and shape casts.
- [ ] Streamed terrain/scene cells add and remove collision geometry consistently with activation; premature unloading does not cause dynamic bodies to fall through.
- [ ] Hit results preserve entity identity; tests cover filters, triggers, collider replacement, and queries before/after origin rebasing.

### US-054: Character and Constraints
**Description:** As a game creator, I want a controllable character and joints so that I can build basic gameplay.
**Depends on:** US-011, US-053.
**Acceptance Criteria:**
- [ ] The character controller supports movement, slope limits, steps, jumping, and moving platforms.
- [ ] Provide fixed/hinge/distance constraints with parameters and safe deletion of connected bodies.
- [ ] A fixture verifies stairs, slopes, triggers, and constraints after cell unload/reload.

### US-055: Engine Lua API
**Description:** As a game creator, I want to write gameplay in Lua so that I do not need to rebuild C++ for every change.
**Depends on:** US-011, US-015, US-024, US-053.
**Acceptance Criteria:**
- [ ] Integrate Lua through in-house C API bindings for entities/components, transforms, input actions, assets, physics queries, and events.
- [ ] Accessing a destroyed entity produces a controlled script error; C++ pointers are not retained as unsafe user handles.
- [ ] Headless tests verify stack balance, type errors, and Lua exceptions.

### US-056: Lua Lifecycle and Reload
**Description:** As a game creator, I want a controlled script lifecycle so that I can run and update gameplay.
**Depends on:** US-025, US-049, US-055.
**Acceptance Criteria:**
- [ ] Script components invoke create/start/update/fixed_update/destroy in a documented order.
- [ ] Errors are isolated to the affected script instance and shown in the console; reload preserves explicitly exported state or resets it according to settings.
- [ ] Repeated Play/Stop and streaming leave no subscriptions or Lua references to destroyed worlds.

### US-057: Audio
**Description:** As a game creator, I want audio so that I can add sound to the game world.
**Depends on:** US-024, US-056.
**Acceptance Criteria:**
- [ ] Provide WAV/OGG import, 2D/3D sources, a listener, attenuation, looping, streamed music, and master/music/SFX buses.
- [ ] Lua controls playback; pause/world unload releases voices without audio-thread access to the ECS.
- [ ] Offline mixing tests verify gain/pan/lifetime; native smoke tests check the device and clear behavior when it is unavailable.

### US-058: Game UI
**Description:** As a game creator, I want a game interface so that I can display a HUD and menus.
**Depends on:** US-011, US-034, US-046, US-056.
**Acceptance Criteria:**
- [ ] The in-house runtime UI supports canvas/anchors, images, UTF-8 text including Cyrillic, buttons, sliders, and Lua callbacks.
- [ ] Hit testing, focus, navigation, UI scaling, and input capture work on both operating systems; the UI does not require the editor/ImGui.
- [ ] Compose UI after scene postprocessing; fixtures verify layout and interactions at different window sizes.

### US-059: Game Saves
**Description:** As a player, I want to save progress so that I can continue after restarting the game.
**Depends on:** US-017, US-049, US-056.
**Acceptance Criteria:**
- [ ] Save slots store selected gameplay components, Lua state, and world deltas using stable GUIDs.
- [ ] Include schema/build metadata, atomic writes, and rejection of corrupt/incompatible saves without losing the previous save.
- [ ] Save/load round trips restore changed unloaded cells and do not serialize GPU/physics pointers.

### US-060: Standalone Runtime and Resource Packs
**Description:** As a game creator, I want to run the game separately so that I can distribute it without the editor.
**Depends on:** US-019, US-049, US-057, US-058, US-059.
**Acceptance Criteria:**
- [ ] The runtime accepts project/start-scene/backend/profile arguments, uses cooked resource packs, and does not link to the editor/importer.
- [ ] Packs contain an index, versions, dependency closure, and integrity checks; a missing mandatory dependency aborts packaging.
- [ ] The game starts from a different working directory without source assets, the build tree, or an installed editor.

### US-061: ImGui Editor Shell
**Description:** As a game creator, I want an editor with panels so that I can work on a project in one application.
**Depends on:** US-010, US-031, US-033.
**Acceptance Criteria:**
- [ ] Integrate Dear ImGui docking through an in-house platform/input adapter and renderer integration for both backends.
- [ ] Provide project open/create, docking layouts, a console, errors, and saved workspace settings.
- [ ] The editor remains usable with an invalid project or missing asset; Cyrillic text renders correctly.

### US-062: Scene Hierarchy and Inspector
**Description:** As a scene author, I want to manage objects and components so that I can edit the game world.
**Depends on:** US-015, US-017, US-061.
**Acceptance Criteria:**
- [ ] The hierarchy supports create/delete/duplicate/reparent/search and synchronizes selection with the inspector.
- [ ] The inspector is generated from reflection, adds/removes components, and validates field input.
- [ ] Save/open and the unsaved-changes dialog work; missing assets display their GUID and an error instead of crashing.

### US-063: Viewport and Gizmos
**Description:** As a scene author, I want to edit transforms in the viewport so that I can place objects visually.
**Depends on:** US-011, US-033, US-062.
**Acceptance Criteria:**
- [ ] Provide a free editor camera, entity picking, translate/rotate/scale gizmos, local/world modes, and snapping.
- [ ] Viewport resizing accounts for DPI; the camera and selection are consistent with world coordinates.
- [ ] Dragging a gizmo does not control the gameplay camera; selection remains valid after deletion and reparenting.

### US-064: Undo/Redo
**Description:** As a scene author, I want to undo changes so that I can edit the project safely.
**Depends on:** US-062, US-063.
**Acceptance Criteria:**
- [ ] Command history supports create/delete, reparenting, component edits, and merging a drag into one operation.
- [ ] Undo/redo restores GUIDs/references, updates dirty state correctly, and clears redo after a new command.
- [ ] An automated edit-and-full-rollback scenario restores the original serialized document.

### US-065: Play, Pause, Step, Stop
**Description:** As a game creator, I want to test scenes inside the editor so that I can verify gameplay quickly.
**Depends on:** US-054, US-056, US-058, US-064.
**Acceptance Criteria:**
- [ ] Play creates a separate runtime world from the editor snapshot; Stop restores edit state without carrying over runtime changes.
- [ ] Pause/Step coordinates physics, scripts, animation, particles, and audio; input is routed to the appropriate viewport.
- [ ] After 20 Play/Stop cycles, no runtime entities, subscriptions, or active voices remain, and GPU live-resource counts do not grow.

### US-066: Asset Browser and Import UI
**Description:** As a game creator, I want to manage assets in the editor so that I do not need the CLI for every operation.
**Depends on:** US-025, US-061, US-064.
**Acceptance Criteria:**
- [ ] Provide a catalog, search, previews, import settings, progress/cancel, and clear import errors.
- [ ] Dragging a model/material/texture into the viewport or inspector creates correct GUID references.
- [ ] Rename/delete shows affected references; hot reload updates previews without losing selection.

### US-067: Material Graph Compiler
**Description:** As a material author, I want a material graph so that I can define shaders without writing HLSL manually.
**Depends on:** US-031, US-034.
**Acceptance Criteria:**
- [ ] The versioned DAG supports constants/parameters, arithmetic, UVs, texture sampling, normal maps, and a PBR output.
- [ ] Type checking/cycle detection produces node-specific errors; compile the graph into both shader targets and account for material variants.
- [ ] Changing an instance parameter does not require a shader rebuild; topology changes invalidate the correct cache keys.

### US-068: Visual Material Editor
**Description:** As a material author, I want to edit nodes visually so that I can see results immediately.
**Depends on:** US-064, US-066, US-067.
**Acceptance Criteria:**
- [ ] The canvas supports nodes, pins, links, selection, undo/redo, and saving graph layouts.
- [ ] A preview mesh displays the material with working parameters; errors highlight nodes and preserve the last valid version.
- [ ] A graph round trip preserves semantics; the cooked runtime does not depend on the node-editor UI.

### US-069: Animation State Machine Runtime
**Description:** As a character author, I want a state graph so that I can connect animation to gameplay.
**Depends on:** US-036, US-056.
**Acceptance Criteria:**
- [ ] A versioned graph contains states, clips, float/bool/trigger parameters, conditions, and timed blend transitions.
- [ ] Lua changes parameters; entry/default states and the priority of simultaneous transitions are explicit.
- [ ] Fixed-timestep fixtures verify transitions, loop events, and root motion.

### US-070: Animation Graph Editor
**Description:** As a character author, I want to configure states visually so that I do not need to edit the graph manually.
**Depends on:** US-064, US-066, US-069.
**Acceptance Criteria:**
- [ ] The editor creates states/transitions, edits conditions, and selects compatible clips/skeletons.
- [ ] Preview shows the current state, blend, and timeline; the Play-mode parameter monitor reflects Lua changes.
- [ ] An automated scenario verifies undo/redo, save/reopen, and incompatible-skeleton errors.

### US-071: Terrain Sculpting and Painting
**Description:** As a world author, I want to edit terrain with brushes so that I can create landscapes in the editor.
**Depends on:** US-047, US-053, US-064, US-066.
**Acceptance Criteria:**
- [ ] Raise/lower/smooth/flatten and four-layer splat-paint brushes provide radius, strength, and falloff.
- [ ] Editing across tile boundaries reconciles heights/normals, invalidates LOD/RT geometry, and safely updates the physics collider.
- [ ] A stroke is one undo command; save/reopen and subsequent cooking reproduce the result.

### US-072: World Partition Editor
**Description:** As a world author, I want to see cells and residency so that I can manage a large level.
**Depends on:** US-049, US-063, US-071.
**Acceptance Criteria:**
- [ ] Provide a cell map with states, load/pin/unload controls, and camera navigation to a selected cell.
- [ ] Moving objects between cells preserves GUIDs, references, and dirty state; invisible cells do not lose changes.
- [ ] A debug overlay shows streaming budgets and residency; the editor camera does not activate gameplay scripts outside Play.

### US-073: Game Distribution from the Editor
**Description:** As a game creator, I want to build a distributable game so that I can deliver it to users.
**Depends on:** US-060, US-066, US-072.
**Acceptance Criteria:**
- [ ] Build Game runs validation, dependency closure, cooking, and packaging with progress and cancellation.
- [ ] A native Windows build produces a Windows package, and a native Linux build produces a Linux package; cross-compilation is not promised.
- [ ] The package includes shaders, runtime dependencies, settings, resource packs, and notices; verify launch from a clean directory.

### US-074: Benchmark Harness
**Description:** As a developer, I want reproducible measurements so that I can verify the performance contract.
**Depends on:** US-032, US-060.
**Acceptance Criteria:**
- [ ] `engine_bench` uses fixed seeds, scenes, routes, profiles, warmup, duration, and repeat counts from versioned manifests.
- [ ] Reports include raw frame timings, p50/p95/p99/max, CPU/GPU passes, residency, counters, and hardware/software metadata.
- [ ] Exit codes reflect thresholds and missing capabilities; absent workloads or incomplete runs do not count as success.

### US-075: Reproducible Test Assets
**Description:** As a developer, I want accessible test resources so that any runner can reproduce the scenes.
**Depends on:** US-019, US-074.
**Acceptance Criteria:**
- [ ] Generate geometry/terrain/some animations from a seed; include required file-format fixtures and audio in the repository with provenance and licenses.
- [ ] The generator creates the workload from the benchmark table and a manifest of expected counts/hashes.
- [ ] Generation and execution require no private assets, external accounts, or downloads.

### US-076: Render Benchmark
**Description:** As a rendering developer, I want a dedicated GPU workload so that I can compare rendering paths.
**Depends on:** US-038, US-046, US-051, US-075.
**Acceptance Criteria:**
- [ ] The `render_stress` scene matches the counts and route in the benchmark contract.
- [ ] Run indexed/indirect/meshlet paths and both profiles separately; record screenshots and submitted/visible counters.
- [ ] Forcibly disabling mesh shaders proves the fallback works; disabled features are not reported as enabled.

### US-077: Physics Benchmark
**Description:** As a physics developer, I want a dedicated workload so that I can observe simulation cost.
**Depends on:** US-054, US-075.
**Acceptance Criteria:**
- [ ] The `physics_stress` scene creates the specified number of active bodies, constraints, and characters and follows a fixed action sequence.
- [ ] Reports distinguish active/sleeping bodies, simulation steps, queries, and catch-up events.
- [ ] The test detects lost timesteps, NaN transforms, and objects leaving reference bounds.

### US-078: Streaming Benchmark
**Description:** As a world developer, I want a streaming workload so that I can identify stalls and leaks.
**Depends on:** US-050, US-053, US-075.
**Acceptance Criteria:**
- [ ] The `streaming_stress` scene uses the specified world, a route across cell boundaries, and a separate teleport correctness case.
- [ ] Record loaded/unloaded cells, IO/decode/upload time, and memory budgets together with frame timings.
- [ ] Residency stabilizes during an additional 30-minute soak; no terrain goes missing and no dangling cross-cell references appear.

### US-079: Combined Feature Scene
**Description:** As an engine author, I want to see all features together so that I can verify their interactions.
**Depends on:** US-065, US-068, US-070, US-073, US-076, US-077, US-078.
**Acceptance Criteria:**
- [ ] `all_features` combines the workload from the table, Lua gameplay, audio, UI, save/load, animation graphs, and cooked material graphs.
- [ ] An automated tour verifies feature markers and outputs; profiles test their corresponding GI/RT/upscale modes, and forced runs test alternative geometry paths.
- [ ] An editor scenario performs scene/material/animation/terrain edits, undo/redo, Play/Stop, and package/relaunch on this project.

### US-080: RTX 5080 Performance Acceptance
**Description:** As a user, I want stable 60 FPS so that I can use the engine on my computer.
**Depends on:** US-079.
**Acceptance Criteria:**
- [ ] All performance scenes and both profiles pass the measurement contract on the RTX 5080 for Windows/DX12, Windows/Vulkan, and native Linux/Vulkan.
- [ ] Raw reports contain three repeats for every combination; settings, workload, and thresholds were not automatically reduced to achieve a pass.
- [ ] When a budget is exceeded, identify and fix the measured cause; changing the contract requires a separate user decision.

### US-081: Documentation and Final Release
**Description:** As an engine user, I want a clear, working package so that I can build the engine and release a sample game.
**Depends on:** US-080.
**Acceptance Criteria:**
- [ ] The README describes OS/SDK requirements, offline CMake setup, the editor workflow, Lua API, asset pipeline, and native packaging.
- [ ] Add architecture documentation, component/scene schemas, the vendor manifest/notices, benchmark reproduction instructions, and a capability matrix.
- [ ] A complete acceptance checklist maps mandatory features to tests and artifacts; all standalone packages pass a clean-directory smoke test.

## Functional Requirements

- FR-01: First-party engine/editor/tool code is written in C++20; third-party C/C++ sources retain their projects' language and requirements.
- FR-02: Implement the ECS, window management, and input in-house; ready-made implementations of these three subsystems are prohibited.
- FR-03: The editor, standalone runtime, converter, tests, and benchmarks are separate CMake targets; the runtime does not depend on the editor or source importers.
- FR-04: Support Windows/DX12, Windows/Vulkan, and Linux/Vulkan; select the backend explicitly and include it in reports.
- FR-05: The RHI encapsulates backend-specific GPU operations while retaining explicit synchronization/lifetime/capability contracts.
- FR-06: Provide conventional indexed, GPU indirect, and mesh shader paths; a correct indirect-indexed fallback is available without mesh shaders.
- FR-07: PBR, IBL, directional/point/spot lights, shadows, transparency, HDR, TAA, tone mapping, and skeletal animation are mandatory.
- FR-08: The full profile supports RT shadows/reflections, dynamic GI, denoising, volumetric fog, and temporal upscaling simultaneously.
- FR-09: The native profile uses native resolution, TAA, raster shadows, baked probe GI/IBL, and SSR without heavy RT.
- FR-10: Terrain supports tile LOD, four splat layers, sculpt/paint, shadows, colliders, and world streaming.
- FR-11: GPU particles support the specified emitter parameters, transparent rendering, and a controlled resource budget.
- FR-12: Scene objects have persistent GUIDs, transforms, components, hierarchy, and validated asset references.
- FR-13: Scenes, prefabs, material/animation graphs, world cells, and saves have schema versions and explicitly reject unsupported formats.
- FR-14: Import assets from glTF/GLB, FBX, OBJ, PNG/JPEG, and WAV/OGG; report specific unsupported subtypes.
- FR-15: The converter runs headless and supports incremental dependency-aware cooking, LOD/meshlet generation, and resource packing.
- FR-16: Source asset changes are automatically converted and safely update editor/runtime previews.
- FR-17: World partition asynchronously loads and unloads cells while coordinating rendering, physics, and script lifecycles.
- FR-18: Jolt provides rigid bodies, colliders, collision filters, triggers, queries, a character controller, and constraints.
- FR-19: Lua controls gameplay through a validated API with lifecycle callbacks, diagnostics, and reload.
- FR-20: Input supports keyboard/mouse/gamepad, remapping, text, and capture; ImGui also uses the in-house platform abstraction.
- FR-21: The runtime provides 2D/3D audio, game UI, saves, and standalone game packages.
- FR-22: The editor includes docking, a scene viewport/hierarchy/inspector, gizmos, an asset browser, undo/redo, and Play/Pause/Step/Stop.
- FR-23: The editor includes a visual PBR material graph and animation state graph with preview, undo/redo, and cooking.
- FR-24: The editor edits terrain and world partition; changes are coordinated with derived assets and physics.
- FR-25: Sources for engine libraries, shader tools, and required transitive dependencies are physically included in `third_party/`; configure/build/run does not download them.
- FR-26: Required system tools and OS ABIs are listed separately; a clean build checks their presence and versions.
- FR-27: Dedicated and combined test scenes are reproducible; separate runs verify alternative modes.
- FR-28: Performance is defined by frame-time percentiles under a fixed workload, not by average FPS.
- FR-29: Any missing mandatory feature, unverified platform, or incomplete report retains an incomplete status.

## Non-Goals

- Multiplayer, networking, dedicated servers, MMOs, and distributed simulation.
- macOS, mobile, consoles, VR, WebGPU, OpenGL, DX11, and Linux through a DX12 translation layer.
- Ready-made ECS or window/input frameworks, or another engine or renderer in place of in-house modules.
- A native Wayland backend in the first version: Linux runs through X11 or XWayland, and this system requirement is documented explicitly.
- Arbitrary visual gameplay scripting in place of Lua, C++ code hot reload, a full cinematic sequencer, and DCC modeling.
- Voxel/destructible terrain, a built-in terrain erosion solver, fluid/cloth/soft-body simulation, and a fully path-traced renderer.
- Material nodes beyond the defined PBR set, IK/retargeting/editor blend spaces, and automatic import of every FBX feature.
- DLSS/XeSS, frame generation, and dynamic resolution as mandatory first-version integrations; FSR 2 provides upscaling.
- Cross-compilation of game packages and automatic installation of compilers/OS SDKs/GPU drivers through CMake.
- A promise of 60 FPS for arbitrary user content, any computer, or arbitrary Ultra settings outside the benchmark contract.

## Technical Considerations

### Project Structure

```text
cmake/                  preset helpers, validation, toolchains
third_party/            complete pinned dependency source trees
third_party/manifest.json
engine/core/            diagnostics, IO, IDs, jobs
engine/platform/        in-house Win32/X11 windowing and native input
engine/ecs/             in-house entity storage, queries, systems
engine/scene/           reflection, hierarchy, formats, prefabs
engine/assets/          registry, runtime loading, packs
engine/rhi/             common contract and dx12/vulkan implementations
engine/render/          render graph, lighting, geometry paths, effects
engine/world/           terrain, cells, streaming, coordinates
engine/physics/         Jolt adapter
engine/script/          Lua bindings and lifecycle
engine/audio/           sound runtime
engine/ui/              game UI
editor/                 ImGui editor and document/command models
runtime/                standalone game host
tools/asset_converter/  import/cook/validate/package
tools/benchmark/        harness and scene generator
tests/                  CPU, GPU, and editor tests; format fixtures
samples/all_features/   combined sample project
benchmarks/             versioned workload/profile/route manifests
docs/                   build, architecture, API, pipeline, results
```

Target and dependency organization prohibits a reverse dependency from the runtime to the editor. Editor UI documents and command history are tested without a GUI. Render extraction publishes a frame snapshot, and ECS structural changes occur only at defined synchronization points.

### Platform and Build Baseline

- Target architecture is x86-64, with Windows 11 and Linux using glibc, X11 or XWayland, and an installed Vulkan driver. Verify MSVC, Clang, and GCC through presets; pin specific working versions in CI and build documentation during US-001–US-004.
- Require CMake 3.28 or later and Ninja Multi-Config; Debug and Release have separate build/test presets. Compiler paths and OS SDK locations are not hardcoded in the repository.
- Windows uses Win32/Raw Input/XInput; Linux uses X11/XInput2/XIM/evdev. In-house code calls these APIs without SDL/GLFW. Native system ABIs, compilers, standard libraries, the Windows SDK, display/audio services, and GPU drivers are not included as sources in `third_party/`.
- Vulkan headers/loader/tools are included in the vendor set; an installed Vulkan SDK may be used for diagnostics but does not replace pinned library sources. The driver/ICD is installed on the system.
- The Vulkan baseline is 1.3 with required feature bits; the mesh path uses `VK_EXT_mesh_shader`, and RT uses KHR acceleration structures/ray tracing. DX12 checks mesh shader tier and DXR capabilities rather than relying only on the GPU name. These checks follow the API contracts for [DirectX Mesh Shader](https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html) and [Vulkan Ray Tracing](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html).
- Native shader source is HLSL; DXC outputs DXIL/SPIR-V. [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) supports this shared toolchain.

### Dependency Catalog

This is the permitted source dependency set, which must actually be downloaded and included during implementation. Pin exact revisions and all required transitive dependencies in the manifest rather than using a floating `latest`. If a selected revision does not pass both backends/operating systems, select a compatible revision without changing mandatory functionality.

| Component | Purpose |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui), pinned docking revision | Editor UI; in-house platform/input adapter |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Physics backend |
| [Lua](https://www.lua.org/) | Lua VM; in-house C API bindings, without sol2 |
| [Assimp](https://github.com/assimp/assimp) and required contrib sources | Model/animation import in the converter |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Offline LOD/meshlet generation |
| GLM | Math primitives; in-house wrappers define engine conventions |
| stb_image, stb_image_resize, stb_truetype, stb_vorbis | Image/font/audio decoding and preprocessing |
| bc7enc_rdo | BC7 encoding; the BC5 encoder and format wrapper reside in the converter |
| miniaudio | Audio device/mixing adapter |
| nlohmann/json, zstd | Authoring formats and pack compression |
| ImGuizmo, imgui-node-editor | Transform gizmos and graph canvas |
| Catch2 | CPU/integration test harness |
| D3D12MemoryAllocator, VulkanMemoryAllocator | GPU allocations behind the in-house RHI |
| DirectXShaderCompiler, SPIRV-Headers/Tools, and required transitive dependencies | Offline shader toolchain |
| Vulkan-Headers, Vulkan-Loader, Vulkan-ValidationLayers, and required transitive dependencies | Vulkan build/debug infrastructure |
| [FidelityFX FSR 2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2) | Open-source temporal upscaling with DX12/Vulkan backends |
| A licensed font with Cyrillic support, format fixtures, and sample sounds | Reproducible editor/runtime/test assets |

FSR 2 is selected as the specific baseline for shared DX12/Vulkan upscaling; its source project provides both backends. This is a first-version technical decision, not a claim that it is the latest available technology. [Official FSR 2](https://gpuopen.com/fidelityfx-superresolution-2/).

OS headers/import libraries and system display/audio services are accounted for separately from engine libraries. The vendor validator checks the complete closure of required sources, licenses/notices, and local patches. Filling `third_party/` with README links or scripts that only download sources during the build does not satisfy the requirement.

### Image Profiles

| Setting | `native_1080` | `full_1080` |
|---|---|---|
| Output | 1920×1080 | 1920×1080 |
| Internal resolution | 1920×1080 | 1280×720, fixed |
| AA/upscale | TAA, no upscaling | FSR 2 Quality |
| Geometry | Meshlets for static meshes, indirect for the rest; test forced fallback separately | Same |
| PBR/animation/terrain/particles | Enabled | Enabled |
| Main shadows | 4 directional cascades at 2048²; 4 local shadow lights in a 4096² atlas | RT directional + the same local shadow budget |
| GI | Baked diffuse probes + IBL | Dynamic ray-traced diffuse probes + IBL |
| Reflections | SSR + environment fallback | Half-internal-resolution RT, 1 ray/pixel, 1 bounce, max distance 100 m + environment fallback |
| RT shadows | Disabled | 1 ray/pixel, temporal/spatial filtering |
| Dynamic GI budget | Not applicable | Grid 32×8×32, spacing 4 m, 64 rays/probe, update 1/16 of the grid per frame |
| Volumetric fog | Enabled, grid 160×90×64 | Enabled, same grid |
| HDR/post | Linear HDR, exposure, bloom, tone mapping | Same |
| UI | Output resolution | Output resolution after upscaling |
| Frame generation / dynamic resolution | Disabled | Disabled |

Compatible full-profile effects run simultaneously. Profiles and forced runs cover mutually exclusive GI/AA/geometry path implementations. Profile parameters reside in versioned manifests; automatically disabling effects when the frame-time budget is exceeded is prohibited. On hardware without RT, the full profile reports unsupported instead of relabeling a raster fallback as `full`.

### Benchmark Scenes and Workloads

All numbers below are a proposed workload contract defined at the user's request, not RTX 5080 measurement results. If a budget cannot be achieved, fix the implementation first; workload reductions or quality relaxations require separate agreement.

Shared world: 8192×8192 m, cells of 256×256 m, 32×32 cells; each terrain tile has 257×257 height samples. Detailed residency is a 9×9-cell window with prefetch of the next ring; far terrain LOD extends to 2048 m. Object draw distance is 1000 m, and vertical FOV is 60°. Modified terrain samples on shared boundaries follow a single authoring contract.

| Scene | Fixed Workload |
|---|---|
| `render_stress` | 20 000 renderable instances, 200 unique meshes, 100 animated characters with 64 bones each, 100 000 live GPU particles, 128 local lights including 4 shadowed lights, plus a directional sun. Fixtures/LOD manifests specify at least 2 million submitted triangles in the indexed reference path on the starting frame; record the route and actual counters. |
| `physics_stress` | 2 000 simultaneously active dynamic rigid bodies, 200 static colliders, 100 constraints, 20 characters; 1 000 raycasts/s and 100 shape casts/s at a fixed 60 Hz. The scenario regularly applies forces or interactions to bodies; replacing the workload with sleeping bodies is prohibited. |
| `streaming_stress` | The shared world, 200 000 instance descriptors distributed across cells, movement at 30 m/s along a 5400 m route with boundary crossings and backtracking. Activation/unload occurs during measurement; a separate correctness case verifies teleport. |
| `all_features` | The shared world with 100 000 descriptors; the reference area and route maintain 10 000 resident renderables, including 50 animated characters with 64 bones each, 1 000 active dynamic bodies, 50 constraints, 50 000 live particles, 64 local lights including 4 shadowed lights, and a directional sun. Include terrain with 4 layers, Lua gameplay/input, 16 audio emitters, a music stream, HUD/menu, save/load fixtures, and material/animation graphs. |

The generator stores expected totals, active/visible distributions at checkpoints, and content hashes. A report cannot consider the workload executed solely from the number of created descriptors: verify resident, active, submitted, and feature-specific counters. Total descriptors may be outside the view by the nature of an open-world test; unintended disappearance of reference objects is a failure.

Both render scenes and the streaming scene run with both profiles. Measure the physics scene with the visual runtime and separately in headless mode for CPU diagnostics; the FPS requirement applies to the visual run. Verify game UI/audio/save correctness through separate actions: lengthy save writes and initial loading are not mixed into the steady-state camera route, but their latency is reported separately.

### Performance Contract

- Reference GPU: NVIDIA GeForce RTX 5080. The environment provisionally identified the CPU as an AMD Ryzen 9 9950X3D2 16-Core Processor; the native benchmark runner automatically records the actual CPU, RAM, VRAM, SSD, OS build, driver, power profile, and display mode. WSL memory capacity is not treated as the computer's physical RAM.
- Verify Windows/DX12, Windows/Vulkan, and native Linux/Vulkan on the same computer. Another machine may be used for development smoke tests but does not replace this performance acceptance.
- Use the standalone runtime in Release with validation/debug overlays disabled; disable VSync, FPS limiters, and frame generation. Profiling timestamps/counters remain enabled consistently across repeats.
- For each scene/profile/backend: 30 s warmup followed by 180 s of measurement, with three independent repeats. Prepare shaders before measurement; cell IO/decode/upload and gameplay continue during the measured route.
- CPU frame time is the wall-clock interval between the starts of consecutive frames, including fence waits, present, and runtime streaming work. GPU frame time is the duration of the complete frame workload measured through timestamp queries. Acceptance uses `max(cpu_frame_ms, gpu_frame_ms)`; store raw CPU/GPU values separately. Limit frames in flight to two; convert multi-queue timestamps to a verifiable common frame span.
- In every repeat, p99 is no higher than `1000/60` ms and at least 99% of frames meet that threshold. Use the nearest-rank percentile `ceil(p*N)`; rounding the report to 16.67 ms does not relax the comparison.
- Additionally bound rare delays: at most 0.1% of frames exceed `1000/30` ms, and no frame exceeds 100 ms after warmup. A failed repeat is not hidden by averaging it with successful ones.
- Peak runtime residency of the combined scene is at most 12 GiB of GPU memory and 16 GiB of host working set, including transient streaming allocations. Disk caches and toolchain sources are not runtime residency.
- Separately record first launch/cold IO, shader preparation, teleport, and save/load latency. These operations do not receive an invented FPS pass; the corresponding correctness/stall reports are mandatory.
- Golden images use backend-specific baselines and documented tolerances. Verify key features through masks/depth/object IDs/feature counters so that image tolerance cannot hide a missing effect.
- These values are future acceptance criteria. The PRD does not claim that the performance target has already been achieved.

## Success Metrics

- Complete all US-001–US-081 stories; link mandatory acceptance criteria to tests or verifiable artifacts.
- A clean offline build passes on Windows and Linux with all library/tool dependency sources in the repository.
- CPU/integration/sanitizer tests pass; GPU validation reports no errors across the three OS/backend combinations.
- In the editor, create a project, import a model, configure material/animation graphs and terrain, run Lua gameplay, save, and release a standalone game.
- All benchmark scenes and profiles pass the fixed frame-time and memory budgets on the reference computer; raw reports are available for reproduction.
- Complete visual editor smoke tests on both operating systems; automated UI models and repeated Play/Stop pass without resource accumulation.
- Game packages run without the editor, source assets, the original build tree, or network downloads.

## Open Questions

- The engine name has not been chosen; use neutral `engine_*` target names until a separate decision. This does not block implementation.
- Native benchmark metadata will record the reference computer's exact OS builds, driver versions, RAM/VRAM, and storage. GPU verification is unavailable through the current WSL environment.
- Native Linux availability with the RTX 5080 has not been confirmed. This does not block CPU development but is mandatory for final Linux GPU/performance acceptance.
- Determine and verify specific dependency revisions in US-002–US-003; dependency tasks are not complete until the manifest is populated.

[/PRD]
