# Capability matrix

What the engine supports on each platform and backend, and what has been verified there. The verification column
names what was run; a blank or "not run" means there is no report. Final acceptance results are in
[acceptance.md](acceptance.md). A capability is not verified on a platform until its gate or GPU report exists.

Platforms: **Windows** (MSVC, x64), **Linux** (Clang 10 locally, Clang 14 and GCC 11 in CI). Backends: **DX12**
(Windows only), **Vulkan** (Windows and native Linux).

## Platform and build

| Capability | Windows | Linux | Verification |
|---|---|---|---|
| Vendor validation (`ValidateVendor.cmake`) | yes | yes | Both run; see acceptance.md |
| Configure and build (Debug) | yes | yes (Clang); GCC 9 cannot build Jolt, GCC 11+ required | See acceptance.md |
| CPU unit and integration tests | yes | yes | `win-unit`, `linux-unit` |
| Sanitizers (ASan + UBSan, CPU) | not in CI on Windows | yes | `linux-sanitize`; not run for US-081 |
| Offline configure and build | yes | yes (`offline_verify.sh`, network namespace) | Linux script exists; clean-copy run not done |
| Native window and input | in-house Win32 | in-house X11 and evdev | `platform_*` tests |
| Standalone game package | yes (native) | yes (native) | `editor_distribution_tests`; clean-directory smoke test not done |

## Rendering

| Capability | DX12 (Windows) | Vulkan (Windows) | Vulkan (native Linux) | Verification |
|---|---|---|---|---|
| Device, presentation, descriptors | yes | yes | yes, needs a Vulkan 1.2 driver with timeline semaphores | `rhi_*` GPU tests |
| Validation layer | D3D12 debug layer | Vulkan validation (1.4.363 layer built for Windows) | not available on this verification machine | Debug runs only |
| Render graph and scene rendering | yes | yes | yes | `render_graph_*`, `render_scene_*` |
| PBR, lighting, raster shadows | yes | yes | yes | `render_material_*`, `lighting_*` |
| Skinning and playback | yes | yes | yes | `render_skin_*` |
| GPU indirect | yes | yes | yes | `render_indirect_gpu_tests` |
| Mesh shaders | yes, when the device supports them | yes, when the device supports them | yes, when the device supports them | `render_meshlet_*`; without mesh shaders the renderer uses the indirect-indexed path |
| HDR, TAA, post | yes | yes | yes | `postprocess_tests`, `render_postprocess_gpu_tests` |
| Probe and reflection GI | yes | yes | yes | `gi_probe_tests`, `render_gi_gpu_tests` |
| Ray-traced shadows, reflections, GI | yes, DXR | yes, `VK_KHR_ray_query` and `VK_KHR_pipeline_library` | yes, when the device supports it | `render_rt_*`, `render_dynamic_gi_*` |
| Denoising and volumetrics | yes | yes | yes | `render_denoise_*`, `render_volumetric_*` |
| Temporal upscaling (FSR 2) | **not available** | **not available** | **not available** | The vendored FSR 2 tree lacks its generated shader permutations; backends return `Unsupported` |
| Terrain runtime | yes | yes | yes | `render_terrain_*`, `world_terrain_tests` |
| GPU particles | yes | yes | yes | `render_particles_*` |
| Dynamic GI and RT in the scene | yes | yes | yes | `render_rt_scene_gpu_tests` |

"Yes" for a GPU capability means the code path exists. Whether it has been run on a device is recorded in
[acceptance.md](acceptance.md). Software adapters (WARP, llvmpipe, lavapipe, SwiftShader) and WSL do not produce GPU reports.

## Physics, animation, scripting, audio, UI

| Capability | Status | Tests |
|---|---|---|
| Jolt world with a fixed timestep | Implemented on Windows and Clang; GCC build not verified | `physics_world_tests` |
| Colliders, queries, terrain physics, streamed collision | Implemented | `physics_query_tests` |
| Character controller and constraints | Implemented | `physics_character_tests` |
| Animation state machine and graph documents | Implemented | `animation_state_machine_tests`, `animation_graph_edit_tests` |
| Lua API (entities, transforms, input, assets, physics, animation, events, audio) | Implemented | `script_lua_tests`, `script_animation_tests`, `script_audio_tests` |
| Script lifecycle and reload | Implemented | `script_lifecycle_tests` |
| Audio (offline mixer, device through a C shim, cell-scoped voices) | Implemented | `audio_mixer_tests`, `audio_device_tests`, `audio_decode_tests` |
| Game UI (CPU rasterized, input, Lua callbacks) | Implemented | `ui_layout_tests`, `ui_text_tests`, `ui_interaction_tests`, `script_ui_tests` |
| Game saves | Implemented | `save_tests` |
| Resource packs and standalone runtime | Implemented; the runtime's component catalog is empty in this build | `pack_tests`, `runtime_options_tests`, `tests/pack/PackRuntimeRoundTrip.cmake` |

## Editor and tools

| Capability | Status | Tests |
|---|---|---|
| ImGui editor shell and docking layout | Implemented; visual smoke test on Windows and Linux not run | `editor_imgui_tests` |
| Scene hierarchy and inspector | Implemented | `editor_scene_tests` |
| Viewport and gizmos | Implemented | `editor_viewport_tests` |
| Undo and redo | Implemented (scene and material graph) | `editor_scene_tests`, `material_graph_edit_tests` |
| Play, pause, step, stop | Implemented; the authored scene is not changed by Play | `editor_play_tests` |
| Asset browser and import | Implemented | `editor_asset_tests` |
| Terrain sculpting and painting | Implemented | `editor_terrain_tests`, `world_terrain_edit_tests` |
| World partition editing | Implemented | `editor_world_tests` |
| Material graph compiler and editor | Implemented | `material_graph_*` |
| Animation graph editor | Implemented | `animation_graph_edit_tests` |
| Build game from the editor | Implemented | `editor_distribution_tests` |
| Asset converter (validate, import, cook, watch) | Implemented | `converter_*` |

## Benchmarks

| Capability | Status |
|---|---|
| Harness, manifest, contract evaluation, report | Implemented (`engine_bench`) |
| Seeded workload generator | Implemented; digests pinned in `benchmarks/workload_expected.json` |
| Physics stress, headless profile on CPU | Implemented (US-077) |
| Streaming stress, headless profile on CPU | Implemented (US-078); the 30-minute soak is outside acceptance |
| Render stress (GPU) | Implemented; GPU timestamps and residency are not yet collected, so acceptance runs report `incomplete` |
| All-features scene | Declared; reports `missing` until its story builds it |
| Full acceptance suite on DX12 and Vulkan | **Not produced.** See [acceptance.md](acceptance.md) and [benchmarks.md](benchmarks.md) |

## Known limits

- FSR 2 cannot be built until its generated shader tables are vendored or regenerated (see [vendor.md](vendor.md)).
- Vulkan on Linux has not been run on a native device with a validation layer on this verification machine.
- Any GPU claim needs a report from the reference RTX 5080 computer, not from WSL or a software adapter.
- The project has no license file yet (see [vendor.md](vendor.md)).
