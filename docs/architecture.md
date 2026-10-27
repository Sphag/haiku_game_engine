# Architecture

The engine is a set of static libraries under `engine/`, one directory per module, linked by CMake targets named
`engine_<module>`. Tools (`tools/`) and the tests (`tests/`) link them. Each module's detailed design is in its own
document under `docs/`; this page gives the layers, the link rules, and the rules that cross modules.

## Layers

```
            tools/game_editor   tools/asset_converter   tools/asset_packer   tools/benchmark   tests
                     |                  |                       |                 |             |
             engine/editor ---- engine/converter (tools only) ----+              |             |
                     |                                                              |             |
   +-----------------+-------------------------------------------------+------------+             |
   | engine/script  engine/save  engine/ui  engine/audio  engine/physics  engine/animation        |
   | engine/gi  engine/material_graph  engine/render  engine/shader  engine/world                 |
   +--------------------------------------------------------------------------------------------+
                     |                                       |
   engine/rhi (contract; dx12 and vulkan backends)     engine/scene  engine/ecs  engine/assets  engine/pack
                     |                                       |
   engine/platform (windows, linux)  ----------------  engine/core (services, result, path, jobs, log, units)
```

| Module | Responsibility | Document |
|---|---|---|
| `core` | Result and error types, paths, UTF-8, GUIDs, files, jobs, logging, units, coordinates | [runtime-conventions.md](runtime-conventions.md) |
| `platform` | In-house Windows and Linux windows and input (no SDL or GLFW) | [windowing.md](windowing.md), [input.md](input.md) |
| `ecs` | Handles, storage, queries, systems, and deferred structural changes | [ecs.md](ecs.md) |
| `scene` | Scene graph, transforms, reflected components, scene and prefab files | [scene-graph.md](scene-graph.md), [scene-format.md](scene-format.md) |
| `assets`, `pack` | Asset registry and sidecars, runtime handles, resource packs | [asset-registry.md](asset-registry.md), [runtime-packs.md](runtime-packs.md) |
| `rhi` | Backend-neutral GPU contract; DX12 and Vulkan implementations; a validation device | [rhi.md](rhi.md), [dx12.md](dx12.md), [vulkan.md](vulkan.md) |
| `shader` | HLSL to DXIL and SPIR-V through the staged DXC; reflection; PSO cache | [shader-pipeline.md](shader-pipeline.md) |
| `render` | Render graph, scene rendering, lighting, shadows, meshlets, post, ray tracing, particles, terrain | [render-graph.md](render-graph.md), [scene-rendering.md](scene-rendering.md) |
| `gi`, `material_graph` | Probe and reflection GI, dynamic GI, material graph analysis and compile | [gi-probes.md](gi-probes.md), [dynamic-gi.md](dynamic-gi.md), [material-graph.md](material-graph.md) |
| `world` | Large-world origin, world partition cells, streaming, terrain tiles | [large-world.md](large-world.md) |
| `physics` | Jolt world with a fixed timestep, colliders, queries, character, constraints, streamed collision | [physics.md](physics.md) |
| `animation` | Clips, skinning, state machines, graph documents | [animation.md](animation.md) |
| `audio` | Offline mixer, device through a C shim, cell-scoped voices | |
| `script` | Lua VM behind a C shim, per-instance environments, the engine API, UI callbacks | [runtime-conventions.md](runtime-conventions.md) |
| `ui` | CPU-rasterized game UI composited after tone mapping | [ui.md](ui.md) |
| `save` | Game save slots with checksums and the previous-save copy | [schemas.md](schemas.md) |
| `editor` | Editor model (no ImGui, no device), scene and world editing, play session, distribution | [editor.md](editor.md) |
| `converter` | Model, texture, mesh, and world importers; cook and watch (tools only) | [asset-converter.md](asset-converter.md) |
| `runtime` | The standalone `game_runtime` | [runtime-packs.md](runtime-packs.md) |

## Link rules

- **The runtime is editor-free.** `engine_assert_runtime_is_editor_free` (`engine/runtime/CMakeLists.txt`) walks the
  transitive link graph of `game_runtime` at configure time and fails when it reaches the converter, its codecs, or
  Assimp. A new runtime dependency that pulls one of them in fails the configure step.
- **Native code stays behind C shims.** Vendored C and C++ code that the first-party code should not include (miniaudio,
  stb_truetype, the Lua VM) sits in its own target outside the first-party warning policy. First-party code includes only
  the shim header.
- **Native GPU APIs stay in `rhi`.** The DX12 and Vulkan backends implement the `rhi` contract; the render code above it
  does not include a native API header.
- **First-party code builds with warnings as errors** (`-Wall -Wextra -Wpedantic -Wshadow -Werror`, or `/W4 /WX`). Vendored code
  keeps its own flags.

## Frame and ownership

- **Fixed timestep.** Physics advances on a fixed step (`physics`, `docs/physics.md`). The physics benchmark runs on simulated
  time, so a lost step comes only from the catch-up limit; wall time is measured but not judged.
- **Large-world coordinates.** World positions are `double`. `WorldOrigin` moves the origin past 2048 m and sends
  `OriginShift`; only float state kept across frames (physics, matrices) is rebased. No returned transform jumps on a rebase.
- **Scene edits are checked atomically.** A structural edit copies the scene document, changes the copy, and calls
  `Scene::load`, which validates the whole document before it changes anything. A rejected edit leaves the scene unchanged.
- **The authored world is the only copy of an edit.** `WorldEditor` keeps edits in the authored `World`. Residency is a
  `StreamingManager` over a derived cache (`<dir>/.stream`), rebuilt after an edit.
- **Save data is never written into the authored project.** Saves read the world's dirty flags and never clear them.
- **Scripts run in their own environment.** Callbacks are read from the instance's environment only. A callback error
  faults that instance; the others keep running.
- **Jobs and shutdown.** Background work runs on the job system (`core`), which shuts down in a controlled order.
- **Editor model is independent of ImGui and the device.** `EditorModel` holds the state and commands; the ImGui shell draws
  it. Undo snapshots the scene document before and after each command.

## Rendering path

The render graph (`render`) orders passes and resources; each pass declares its reads and writes, and a resource has one
writer. The scene renderer picks a geometry path per mesh: direct, indirect indexed, or meshlet, with an indirect-indexed fallback
when mesh shaders are not available. Ray-traced shadows, reflections, and GI are optional passes that bind 1x1 dummies when
off. Upscaling and post-processing consume the scene colour.

## Where to start reading

- Build and gates: [build.md](build.md)
- What is implemented and what is not: [capability-matrix.md](capability-matrix.md)
- Mandatory features and their tests: [acceptance.md](acceptance.md)
- The product requirements behind this layout: `tasks/prd-open-world-engine.md`
