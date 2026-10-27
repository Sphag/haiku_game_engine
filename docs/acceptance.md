# Acceptance checklist

This page maps each PRD story to the tests and artifacts that cover it, then lists the release gates and the items that
are still open. Source of the story list: `tasks/prd-open-world-engine.md` and `tasks/prd.json`.

**Final acceptance is incomplete.** The reasons are in [Open items](#open-items). Nothing on this page should be read as
"passed" unless it names the run that produced the result.

## Reading the map

- Test files are under `tests/src/` (CPU and GPU). CTest runs them by tag (`[unit]`, `[integration]`, `[gpu][<backend>]`,
  `[editor]`). Catch2 cannot select one case by name, so check a suite's new cases by reconciling its total.
- `Artifacts` names what a run leaves behind: a report, a fixture, a benchmark folder, or a gate log.
- A story with no GPU test is CPU-only. A GPU test that needs a device is reported `not-run` when no device is present.

## Feature map

| Story | Feature | Tests | Artifacts and docs |
|---|---|---|---|
| US-001 | CMake and initial checks | `tests/cli/CliContract.cmake`, CTest cases `engine_tests_cli_*` | `cmake/ValidateVendor.cmake`, [build.md](build.md) |
| US-002 | Library dependency sources | `fixture_provenance_tests` (fixtures), vendor validator | `third_party/manifest.json`, [vendor.md](vendor.md) |
| US-003 | Buildable shader toolchain | `shader_compiler_tests`, `shader_diagnostics_tests`, `shader_program_tests` | `engine_add_hlsl_shader`, [shader-pipeline.md](shader-pipeline.md) |
| US-004 | CI and verification reports | CI workflow (`.github/workflows/ci.yml`, `gpu.yml`) | `artifacts/reports/*.xml`, `gpu-*.json` |
| US-005 | Core runtime services | `core_*_tests` (assert, clock, coordinates, file I/O, GUID, log, path, result, units, UTF-8) | [runtime-conventions.md](runtime-conventions.md) |
| US-006 | Jobs and controlled shutdown | `core_jobs_tests`, `lifecycle_tests` | [runtime-conventions.md](runtime-conventions.md) |
| US-007, US-008 | Windowing (Win32, X11) | `platform_win32_tests`, `platform_x11_tests`, `platform_window_tests` | [windowing.md](windowing.md) |
| US-009, US-010 | Input (Win32, Linux evdev and X11) | `platform_win32_input_tests`, `platform_evdev_tests`, `platform_x11_input_tests`, `platform_input_tests`, `platform_replay_tests` | [input.md](input.md) |
| US-011 | Actions and input routing | `input_action_tests`, `input_router_tests`, `input_profile_tests`, `input_scenario_tests` | [input.md](input.md) |
| US-012 to US-014 | ECS handles, storage, systems | `ecs_world_tests`, `ecs_storage_tests`, `ecs_systems_tests`, `ecs_command_buffer_tests` | [ecs.md](ecs.md) |
| US-015 | Component reflection | `scene_reflection_tests` | [schemas.md](schemas.md) |
| US-016 | Scene graph and transforms | `scene_graph_tests`, `scene_transform_tests` | [scene-graph.md](scene-graph.md) |
| US-017 | Scene format and prefabs | `scene_format_tests`, `scene_tests` | [scene-format.md](scene-format.md), [schemas.md](schemas.md) |
| US-018 | Asset registry | `assets_registry_tests`, `assets_project_tests` | [asset-registry.md](asset-registry.md) |
| US-019 | Asset converter CLI | `converter_tests` | [asset-converter.md](asset-converter.md) |
| US-020 | Model import | `converter_model_tests` | [model-import.md](model-import.md) |
| US-021 | Texture conversion | `converter_texture_tests` | [texture-import.md](texture-import.md) |
| US-022 | Skeleton and clip import | `converter_model_tests`, `animation_tests` | [model-import.md](model-import.md) |
| US-023 | LOD and meshlet cooking | `converter_mesh_tests`, `render_meshlet_tests` | [mesh-cooking.md](mesh-cooking.md) |
| US-024 | Runtime asset handles | `assets_handle_tests`, `assets_cooked_tests` | [runtime-assets.md](runtime-assets.md) |
| US-025 | Changed asset updates | `converter_watch_tests` | [source-watch.md](source-watch.md) |
| US-026 | RHI contract | `rhi_contract_tests`, `rhi_vulkan_mapping_tests` | [rhi.md](rhi.md) |
| US-027, US-029 | DX12 device, resources, descriptors | `rhi_dx12_gpu_tests` `[gpu][dx12]` | [dx12.md](dx12.md) |
| US-028, US-030 | Vulkan device, resources, descriptors | `rhi_vulkan_gpu_tests` `[gpu][vulkan]` | [vulkan.md](vulkan.md) |
| US-031 | Shaders, reflection, PSO cache | `shader_reflection_tests`, `shader_cache_tests`, `shader_gpu_tests` | [shader-pipeline.md](shader-pipeline.md) |
| US-032 | Render graph | `render_graph_tests`, `render_graph_gpu_tests` | [render-graph.md](render-graph.md) |
| US-033 | Scene rendering and picking | `render_scene_tests`, `render_scene_gpu_tests` | [scene-rendering.md](scene-rendering.md) |
| US-034 | PBR and transparency | `render_material_tests`, `render_material_gpu_tests` | [materials.md](materials.md) |
| US-035 | Lighting and raster shadows | `lighting_tests`, `lighting_gpu_tests` | [lighting.md](lighting.md) |
| US-036 | Playback and skinning | `render_skin_tests`, `render_skin_gpu_tests`, `animation_tests` | [animation.md](animation.md) |
| US-037 | GPU indirect pipeline | `render_indirect_gpu_tests` | [scene-rendering.md](scene-rendering.md) |
| US-038 | Mesh shader pipeline | `render_meshlet_tests`, `render_meshlet_gpu_tests` | [mesh-shaders.md](mesh-shaders.md) |
| US-039 | HDR, TAA, post | `postprocess_tests`, `render_postprocess_gpu_tests` | [postprocess.md](postprocess.md) |
| US-040 | Native-profile GI and reflections | `gi_probe_tests`, `gi_reflection_tests`, `render_gi_gpu_tests` | [gi-probes.md](gi-probes.md), [reflection.md](reflection.md) |
| US-041 to US-043 | DX12 and Vulkan ray tracing, RT shadows and reflections | `render_rt_tests`, `render_rt_gpu_tests`, `render_rt_scene_gpu_tests` | [ray-tracing-effects.md](ray-tracing-effects.md) |
| US-044 | Dynamic RT GI | `gi_dynamic_grid_tests`, `render_dynamic_gi_tests`, `render_dynamic_gi_gpu_tests` | [dynamic-gi.md](dynamic-gi.md) |
| US-045 | Denoising and volumetrics | `render_denoise_tests`, `render_denoise_gpu_tests`, `render_volumetric_tests`, `render_volumetric_gpu_tests` | [denoise-volumetrics.md](denoise-volumetrics.md) |
| US-046 | Temporal upscaling | `render_upscaler_tests`, `rhi_upscaler_tests` (FSR 2 unavailable) | [upscaling.md](upscaling.md) |
| US-047 | Terrain runtime | `render_terrain_tests`, `render_terrain_gpu_tests`, `world_terrain_tests` | [terrain-editing.md](terrain-editing.md) |
| US-048 | World partition data | `world_partition_tests` | [world-editing.md](world-editing.md) |
| US-049 | Asynchronous world streaming | `world_streaming_tests` | [large-world.md](large-world.md) |
| US-050 | Large-world coordinates | `world_origin_tests`, `core_coordinates_tests` | [large-world.md](large-world.md) |
| US-051 | GPU particles | `render_particles_tests`, `render_particles_gpu_tests` | [particles.md](particles.md) |
| US-052 | Jolt world and fixed timestep | `physics_world_tests` | [physics.md](physics.md) |
| US-053 | Colliders, queries, terrain physics | `physics_query_tests` | [physics.md](physics.md) |
| US-054 | Character and constraints | `physics_character_tests` | [physics.md](physics.md) |
| US-055 | Engine Lua API | `script_lua_tests`, `script_animation_tests`, `script_audio_tests` | [runtime-conventions.md](runtime-conventions.md) |
| US-056 | Lua lifecycle and reload | `script_lifecycle_tests` | [runtime-conventions.md](runtime-conventions.md) |
| US-057 | Audio | `audio_mixer_tests`, `audio_device_tests`, `audio_decode_tests` | [runtime-conventions.md](runtime-conventions.md) |
| US-058 | Game UI | `ui_layout_tests`, `ui_text_tests`, `ui_interaction_tests`, `script_ui_tests` | [ui.md](ui.md) |
| US-059 | Game saves | `save_tests` | [schemas.md](schemas.md) |
| US-060 | Standalone runtime and resource packs | `pack_tests`, `runtime_options_tests`, `tests/pack/PackRuntimeRoundTrip.cmake` | [runtime-packs.md](runtime-packs.md) |
| US-061 | ImGui editor shell | `editor_imgui_tests`, `editor_console_tests`, `editor_workspace_tests` | [editor.md](editor.md) |
| US-062 | Scene hierarchy and inspector | `editor_scene_tests`, `editor_model_tests` | [editor.md](editor.md) |
| US-063 | Viewport and gizmos | `editor_viewport_tests` | [editor.md](editor.md) |
| US-064 | Undo and redo | `editor_scene_tests`, `material_graph_edit_tests` | [editor.md](editor.md) |
| US-065 | Play, pause, step, stop | `editor_play_tests` | [editor.md](editor.md) |
| US-066 | Asset browser and import UI | `editor_asset_tests`, `editor_project_tests` | [asset-registry.md](asset-registry.md) |
| US-067 | Material graph compiler | `material_graph_tests`, `material_graph_compile_tests` | [material-graph.md](material-graph.md) |
| US-068 | Visual material editor | `material_graph_edit_tests` | [material-graph.md](material-graph.md) |
| US-069 | Animation state machine runtime | `animation_state_machine_tests` | [animation.md](animation.md) |
| US-070 | Animation graph editor | `animation_graph_edit_tests` | [animation.md](animation.md) |
| US-071 | Terrain sculpting and painting | `editor_terrain_tests`, `world_terrain_edit_tests` | [terrain-editing.md](terrain-editing.md) |
| US-072 | World partition editor | `editor_world_tests`, `converter_world_tests` | [world-editing.md](world-editing.md) |
| US-073 | Game distribution from the editor | `editor_distribution_tests` | [distribution.md](distribution.md) |
| US-074 | Benchmark harness | `bench_tests` | [benchmarks.md](benchmarks.md) |
| US-075 | Reproducible test assets | `bench_workload_tests`, `fixture_provenance_tests` | `benchmarks/workload_expected.json`, `tests/fixtures/PROVENANCE.md` |
| US-076 | Render benchmark | `bench_tests` (render stress manifest and driver), GPU runs | `artifacts/bench/us076-*` |
| US-077 | Physics benchmark | `bench_physics_tests` | `artifacts/bench/us077-dx12` |
| US-078 | Streaming benchmark | `bench_streaming_tests` | `artifacts/bench/us078-soak-linux-debug` |
| US-079 | Combined feature scene | `all_features_tests`, `all_features_editor_tests` | [all-features](../tests/src/all_features_tests.cpp) |
| US-080 | RTX 5080 performance acceptance | Acceptance suite (see Open items) | `artifacts/bench/us080-*` (readback-off runs only) |

## Release gates

Each gate is a command, with the environment where it must run. A gate that has not been run is listed as `not run`.

| Gate | Environment | State |
|---|---|---|
| `cmake -P cmake/ValidateVendor.cmake` | Linux | Passed: 26 dependencies, 8542 files |
| `cmake -P cmake/ValidateVendor.cmake` | Windows | Passed: 26 dependencies, 8542 files |
| `cmake --preset linux-dev` | Linux | Passed (exit 0) |
| `cmake --build --preset linux-debug` | Linux | Passed (exit 0). The tree was already built, so only the DXC external step ran |
| `ctest --preset linux-unit --output-on-failure --no-tests=error` | Linux | Passed: 30 of 30 tests (1 unit, 29 integration) |
| `cmake --preset win-dev` | Windows | Passed (exit 0) |
| `cmake --build --preset win-debug` | Windows | Passed (exit 0), rerun on its own. The tree was already built, so only the DXC external step ran |
| `ctest --preset win-unit --output-on-failure --no-tests=error` | Windows | Passed: 29 of 29 tests (1 unit, 28 integration) |
| `cmake --preset linux-gcc`, `cmake --build --preset linux-gcc-debug`, `ctest --preset linux-gcc-unit` | Linux (GCC 11+) | not run; the local GCC 9.4 cannot build Jolt |
| `cmake --preset linux-asan`, `cmake --build --preset linux-asan`, `ctest --preset linux-sanitize` | Linux | passed (ASan + UBSan, CPU tests, 30 of 30) |
| `cmake --build --preset win-release` | Windows | Passed (exit 0; 596 of 596 steps, MSVC Release) |
| `cmake --build --preset linux-release`, `ctest --preset linux-release-unit` | Linux | passed (30 of 30). The first build failed on a release-only test compile error in `core_assert_tests.cpp`; fixed, then rebuilt |
| `engine_tests --suite gpu --backend dx12` | Windows, RTX 5080 | Passed: 72 cases, 43923 assertions. The report names `NVIDIA GeForce RTX 5080`, validation layers on (`artifacts/reports/us081/win-gpu-dx12.log`) |
| `engine_tests --suite gpu --backend vulkan` | Windows, RTX 5080 | Passed: 73 cases, 43935 assertions. The report names `NVIDIA GeForce RTX 5080`, `software=no`, validation layers on, and no VUID lines. The run used the pinned 1.4.363 layer via `VK_LAYER_PATH` (`artifacts/reports/us081/win-gpu-vulkan.log`) |
| `engine_tests --suite gpu --backend vulkan` | native Linux GPU | not run: no native Linux Vulkan device on the verification machine |
| `engine_tests --suite editor --backend <dx12\|vulkan>` | Windows | DX12 passed: 107 cases, 1588 assertions (`artifacts/reports/us081/win-editor-dx12.log`). Vulkan passed: 107 cases, 1587 assertions, pinned 1.4.363 layer via `VK_LAYER_PATH`, no VUID or error lines (`artifacts/reports/us081/win-editor-vulkan.log`) |
| `engine_bench --suite acceptance --backend <dx12\|vulkan> --output artifacts/bench/<run-id>` | Windows (DX12, Vulkan), native Linux (Vulkan), Release | not run; see Open items |
| Offline build from a clean repository copy | Linux | Passed: a copy without `build/`, `artifacts/`, and `.ralph-tui/` ran `tools/ci/offline_verify.sh` in a network namespace with only `lo` (outbound TCP refused). Validate, `linux-dev`, `linux-debug`, and `linux-unit` (30 of 30) passed |
| Standalone packages from clean directories | Windows, Linux | Linux: `engine_tests --suite editor` (CPU, `[editor]~[gpu]`) passed, 106 cases and 1569 assertions, on the current tree. Windows: `engine_tests --suite editor --backend dx12` and `--backend vulkan` passed, 107 cases each. The Windows logs list no per-case names, but `tests/src/editor_distribution_tests.cpp` is tagged `[editor][integration]`, so it runs in that suite. Both OSes run its "Build Game writes a package that launches from a clean folder" case, which builds a package from the sample project into an empty folder and launches it. Both results are from the suite run, not a separate package-only run |
| Visual editor smoke test with screenshots | Windows, Linux | Windows: launch only. `game_editor.exe` (Release) started and drew its panels (Project, Assets, Hierarchy, Viewport, Inspector, Console, Errors), with the console at "no project is open". Screenshot in `artifacts/reports/us081/win-editor-smoke-launch.png`. No project was opened and no action sequence ran, so the full smoke test is still open. Linux: not run as a visual test. On the WSL2 verification machine (WSLg display present), `game_editor` (Release, `build/linux-dev`) exits at start with `cannot create the vulkan device: no hardware Vulkan 1.2 adapter` and draws nothing. This is WSL without a Vulkan device, so it does not stand in for a native Linux smoke test |

## Open items

These block final acceptance. Each one is recorded as incomplete rather than passed.

1. **Native Linux Vulkan GPU report.** The verification machine (WSL2) has no usable Vulkan device. No `engine_tests --suite gpu
   --backend vulkan` report exists for native Linux.
2. **Full acceptance benchmark.** `engine_bench --suite acceptance` has not been run to completion on DX12 or Vulkan. The
   `artifacts/bench` folder holds only the US-076 to US-078 runs and two US-080 readback-off runs. The render, streaming,
   all-features, and physics acceptance scenes do not yet produce GPU timestamps and residency, so an acceptance run reports
   `incomplete` (see [benchmarks.md](benchmarks.md)).
3. **FSR 2.** The vendored tree lacks its generated shader permutations, so temporal upscaling is unavailable (see [vendor.md](vendor.md)).
4. **Linux GCC.** The local GCC 9.4 cannot build Jolt. The `linux-gcc` gate needs GCC 11 or newer.
5. **Visual editor smoke test.** No screenshot-backed run of the editor on Windows or Linux exists for US-081.
6. **Clean-directory checks.** The offline clean-copy build passed. The standalone package test (build from the sample project, then launch from an empty folder) passed in the editor suite on Linux and on Windows (DX12 and Vulkan runs). It is not a separate package-only run, and a package from a real game is not yet built.
7. **License.** The repository has no license file (see [vendor.md](vendor.md)).
